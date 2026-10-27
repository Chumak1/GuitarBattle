"""Progression rules: XP, levels, mastery, statistics and achievements.

Everything here is server-side truth. The client only reports one attempt
(score, accuracy, completion, optional note counters); all multipliers, levels,
personal bests and achievements are derived from the database.

Formula summary (kept in sync with README "Progression"):

* ``XP_REQ(L) = round(50 * L ** 1.5)`` is the cost of the step ``L -> L+1``.
* ``level_for_xp(xp)`` is the largest ``L >= 1`` whose cumulative cost fits ``xp``.
* Attempt XP:
  ``100 * score * difficulty * tempo * content * repeat``, clamped to
  ``[3, 600]``; ``0`` when completion is below 50% or the repeat slot is spent.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import date, datetime, time, timedelta, timezone
from statistics import pstdev
from typing import Any, Iterable, Sequence

from sqlalchemy import func, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.orm import Session

from app.models.game_session import GameSession, Score
from app.models.progression import (
    Achievement,
    DailyChallenge,
    DailyResult,
    SongBestScore,
    UserAchievement,
    XpAward,
)
from app.models.song import Song
from app.models.user import User
from app.services.achievement_catalog import catalog_rows

# ---------------------------------------------------------------------------
# Tuning constants
# ---------------------------------------------------------------------------
#: Attempts below this completion give no attempt XP at all (but still count as
#: a personal best and can still unlock achievements describing the attempt).
MIN_COMPLETION_FOR_XP = 50.0
XP_BASE = 100
XP_MIN_PER_ATTEMPT = 3
XP_MAX_PER_ATTEMPT = 600

DIFFICULTY_FACTORS: dict[int, float] = {1: 0.8, 2: 1.0, 3: 1.2, 4: 1.35, 5: 1.5}
#: Repeat slots for the Nth normal attempt of the same song within one UTC day.
REPEAT_FACTORS: tuple[float, ...] = (1.0, 0.5, 0.25, 0.0)

#: Windows for the profile statistics block.
STATS_WINDOW = 100
STATS_MIN_COMPLETION = 25.0
CONSISTENCY_WINDOW = 20

DAILY_RULES_VERSION = 1
#: How many past days are scanned for the deferred ``daily_first_place`` award.
DAILY_FIRST_PLACE_LOOKBACK_DAYS = 30

MASTERY_TIERS: tuple[tuple[str, float], ...] = (
    ("Novice", 0.0),
    ("Learner", 60.0),
    ("Guitarist", 75.0),
    ("Pro", 87.0),
    ("Master", 95.0),
)
MASTERY_TIER_INDEX = {name: index + 1 for index, (name, _) in enumerate(MASTERY_TIERS)}

#: Genre mastery value required for the "Genre Specialist" achievement.
GENRE_MASTER_THRESHOLD = 87.0


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def clamp(low: float, high: float, value: float) -> float:
    """Clamp ``value`` into ``[low, high]``.

    Argument order follows the specification (``clamp(min, max, value)``) rather
    than the builtin-style ``clamp(value, min, max)``.
    """
    if value < low:
        return low
    if value > high:
        return high
    return value


def _round_half_up(value: float) -> int:
    """Deterministic rounding: ``.5`` always goes up (Python's ``round`` is even)."""
    return int(math.floor(value + 0.5))


def utc_today(now: datetime | None = None) -> date:
    """Current UTC date; the single definition of "today" used by daily logic."""
    if now is None:
        now = datetime.now(timezone.utc)
    elif now.tzinfo is None:
        now = now.replace(tzinfo=timezone.utc)
    return now.astimezone(timezone.utc).date()


def _day_bounds(day: date) -> tuple[datetime, datetime]:
    start = datetime.combine(day, time.min, tzinfo=timezone.utc)
    return start, start + timedelta(days=1)


# ---------------------------------------------------------------------------
# Levels
# ---------------------------------------------------------------------------
def xp_required(level: int) -> int:
    """XP needed to move from ``level`` to ``level + 1``."""
    level = max(1, int(level))
    return _round_half_up(50.0 * (level**1.5))


#: ``_LEVEL_STARTS[L - 1]`` = total XP needed to reach level ``L``.
_LEVEL_STARTS: list[int] = [0]
MAX_LEVEL = 10_000


def _level_start(level: int) -> int:
    level = max(1, min(int(level), MAX_LEVEL))
    while len(_LEVEL_STARTS) < level:
        # The entry being appended describes level ``len(_LEVEL_STARTS) + 1``;
        # the step into it costs ``xp_required(len(_LEVEL_STARTS))``.
        _LEVEL_STARTS.append(_LEVEL_STARTS[-1] + xp_required(len(_LEVEL_STARTS)))
    return _LEVEL_STARTS[level - 1]


def level_for_xp(xp: int) -> int:
    """Largest level ``L >= 1`` whose cumulative requirement fits ``xp``."""
    xp = max(0, int(xp))
    if xp >= _level_start(MAX_LEVEL):
        return MAX_LEVEL
    level = len(_LEVEL_STARTS)
    while _level_start(level + 1) <= xp:
        level += 1
    while level > 1 and _level_start(level) > xp:
        level -= 1
    return level


def xp_to_next_level(xp: int) -> int:
    """XP still missing before the next level."""
    xp = max(0, int(xp))
    return _level_start(level_for_xp(xp) + 1) - xp


def level_progress(xp: int) -> dict[str, int]:
    """Everything the profile needs about the level curve."""
    xp = max(0, int(xp))
    level = level_for_xp(xp)
    start = _level_start(level)
    next_start = _level_start(level + 1)
    return {
        "level": level,
        "xp_total": xp,
        "xp_into_level": xp - start,
        "xp_for_next": next_start - start,
        "xp_remaining": next_start - xp,
    }


# ---------------------------------------------------------------------------
# XP factors
# ---------------------------------------------------------------------------
def difficulty_factor(difficulty: int) -> float:
    difficulty = int(difficulty)
    if difficulty in DIFFICULTY_FACTORS:
        return DIFFICULTY_FACTORS[difficulty]
    if difficulty > 5:
        return min(1.8, 1.5 + 0.05 * (difficulty - 5))
    return 0.6


def tempo_factor(bpm: int) -> float:
    """Faster songs give more XP, but never more than +15%: half of the value of
    a perfect accuracy score, so tempo cannot dominate the formula."""
    return clamp(0.90, 1.15, 0.90 + (int(bpm or 0) - 60) / 120 * 0.25)


def effective_completion(
    completion_percent: float,
    notes_total: int | None = None,
    notes_played: int | None = None,
) -> float:
    """Clamp completion and never let it exceed the fraction of notes played.

    ``notes_played``/``notes_total`` come from the client, so they can only
    *lower* completion. A missing counter keeps the reported completion.
    """
    completion = clamp(0.0, 100.0, float(completion_percent or 0.0))
    if notes_total is not None and notes_played is not None and int(notes_total) > 0:
        played = max(0, min(int(notes_played), int(notes_total)))
        completion = min(completion, 100.0 * played / int(notes_total))
    return clamp(0.0, 100.0, completion)


def content_factor(duration_seconds: int | None, completion_percent: float) -> float:
    """Song length (sub-linear) times how much of it was actually played."""
    base = clamp(0.6, 1.4, math.sqrt(max(int(duration_seconds or 0), 1) / 90))
    return base * (clamp(0.0, 100.0, completion_percent) / 100.0)


def repeat_factor(attempts_today: int) -> float:
    """Repeat slots: 1st attempt of the song today 100%, then 50/25/0%."""
    index = min(max(int(attempts_today), 0), len(REPEAT_FACTORS) - 1)
    return REPEAT_FACTORS[index]


def xp_for_attempt(
    *,
    accuracy_percent: float,
    difficulty: int,
    bpm: int,
    duration_seconds: int,
    completion_percent: float,
    repeat_multiplier: float = 1.0,
    notes_total: int | None = None,
    notes_played: int | None = None,
) -> int:
    """XP for one finished attempt, before achievements.

    ``score_factor`` is accuracy normalised to 0..1. The maximum score of a song
    is not defined server-side (charts are client-side), so accuracy is the only
    fair normalisation available; raw ``score_points`` is never trusted.
    """
    completion = effective_completion(completion_percent, notes_total, notes_played)
    if completion < MIN_COMPLETION_FOR_XP or repeat_multiplier <= 0:
        return 0
    score_factor = clamp(0.0, 1.0, float(accuracy_percent) / 100.0)
    raw = (
        XP_BASE
        * score_factor
        * difficulty_factor(difficulty)
        * tempo_factor(bpm)
        * content_factor(duration_seconds, completion)
        * repeat_multiplier
    )
    return int(clamp(XP_MIN_PER_ATTEMPT, XP_MAX_PER_ATTEMPT, _round_half_up(raw)))


def xp_breakdown(
    *,
    accuracy_percent: float,
    difficulty: int,
    bpm: int,
    duration_seconds: int,
    completion_percent: float,
    repeat_multiplier: float,
    attempts_today: int,
) -> dict[str, Any]:
    """Stored in ``xp_awards.breakdown`` so a support question can be answered."""
    return {
        "score_factor": round(clamp(0.0, 1.0, float(accuracy_percent) / 100.0), 4),
        "difficulty_factor": round(difficulty_factor(difficulty), 4),
        "tempo_factor": round(tempo_factor(bpm), 4),
        "content_factor": round(content_factor(duration_seconds, completion_percent), 4),
        "repeat_factor": repeat_multiplier,
        "attempts_today_before": attempts_today,
        "completion_percent": round(completion_percent, 3),
        "accuracy_percent": round(float(accuracy_percent), 3),
    }


# ---------------------------------------------------------------------------
# Mastery
# ---------------------------------------------------------------------------
def mastery_value(best_accuracy: float, difficulty: int) -> float:
    """Personal best accuracy plus a small bonus for the song's difficulty."""
    return clamp(
        0.0, 100.0, float(best_accuracy or 0.0) + (int(difficulty or 1) - 1) * 2.0
    )


def mastery_tier(value: float) -> tuple[str, float | None]:
    """``(tier, next_threshold)`` for a mastery value in 0..100."""
    value = clamp(0.0, 100.0, float(value or 0.0))
    tier = MASTERY_TIERS[0][0]
    for name, threshold in MASTERY_TIERS:
        if value >= threshold:
            tier = name
    next_threshold: float | None = None
    for _, threshold in MASTERY_TIERS:
        if threshold > value:
            next_threshold = threshold
            break
    return tier, next_threshold


def mastery_payload(best_accuracy: float, difficulty: int) -> dict[str, Any]:
    value = mastery_value(best_accuracy, difficulty)
    tier, next_threshold = mastery_tier(value)
    return {
        "tier": tier,
        "value": round(value, 2),
        "next_threshold": next_threshold,
    }


# ---------------------------------------------------------------------------
# Daily challenge
# ---------------------------------------------------------------------------
def daily_song_for_weekday(db: Session, weekday: int) -> Song | None:
    """The scheduled song of one weekday, published only.

    The slot itself may point at a draft (an administrator prepares the week in
    advance), but players must not receive a song whose assets cannot be
    downloaded yet. An unpublished slot therefore behaves like a free slot.
    """
    from app.services.daily_slots import scheduled_song

    song = scheduled_song(db, weekday)
    if song is None or not song.is_published:
        return None
    return song


def ensure_daily_challenge(db: Session, day: date) -> DailyChallenge | None:
    """Return (and create on first use) the challenge row for ``day``.

    The song comes from the weekday schedule (``daily_slots``). Once a row
    exists it is never rewritten: changing the slot later affects future days
    only, so the official results of the day cannot be mixed up.
    """
    challenge = db.get(DailyChallenge, day)
    if challenge is not None:
        return challenge
    song = daily_song_for_weekday(db, day.weekday())
    if song is None:
        return None
    db.execute(
        pg_insert(DailyChallenge.__table__)
        .values(
            challenge_date=day,
            song_id=song.id,
            rules_version=DAILY_RULES_VERSION,
        )
        .on_conflict_do_nothing(index_elements=["challenge_date"])
    )
    db.flush()
    return db.get(DailyChallenge, day)


def daily_streak(days: Iterable[date], today: date | None = None) -> int:
    """Length of the run of consecutive official days ending at the latest one."""
    today = today or utc_today()
    unique_days = sorted({day for day in days if day <= today}, reverse=True)
    if not unique_days:
        return 0
    streak = 1
    for newer, older in zip(unique_days, unique_days[1:]):
        if (newer - older).days == 1:
            streak += 1
        else:
            break
    return streak


def daily_leaderboard(
    db: Session, day: date, *, limit: int | None = None
) -> list[dict[str, Any]]:
    """Official results of one day, ordered by score then accuracy."""
    stmt = (
        select(
            DailyResult,
            User.username,
            User.display_name,
            Song.slug.label("song_slug"),
            Song.title.label("song_title"),
        )
        .join(User, User.id == DailyResult.user_id)
        .join(DailyChallenge, DailyChallenge.challenge_date == DailyResult.challenge_date)
        .join(Song, Song.id == DailyChallenge.song_id)
        .where(DailyResult.challenge_date == day)
        .order_by(
            DailyResult.score_points.desc(),
            DailyResult.accuracy_percent.desc(),
            DailyResult.created_at.asc(),
        )
    )
    if limit is not None:
        stmt = stmt.limit(limit)
    rows = db.execute(stmt).all()
    return [
        {
            "rank": index + 1,
            "user_id": row.DailyResult.user_id,
            "username": row.username,
            "display_name": row.display_name,
            "song_slug": row.song_slug,
            "song_title": row.song_title,
            "score_points": row.DailyResult.score_points,
            "accuracy_percent": row.DailyResult.accuracy_percent,
            "avg_timing_error_ms": row.DailyResult.avg_timing_error_ms,
            "created_at": row.DailyResult.created_at,
        }
        for index, row in enumerate(rows)
    ]


def _ranked_daily_results():
    """Official results with their place and participant count per day."""
    return (
        select(
            DailyResult.id.label("result_id"),
            DailyResult.user_id.label("user_id"),
            DailyResult.challenge_date.label("challenge_date"),
            DailyResult.score_points.label("score_points"),
            DailyResult.accuracy_percent.label("accuracy_percent"),
            DailyResult.avg_timing_error_ms.label("avg_timing_error_ms"),
            DailyResult.created_at.label("created_at"),
            func.row_number()
            .over(
                partition_by=DailyResult.challenge_date,
                order_by=(
                    DailyResult.score_points.desc(),
                    DailyResult.accuracy_percent.desc(),
                    DailyResult.created_at.asc(),
                    DailyResult.id.asc(),
                ),
            )
            .label("position"),
            func.count()
            .over(partition_by=DailyResult.challenge_date)
            .label("participants"),
        )
        .subquery()
    )


def daily_history(
    db: Session, user_id: Any, *, limit: int = 20, offset: int = 0
) -> tuple[list[dict[str, Any]], int]:
    """Official results of one player, newest day first, with the day rank."""
    ranked = _ranked_daily_results()
    total = int(
        db.scalar(
            select(func.count()).select_from(ranked).where(ranked.c.user_id == user_id)
        )
        or 0
    )
    rows = db.execute(
        select(
            ranked.c.challenge_date,
            ranked.c.score_points,
            ranked.c.accuracy_percent,
            ranked.c.avg_timing_error_ms,
            ranked.c.position,
            ranked.c.participants,
            ranked.c.created_at,
            Song.id.label("song_id"),
            Song.slug.label("song_slug"),
            Song.title.label("song_title"),
            Song.artist.label("artist"),
        )
        .join(
            DailyChallenge,
            DailyChallenge.challenge_date == ranked.c.challenge_date,
        )
        .join(Song, Song.id == DailyChallenge.song_id)
        .where(ranked.c.user_id == user_id)
        .order_by(ranked.c.challenge_date.desc())
        .limit(limit)
        .offset(offset)
    ).all()
    return (
        [
            {
                "date": row.challenge_date,
                "song_id": row.song_id,
                "song_slug": row.song_slug,
                "song_title": row.song_title,
                "artist": row.artist,
                "score_points": row.score_points,
                "accuracy_percent": row.accuracy_percent,
                "avg_timing_error_ms": row.avg_timing_error_ms,
                "rank": int(row.position),
                "participants": int(row.participants),
                "created_at": row.created_at,
            }
            for row in rows
        ],
        total,
    )


def daily_first_place_dates(
    db: Session, user_id: Any, *, before: date, lookback_days: int = DAILY_FIRST_PLACE_LOOKBACK_DAYS
) -> list[date]:
    """Days in ``(before - lookback, before)`` where the player finished first."""
    ranked = (
        select(
            DailyResult.challenge_date.label("challenge_date"),
            DailyResult.user_id.label("user_id"),
            func.row_number()
            .over(
                partition_by=DailyResult.challenge_date,
                order_by=(
                    DailyResult.score_points.desc(),
                    DailyResult.accuracy_percent.desc(),
                    DailyResult.created_at.asc(),
                    DailyResult.id.asc(),
                ),
            )
            .label("position"),
        )
        .subquery()
    )
    stmt = (
        select(ranked.c.challenge_date)
        .where(
            ranked.c.user_id == user_id,
            ranked.c.position == 1,
            ranked.c.challenge_date < before,
            ranked.c.challenge_date >= before - timedelta(days=lookback_days),
        )
        .order_by(ranked.c.challenge_date.desc())
    )
    return list(db.scalars(stmt).all())


# ---------------------------------------------------------------------------
# Snapshot: everything achievement rules read from the database
# ---------------------------------------------------------------------------
@dataclass
class ProgressionSnapshot:
    personal_bests: list[tuple[SongBestScore, Song]]
    pb_songs: int
    pb_genres: int
    total_published_songs: int
    total_published_genres: int
    best_tier_index: int
    best_tier: str
    best_mastery_value: float
    best_genre_mastery: float
    genre_mastery: list[dict[str, Any]]
    daily_dates: list[date]
    daily_streak: int


def load_personal_bests(
    db: Session, user_id: Any, *, limit: int | None = None, offset: int = 0
) -> list[tuple[SongBestScore, Song]]:
    """Personal bests joined with published songs, newest first."""
    stmt = (
        select(SongBestScore, Song)
        .join(Song, Song.id == SongBestScore.song_id)
        .where(SongBestScore.user_id == user_id, Song.is_published.is_(True))
        .order_by(SongBestScore.updated_at.desc(), Song.title.asc())
        .offset(offset)
    )
    if limit is not None:
        stmt = stmt.limit(limit)
    return [(row[0], row[1]) for row in db.execute(stmt).all()]


def genre_mastery_rows(
    personal_bests: Sequence[tuple[SongBestScore, Song]]
) -> list[dict[str, Any]]:
    """Average mastery of the personal bests of each genre (no invented genres)."""
    buckets: dict[str, list[float]] = {}
    for best, song in personal_bests:
        buckets.setdefault(song.genre or "Other", []).append(
            mastery_value(best.best_accuracy, song.difficulty)
        )
    rows = [
        {
            "genre": genre,
            "value": round(sum(values) / len(values), 2),
            "songs": len(values),
        }
        for genre, values in buckets.items()
    ]
    rows.sort(key=lambda item: (-item["value"], -item["songs"], item["genre"]))
    return rows


def build_snapshot(db: Session, user_id: Any) -> ProgressionSnapshot:
    personal_bests = load_personal_bests(db, user_id)
    total_songs = int(
        db.scalar(select(func.count(Song.id)).where(Song.is_published.is_(True))) or 0
    )
    total_genres = int(
        db.scalar(
            select(func.count(func.distinct(Song.genre))).where(
                Song.is_published.is_(True)
            )
        )
        or 0
    )

    best_tier_index = 0
    best_tier = MASTERY_TIERS[0][0]
    best_value = 0.0
    genres: set[str] = set()
    for best, song in personal_bests:
        genres.add(song.genre or "Other")
        value = mastery_value(best.best_accuracy, song.difficulty)
        tier, _ = mastery_tier(value)
        tier_index = MASTERY_TIER_INDEX[tier]
        if tier_index > best_tier_index:
            best_tier_index = tier_index
            best_tier = tier
        if value > best_value:
            best_value = value

    genre_rows = genre_mastery_rows(personal_bests)
    daily_dates = list(
        db.scalars(
            select(DailyResult.challenge_date)
            .where(DailyResult.user_id == user_id)
            .order_by(DailyResult.challenge_date.desc())
        ).all()
    )
    return ProgressionSnapshot(
        personal_bests=personal_bests,
        pb_songs=len(personal_bests),
        pb_genres=len(genres),
        total_published_songs=total_songs,
        total_published_genres=total_genres,
        best_tier_index=best_tier_index,
        best_tier=best_tier,
        best_mastery_value=best_value,
        best_genre_mastery=genre_rows[0]["value"] if genre_rows else 0.0,
        genre_mastery=genre_rows,
        daily_dates=daily_dates,
        daily_streak=daily_streak(daily_dates),
    )


# ---------------------------------------------------------------------------
# Statistics block of the profile
# ---------------------------------------------------------------------------
def player_stats(db: Session, user_id: Any) -> dict[str, Any]:
    """Derived play-style numbers over the last 100 meaningful attempts."""
    rows = db.execute(
        select(
            Score.accuracy_percent,
            Score.avg_timing_error_ms,
            GameSession.speed_percent,
            GameSession.song_id,
            Song.genre,
        )
        .join(GameSession, GameSession.id == Score.session_id)
        .join(Song, Song.id == GameSession.song_id)
        .where(
            GameSession.user_id == user_id,
            GameSession.status == "finished",
            Score.completion_percent > STATS_MIN_COMPLETION,
        )
        .order_by(Score.created_at.desc())
        .limit(STATS_WINDOW)
    ).all()

    total_published = int(
        db.scalar(select(func.count(Song.id)).where(Song.is_published.is_(True))) or 0
    )
    total_genres = int(
        db.scalar(
            select(func.count(func.distinct(Song.genre))).where(
                Song.is_published.is_(True)
            )
        )
        or 0
    )

    samples = len(rows)
    if samples == 0:
        return {
            "accuracy": 0.0,
            "timing": None,
            "speed": 0.0,
            "consistency": 0.0,
            "versatility": 0.0,
            "samples": 0,
        }

    accuracies = [float(row.accuracy_percent or 0.0) for row in rows]
    speed_values = [float(row.speed_percent or 0.0) for row in rows]
    timing_values = [
        float(row.avg_timing_error_ms)
        for row in rows
        if row.avg_timing_error_ms is not None
    ]

    accuracy = sum(accuracies) / samples
    timing = (
        100.0 - clamp(0.0, 100.0, (sum(timing_values) / len(timing_values)) / 1.5)
        if timing_values
        else None
    )
    speed = clamp(0.0, 100.0, (sum(speed_values) / samples - 50.0) / 70.0 * 100.0)

    recent = accuracies[:CONSISTENCY_WINDOW]
    consistency = 0.0 if len(recent) < 2 else 100.0 - min(100.0, pstdev(recent))

    distinct_songs = len({row.song_id for row in rows})
    distinct_genres = len({row.genre or "Other" for row in rows})
    song_ratio = distinct_songs / total_published if total_published else 0.0
    genre_ratio = distinct_genres / total_genres if total_genres else 0.0
    versatility = clamp(0.0, 100.0, 100.0 * (0.6 * song_ratio + 0.4 * genre_ratio))

    return {
        "accuracy": round(accuracy, 2),
        "timing": None if timing is None else round(timing, 2),
        "speed": round(speed, 2),
        "consistency": round(consistency, 2),
        "versatility": round(versatility, 2),
        "samples": samples,
    }


# ---------------------------------------------------------------------------
# Personal best bookkeeping
# ---------------------------------------------------------------------------
def update_best_score(
    db: Session, *, user_id: Any, song_id: Any, score: Score
) -> tuple[bool, float | None, int | None]:
    """Create/refresh ``song_best_scores`` for one attempt.

    Returns ``(updated, previous_accuracy, previous_score)``. ``attempts`` grows
    on every finished attempt, the best values only on a real improvement
    (higher score, or equal score with better accuracy).
    """
    best = db.scalar(
        select(SongBestScore)
        .where(SongBestScore.user_id == user_id, SongBestScore.song_id == song_id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    created = False
    if best is None:
        inserted = db.execute(
            pg_insert(SongBestScore.__table__)
            .values(
                user_id=user_id,
                song_id=song_id,
                best_score=score.score_points,
                best_accuracy=score.accuracy_percent,
                best_max_combo=score.max_combo,
                best_avg_timing_error_ms=score.avg_timing_error_ms,
                attempts=1,
            )
            .on_conflict_do_nothing(index_elements=["user_id", "song_id"])
        ).rowcount
        db.flush()
        best = db.scalar(
            select(SongBestScore)
            .where(SongBestScore.user_id == user_id, SongBestScore.song_id == song_id)
            .with_for_update()
            .execution_options(populate_existing=True)
        )
        if best is None:  # pragma: no cover - defensive, the insert must land
            raise RuntimeError("Could not store the personal best")
        created = bool(inserted)
        if created:
            return True, None, None

    previous_accuracy = float(best.best_accuracy)
    previous_score = int(best.best_score)
    best.attempts = int(best.attempts or 0) + 1

    improved = score.score_points > previous_score or (
        score.score_points == previous_score
        and float(score.accuracy_percent) > previous_accuracy
    )
    if improved:
        best.best_score = int(score.score_points)
        best.best_accuracy = float(score.accuracy_percent)
        best.best_max_combo = int(score.max_combo)
        if score.avg_timing_error_ms is not None:
            best.best_avg_timing_error_ms = float(score.avg_timing_error_ms)
    db.flush()
    return improved, previous_accuracy, previous_score


def insert_xp_award(
    db: Session,
    *,
    user_id: Any,
    session_id: Any,
    amount: int,
    breakdown: dict[str, Any] | None,
) -> bool:
    """Insert the XP ledger row; ``False`` when the session already had one."""
    inserted = db.execute(
        pg_insert(XpAward.__table__)
        .values(
            user_id=user_id,
            session_id=session_id,
            amount=amount,
            breakdown=breakdown,
        )
        .on_conflict_do_nothing(index_elements=["session_id"])
    ).rowcount
    return bool(inserted)


# ---------------------------------------------------------------------------
# Achievements
# ---------------------------------------------------------------------------
def genre_thresholds(
    catalog: Sequence[Achievement], total_published_genres: int
) -> dict[str, int]:
    """Effective threshold per ``genres_*`` code.

    The catalog may ask for more genres than exist. The effective threshold is
    ``min(goal, published genres)``, and when two achievements collapse onto the
    same effective threshold only the first one stays attainable — the rest are
    intentionally locked instead of being handed out for free.
    """
    thresholds: dict[str, int] = {}
    seen: set[int] = set()
    for achievement in catalog:
        if not achievement.code.startswith("genres_") or not achievement.goal:
            continue
        effective = min(int(achievement.goal), total_published_genres)
        if effective <= 0 or effective in seen:
            continue
        seen.add(effective)
        thresholds[achievement.code] = effective
    return thresholds


def genre_achievement_note(
    code: str, goal: int | None, total_published_genres: int
) -> str | None:
    """Suffix shown when the catalog cannot satisfy a genre goal."""
    if not code.startswith("genres_") or not goal:
        return None
    if total_published_genres >= int(goal):
        return None
    noun = "genre" if total_published_genres == 1 else "genres"
    return (
        f" Unavailable: catalogue has {total_published_genres} {noun} "
        f"of {int(goal)} required."
    )


@dataclass(frozen=True)
class _AttemptFacts:
    """Values of the attempt being submitted (already clamped server-side)."""

    completion_percent: float
    accuracy_percent: float
    miss_count: int
    avg_timing_error_ms: float | None
    difficulty: int
    is_daily: bool
    personal_best_updated: bool
    previous_best_accuracy: float | None
    qualifying_attempts: int
    daily_result_count: int

    @property
    def qualifies(self) -> bool:
        """Performance achievements require a real, mostly finished attempt."""
        return self.completion_percent >= MIN_COMPLETION_FOR_XP

    @property
    def improvement_pp(self) -> float:
        if self.previous_best_accuracy is None:
            return 0.0
        return self.accuracy_percent - self.previous_best_accuracy


def evaluate_achievement(
    achievement: Achievement,
    *,
    snapshot: ProgressionSnapshot,
    attempt: _AttemptFacts,
    thresholds: dict[str, int],
) -> tuple[bool, int]:
    """``(unlocked, progress)`` for one catalog row. Pure function."""
    code = achievement.code
    goal = int(achievement.goal or 0)

    if code == "first_notes":
        unlocked = attempt.qualifies and attempt.qualifying_attempts <= 1
        return unlocked, 1 if unlocked else 0

    if code == "first_personal_best":
        unlocked = attempt.qualifies and attempt.personal_best_updated
        return unlocked, 1 if attempt.personal_best_updated else 0

    if code.startswith("songs_"):
        return (
            attempt.qualifies and goal > 0 and snapshot.pb_songs >= goal
        ), min(snapshot.pb_songs, goal or snapshot.pb_songs)

    if code.startswith("accuracy_"):
        progress = min(goal, int(_round_half_up(attempt.accuracy_percent)))
        return (attempt.qualifies and attempt.accuracy_percent >= goal), progress

    if code == "clean_run":
        unlocked = attempt.qualifies and attempt.miss_count == 0
        return unlocked, 1 if unlocked else 0

    if code == "timing_40ms":
        metric = attempt.avg_timing_error_ms
        if metric is None:
            # No metric reported: the achievement stays locked instead of being
            # granted on missing data.
            return False, 0
        unlocked = attempt.qualifies and metric <= goal
        return unlocked, max(0, min(goal, int(_round_half_up(goal - metric))))

    if code.startswith("improve_"):
        improvement = attempt.improvement_pp
        progress = max(0, min(goal, int(_round_half_up(improvement))))
        unlocked = (
            attempt.qualifies
            and attempt.personal_best_updated
            and attempt.previous_best_accuracy is not None
            and improvement >= goal
        )
        return unlocked, progress

    if code.startswith("genres_"):
        effective = thresholds.get(code)
        progress = min(snapshot.pb_genres, goal or snapshot.pb_genres)
        if effective is None:
            return False, progress
        return attempt.qualifies and snapshot.pb_genres >= effective, progress

    if code == "genre_master":
        unlocked = (
            attempt.qualifies
            and snapshot.best_genre_mastery >= GENRE_MASTER_THRESHOLD
        )
        return unlocked, int(_round_half_up(snapshot.best_genre_mastery))

    if code.startswith("mastery_"):
        progress = snapshot.best_tier_index
        return (attempt.qualifies and goal > 0 and progress >= goal), progress

    if code == "hard_80":
        progress = min(goal, int(_round_half_up(attempt.accuracy_percent)))
        unlocked = (
            attempt.qualifies
            and attempt.difficulty >= 4
            and attempt.accuracy_percent >= goal
        )
        return unlocked, progress

    if code == "daily_first":
        unlocked = attempt.daily_result_count == 1
        return unlocked, 1 if unlocked else 0

    if code.startswith("daily_streak_"):
        progress = snapshot.daily_streak
        return (goal > 0 and progress >= goal), progress

    if code == "daily_score_70":
        progress = min(goal, int(_round_half_up(attempt.accuracy_percent)))
        unlocked = (
            attempt.is_daily
            and attempt.qualifies
            and attempt.accuracy_percent >= goal
        )
        return unlocked, progress

    # daily_first_place is awarded by ``award_deferred_daily_first_place``
    # because it can only be decided after the day has ended.
    return False, 0


def achievement_progress(
    db: Session, user_id: Any, snapshot: ProgressionSnapshot
) -> dict[str, int]:
    """Current progress of every catalog row, for the achievements screen."""
    catalog = catalog_rows(db)
    thresholds = genre_thresholds(catalog, snapshot.total_published_genres)
    empty_attempt = _AttemptFacts(
        completion_percent=0.0,
        accuracy_percent=0.0,
        miss_count=1,
        avg_timing_error_ms=None,
        difficulty=1,
        is_daily=False,
        personal_best_updated=False,
        previous_best_accuracy=None,
        qualifying_attempts=2,
        daily_result_count=0,
    )
    return {
        achievement.code: evaluate_achievement(
            achievement,
            snapshot=snapshot,
            attempt=empty_attempt,
            thresholds=thresholds,
        )[1]
        for achievement in catalog
    }


def _award_achievement(
    db: Session, *, user_id: Any, achievement: Achievement, progress: int, now: datetime
) -> bool:
    inserted = db.execute(
        pg_insert(UserAchievement.__table__)
        .values(
            user_id=user_id,
            achievement_code=achievement.code,
            progress=progress,
            awarded_at=now,
        )
        .on_conflict_do_nothing(index_elements=["user_id", "achievement_code"])
    ).rowcount
    return bool(inserted)


def award_deferred_daily_first_place(db: Session, user: User) -> list[dict[str, Any]]:
    """Grant "Daily Champion" once the winning day is over.

    There is no background scheduler in this deployment, so the award happens on
    the next progression/achievements/leaderboard read. It is idempotent.
    """
    achievement = db.scalar(
        select(Achievement).where(
            Achievement.code == "daily_first_place", Achievement.is_active.is_(True)
        )
    )
    if achievement is None:
        return []
    owned = db.scalar(
        select(UserAchievement.id).where(
            UserAchievement.user_id == user.id,
            UserAchievement.achievement_code == achievement.code,
        )
    )
    if owned is not None:
        return []

    today = utc_today()
    first_places = daily_first_place_dates(db, user.id, before=today)
    if not first_places:
        return []

    now = datetime.now(timezone.utc)
    if not _award_achievement(
        db,
        user_id=user.id,
        achievement=achievement,
        progress=len(first_places),
        now=now,
    ):
        db.rollback()
        return []

    db.refresh(user, with_for_update=True)
    user.xp_total = int(user.xp_total or 0) + int(achievement.xp_bonus)
    user.level = level_for_xp(user.xp_total)
    db.commit()
    return [
        {
            "code": achievement.code,
            "title": achievement.title,
            "xp_bonus": int(achievement.xp_bonus),
        }
    ]


# ---------------------------------------------------------------------------
# Finish pipeline
# ---------------------------------------------------------------------------
@dataclass
class FinishOutcome:
    xp_earned: int
    xp_total: int
    level: int
    level_up: bool
    xp_to_next: int
    xp_remaining: int
    personal_best_updated: bool
    mastery: dict[str, Any]
    achievements_awarded: list[dict[str, Any]]


def attempts_today_for_song(
    db: Session, *, user_id: Any, song_id: Any, exclude_session_id: Any, now: datetime
) -> int:
    """Finished *normal* attempts of this song today, before the current one."""
    start, end = _day_bounds(now.astimezone(timezone.utc).date())
    return int(
        db.scalar(
            select(func.count(GameSession.id))
            .join(Score, Score.session_id == GameSession.id)
            .where(
                GameSession.user_id == user_id,
                GameSession.song_id == song_id,
                GameSession.mode == "normal",
                GameSession.status == "finished",
                GameSession.id != exclude_session_id,
                GameSession.started_at >= start,
                GameSession.started_at < end,
            )
        )
        or 0
    )


def apply_attempt_progression(
    db: Session,
    *,
    user: User,
    session: GameSession,
    score: Score,
    song: Song,
    daily_date: date | None = None,
    now: datetime | None = None,
) -> FinishOutcome:
    """Persist PB, XP, daily result and achievements for one finished attempt.

    Must run inside the caller's transaction, after the session row was locked
    with ``SELECT ... FOR UPDATE`` and after the score was added.
    """
    now = now or datetime.now(timezone.utc)
    level_before = level_for_xp(int(user.xp_total or 0))

    completion = effective_completion(
        score.completion_percent, score.notes_total, score.notes_played
    )
    score.completion_percent = completion

    attempts_today = attempts_today_for_song(
        db,
        user_id=user.id,
        song_id=song.id,
        exclude_session_id=session.id,
        now=now,
    )
    multiplier = repeat_factor(attempts_today)

    pb_updated, previous_accuracy, _ = update_best_score(
        db, user_id=user.id, song_id=song.id, score=score
    )

    xp_earned = xp_for_attempt(
        accuracy_percent=float(score.accuracy_percent),
        difficulty=song.difficulty,
        bpm=song.bpm,
        duration_seconds=song.duration_seconds,
        completion_percent=completion,
        repeat_multiplier=multiplier,
        notes_total=score.notes_total,
        notes_played=score.notes_played,
    )
    if not insert_xp_award(
        db,
        user_id=user.id,
        session_id=session.id,
        amount=xp_earned,
        breakdown=xp_breakdown(
            accuracy_percent=float(score.accuracy_percent),
            difficulty=song.difficulty,
            bpm=song.bpm,
            duration_seconds=song.duration_seconds,
            completion_percent=completion,
            repeat_multiplier=multiplier,
            attempts_today=attempts_today,
        ),
    ):
        # A concurrent request already wrote the ledger row for this session.
        xp_earned = 0

    if daily_date is not None:
        db.add(
            DailyResult(
                challenge_date=daily_date,
                user_id=user.id,
                session_id=session.id,
                score_points=int(score.score_points),
                accuracy_percent=float(score.accuracy_percent),
                avg_timing_error_ms=score.avg_timing_error_ms,
            )
        )
        db.flush()

    snapshot = build_snapshot(db, user.id)
    qualifying_attempts = int(
        db.scalar(
            select(func.count(Score.id))
            .join(GameSession, GameSession.id == Score.session_id)
            .where(
                GameSession.user_id == user.id,
                GameSession.status == "finished",
                Score.completion_percent >= MIN_COMPLETION_FOR_XP,
            )
        )
        or 0
    )
    facts = _AttemptFacts(
        completion_percent=completion,
        accuracy_percent=float(score.accuracy_percent),
        miss_count=int(score.miss_count),
        avg_timing_error_ms=score.avg_timing_error_ms,
        difficulty=int(song.difficulty),
        is_daily=daily_date is not None,
        personal_best_updated=pb_updated,
        previous_best_accuracy=previous_accuracy,
        qualifying_attempts=qualifying_attempts,
        daily_result_count=len(snapshot.daily_dates),
    )

    catalog = catalog_rows(db)
    thresholds = genre_thresholds(catalog, snapshot.total_published_genres)
    owned = set(
        db.scalars(
            select(UserAchievement.achievement_code).where(
                UserAchievement.user_id == user.id
            )
        ).all()
    )
    awarded: list[dict[str, Any]] = []
    bonus = 0
    for achievement in catalog:
        if achievement.code in owned:
            continue
        unlocked, progress = evaluate_achievement(
            achievement, snapshot=snapshot, attempt=facts, thresholds=thresholds
        )
        if not unlocked:
            continue
        if _award_achievement(
            db,
            user_id=user.id,
            achievement=achievement,
            progress=progress,
            now=now,
        ):
            awarded.append(
                {
                    "code": achievement.code,
                    "title": achievement.title,
                    "xp_bonus": int(achievement.xp_bonus),
                }
            )
            bonus += int(achievement.xp_bonus)

    total = int(user.xp_total or 0) + xp_earned + bonus
    user.xp_total = total
    user.level = level_for_xp(total)
    db.flush()

    return FinishOutcome(
        xp_earned=xp_earned,
        xp_total=total,
        level=user.level,
        level_up=user.level > level_before,
        xp_to_next=xp_to_next_level(total),
        xp_remaining=xp_to_next_level(total),
        personal_best_updated=pb_updated,
        mastery=mastery_payload(
            _best_accuracy_after(db, user.id, song.id) or 0.0, song.difficulty
        ),
        achievements_awarded=awarded,
    )


def _best_accuracy_after(db: Session, user_id: Any, song_id: Any) -> float | None:
    return db.scalar(
        select(SongBestScore.best_accuracy).where(
            SongBestScore.user_id == user_id, SongBestScore.song_id == song_id
        )
    )
