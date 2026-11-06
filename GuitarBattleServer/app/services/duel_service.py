"""Server-authoritative Guitar Duels orchestration.

The service owns the match state machine, the queue, chart freezing, note
resolution, XP and Elo. API handlers only translate HTTP to calls here; the
WebSocket layer broadcasts the returned snapshots and exchange messages.

Everything is written through the caller's SQLAlchemy session. Mutation entry
points lock the ``duel_matches`` row first, which serialises ``ready``,
``events``, ``finish`` and ``leave`` for one match without a distributed lock.
"""
from __future__ import annotations

import secrets
from datetime import datetime, timedelta, timezone
from typing import Any, Iterable, Sequence
from uuid import UUID, uuid4

from sqlalchemy import case, func, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.orm import Session, aliased

from app.models.duel import DuelEvent, DuelMatch, DuelParticipant
from app.models.song import Song, SongFile
from app.models.song_midi import SongMidiAnalysis, SongMidiNote
from app.models.user import User
from app.services import duel_rules
from app.services.progression import level_for_xp

# ---------------------------------------------------------------------------
# Tuning
# ---------------------------------------------------------------------------
#: How long an online wait stays joinable before the client is told there is
#: no opponent and should offer a bot match.
QUEUE_TIMEOUT_SECONDS = 30
QUEUE_TTL = timedelta(seconds=QUEUE_TIMEOUT_SECONDS)

#: Rating window of the simple queue. It widens while the player waits, so a
#: small player pool still produces matches without ever mixing extremes at
#: the first second.
QUEUE_RATING_WINDOW = 200
QUEUE_RATING_WINDOW_GROWTH_PER_SECOND = 20
QUEUE_RATING_WINDOW_MAX = 1_000

#: Time between ``ready`` and the first chart note. The server writes this into
#: ``started_at``; clients count down locally from the snapshot.
COUNTDOWN_MS = 3_000

#: Accepted lateness of a reported note around its chart position.
EVENT_WINDOW_BEFORE_MS = 1_500
EVENT_WINDOW_AFTER_MS = 4_000

#: Fallback timing for a song without an analysed MIDI: one note each 500 ms.
#: The windows are deliberately wider, because the client may use a slightly
#: different chart length for a legacy song.
SYNTHETIC_NOTE_INTERVAL_MS = 500
SYNTHETIC_WINDOW_BEFORE_MS = 3_000
SYNTHETIC_WINDOW_AFTER_MS = 15_000

MAX_EVENTS_PER_BATCH = 100
MAX_NOTE_COUNT = 5_000

QUALITY_COUNTER = {
    "perfect": "perfect_count",
    "great": "great_count",
    "good": "good_count",
    "miss": "miss_count",
}


class DuelError(Exception):
    """A domain error with an HTTP status and a stable machine code."""

    def __init__(self, status_code: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.code = code
        self.message = message

    def detail(self) -> dict[str, str]:
        return {"code": self.code, "message": self.message}


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def _now() -> datetime:
    return datetime.now(timezone.utc)


def _aware(value: datetime | None) -> datetime | None:
    if value is None:
        return None
    return value if value.tzinfo is not None else value.replace(tzinfo=timezone.utc)


def _ms_since(started_at: datetime | None, now: datetime) -> int:
    started = _aware(started_at)
    if started is None:
        return 0
    return int((now - started).total_seconds() * 1000)


def song_info(song: Song) -> dict[str, Any]:
    return {
        "id": song.id,
        "slug": song.slug,
        "title": song.title,
        "artist": song.artist,
        "bpm": int(song.bpm),
        "difficulty": int(song.difficulty),
        "duration_seconds": int(song.duration_seconds or 0),
        "time_signature_numerator": int(song.time_signature_numerator or 4),
        "time_signature_denominator": int(song.time_signature_denominator or 4),
    }


# ---------------------------------------------------------------------------
# Chart snapshot
# ---------------------------------------------------------------------------
def load_chart(db: Session, song: Song) -> tuple[int, list[int] | None]:
    """Freeze the chart of a song into ``(note_count, note_times_ms)``.

    The active MIDI analysis is the authoritative chart. Its note offsets are
    copied into the match, so re-uploading or re-analysing the song later can
    never move the notes of a match that is already running.

    A song without an analysed MIDI (legacy catalogue entries) gets a synthetic
    chart derived from its duration; ``None`` tells the validator to use the
    wider synthetic timing windows.
    """
    midi_file = db.scalar(
        select(SongFile)
        .where(
            SongFile.song_id == song.id,
            SongFile.role == "midi",
            SongFile.is_active.is_(True),
        )
        .order_by(SongFile.created_at.desc(), SongFile.id.desc())
        .limit(1)
    )
    if midi_file is not None:
        analysis = db.scalar(
            select(SongMidiAnalysis).where(
                SongMidiAnalysis.song_file_id == midi_file.id
            )
        )
        if analysis is not None and int(analysis.note_count or 0) > 0:
            starts = db.scalars(
                select(SongMidiNote.start_seconds)
                .where(SongMidiNote.song_file_id == midi_file.id)
                .order_by(
                    SongMidiNote.start_tick,
                    SongMidiNote.track_index,
                    SongMidiNote.channel,
                    SongMidiNote.midi_note,
                    SongMidiNote.id,
                )
            ).all()
            if starts:
                times = [
                    max(0, int(round(float(value) * 1000.0)))
                    for value in starts[:MAX_NOTE_COUNT]
                ]
                return len(times), times

    duration = max(15, min(600, int(song.duration_seconds or 60)))
    count = max(1, min(MAX_NOTE_COUNT, duration * 2))
    return count, None


# ---------------------------------------------------------------------------
# Rating helpers
# ---------------------------------------------------------------------------
def current_rating(db: Session, user_id: UUID) -> int:
    """The latest online rating of a player, or the start rating."""
    value = db.scalar(
        select(DuelParticipant.rating_after)
        .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
        .where(
            DuelParticipant.user_id == user_id,
            DuelParticipant.is_bot.is_(False),
            DuelParticipant.rating_after.is_not(None),
            DuelMatch.mode == "online",
            DuelMatch.status == "finished",
        )
        .order_by(
            DuelMatch.finished_at.desc().nullslast(),
            DuelMatch.id.desc(),
        )
        .limit(1)
    )
    return int(value) if value is not None else duel_rules.ELO_START_RATING


def _latest_rating_subquery():
    ranked = (
        select(
            DuelParticipant.user_id.label("user_id"),
            DuelParticipant.rating_after.label("rating"),
            func.row_number()
            .over(
                partition_by=DuelParticipant.user_id,
                order_by=(
                    DuelMatch.finished_at.desc().nullslast(),
                    DuelMatch.id.desc(),
                ),
            )
            .label("rn"),
        )
        .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
        .where(
            DuelParticipant.is_bot.is_(False),
            DuelParticipant.user_id.is_not(None),
            DuelParticipant.rating_after.is_not(None),
            DuelMatch.mode == "online",
            DuelMatch.status == "finished",
        )
        .subquery()
    )
    return select(ranked.c.user_id, ranked.c.rating).where(ranked.c.rn == 1).subquery()


# ---------------------------------------------------------------------------
# Participant / snapshot helpers
# ---------------------------------------------------------------------------
def _participants(db: Session, match_id: UUID) -> dict[str, DuelParticipant]:
    rows = db.scalars(
        select(DuelParticipant).where(DuelParticipant.match_id == match_id)
    ).all()
    return {row.side: row for row in rows}


def _participant(
    db: Session, match_id: UUID, user_id: UUID
) -> DuelParticipant | None:
    return db.scalar(
        select(DuelParticipant).where(
            DuelParticipant.match_id == match_id,
            DuelParticipant.user_id == user_id,
        )
    )


def _users_for(db: Session, participants: Iterable[DuelParticipant]) -> dict[UUID, User]:
    ids = [p.user_id for p in participants if p.user_id is not None]
    if not ids:
        return {}
    return {
        user.id: user
        for user in db.scalars(select(User).where(User.id.in_(ids))).all()
    }


def _participant_payload(
    participant: DuelParticipant,
    user: User | None,
    *,
    connected_sides: set[str] | None = None,
) -> dict[str, Any]:
    before = participant.rating_before
    after = participant.rating_after
    delta = (
        int(after) - int(before)
        if before is not None and after is not None
        else None
    )
    stats = duel_rules.stats_payload(participant)
    return {
        "side": participant.side,
        "user_id": participant.user_id,
        "username": user.username if user is not None else None,
        "display_name": user.display_name if user is not None else None,
        "is_bot": bool(participant.is_bot),
        "bot_name": participant.bot_name,
        "bot_avatar": participant.bot_avatar,
        "ready": participant.ready_at is not None,
        "connected": bool(connected_sides and participant.side in connected_sides),
        "left": participant.left_at is not None,
        "hp": round(float(participant.hp_final), 2),
        "perfect": stats["perfect"],
        "great": stats["great"],
        "good": stats["good"],
        "miss": stats["miss"],
        "notes": stats["notes"],
        "accuracy": stats["accuracy"],
        "damage_dealt": round(float(participant.damage_dealt), 2),
        "damage_taken": round(float(participant.damage_taken), 2),
        "xp_awarded": int(participant.xp_awarded or 0),
        "rating_before": before,
        "rating_after": after,
        "rating_delta": delta,
        "result": participant.result,
    }


def snapshot(
    db: Session,
    match: DuelMatch,
    *,
    your_user_id: UUID | None = None,
    connected_sides: set[str] | None = None,
    now: datetime | None = None,
) -> dict[str, Any]:
    """Authoritative public state of a match."""
    now = now or _now()
    song = db.get(Song, match.song_id)
    if song is None:  # pragma: no cover - enforced by the FK
        raise DuelError(404, "song_not_found", "Song not found")

    participants = _participants(db, match.id)
    users = _users_for(db, participants.values())
    ordered = sorted(
        participants.values(),
        key=lambda row: 0 if row.side == "player" else 1,
    )
    payloads = [
        _participant_payload(
            row, users.get(row.user_id) if row.user_id else None,
            connected_sides=connected_sides,
        )
        for row in ordered
    ]

    your_side = None
    if your_user_id is not None:
        for row in ordered:
            if row.user_id == your_user_id:
                your_side = row.side
                break

    winner_side = None
    for row in ordered:
        if row.result == "win":
            winner_side = row.side
            break

    starts_in_ms = 0
    started_at = _aware(match.started_at)
    if started_at is not None:
        starts_in_ms = int((started_at - now).total_seconds() * 1000)

    waiting_seconds = 0
    if match.status == "waiting":
        created_at = _aware(match.created_at) or now
        waiting_seconds = max(0, int((now - created_at).total_seconds()))

    return {
        "match_id": match.id,
        "mode": match.mode,
        "status": match.status,
        "rules_version": int(match.rules_version),
        "seed": int(match.seed),
        "bot_level": match.bot_level,
        "song": song_info(song),
        "note_count": int(match.note_count),
        "next_note_index": int(match.next_note_index),
        "your_side": your_side,
        "starts_in_ms": starts_in_ms,
        "server_time_ms": _ms_since(match.started_at, now),
        "waiting_seconds": waiting_seconds,
        "waiting_expired": (
            match.mode == "online"
            and match.status == "waiting"
            and waiting_seconds >= QUEUE_TIMEOUT_SECONDS
        ),
        "winner_user_id": match.winner_user_id,
        "winner_side": winner_side,
        "created_at": match.created_at,
        "started_at": match.started_at,
        "finished_at": match.finished_at,
        "participants": payloads,
    }


# ---------------------------------------------------------------------------
# Match creation and queue
# ---------------------------------------------------------------------------
def _find_waiting_opponent(
    db: Session, *, user: User, song: Song, rating: int
) -> tuple[DuelMatch, DuelParticipant] | None:
    """Find and join a waiting online match of the same song.

    ``FOR UPDATE OF duel_matches SKIP LOCKED`` lets two simultaneous players
    pick two different rooms instead of racing for the same one.
    """
    now = _now()
    cutoff = now - QUEUE_TTL
    guard = aliased(DuelParticipant)
    opponent_exists = (
        select(guard.id)
        .where(
            guard.match_id == DuelMatch.id,
            guard.side == "opponent",
        )
        .exists()
    )
    candidates = db.scalars(
        select(DuelMatch)
        .join(DuelParticipant, DuelParticipant.match_id == DuelMatch.id)
        .where(
            DuelMatch.status == "waiting",
            DuelMatch.mode == "online",
            DuelMatch.song_id == song.id,
            DuelMatch.created_at >= cutoff,
            DuelParticipant.side == "player",
            DuelParticipant.user_id.is_not(None),
            DuelParticipant.user_id != user.id,
            ~opponent_exists,
        )
        .order_by(DuelMatch.created_at.asc(), DuelMatch.id.asc())
        .limit(20)
        .with_for_update(of=DuelMatch, skip_locked=True)
    ).all()

    for candidate in candidates:
        creator = db.scalar(
            select(DuelParticipant).where(
                DuelParticipant.match_id == candidate.id,
                DuelParticipant.side == "player",
            )
        )
        if creator is None or creator.user_id is None:
            continue
        other_rating = (
            int(creator.rating_before)
            if creator.rating_before is not None
            else current_rating(db, creator.user_id)
        )
        created_at = _aware(candidate.created_at) or now
        waited = max(0.0, (now - created_at).total_seconds())
        window = min(
            QUEUE_RATING_WINDOW_MAX,
            QUEUE_RATING_WINDOW
            + int(waited * QUEUE_RATING_WINDOW_GROWTH_PER_SECOND),
        )
        if abs(other_rating - rating) > window:
            continue

        joiner = DuelParticipant(
            match_id=candidate.id,
            user_id=user.id,
            side="opponent",
            is_bot=False,
            rating_before=rating,
        )
        db.add(joiner)
        db.flush()
        return candidate, joiner

    return None


def create_match(
    db: Session, *, user: User, song_id: UUID, mode: str, bot_level: str | None
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Create a bot match or enqueue/join an online match."""
    song = db.scalar(
        select(Song).where(Song.id == song_id, Song.archived_at.is_(None))
    )
    if song is None:
        raise DuelError(404, "song_not_found", "Song not found")
    if not song.is_published:
        raise DuelError(422, "song_not_published", "Song is not published")

    note_count, note_times = load_chart(db, song)
    seed = secrets.randbits(32)
    messages: list[dict[str, Any]] = []

    if mode == "bot":
        profile = duel_rules.bot_profile(bot_level or "easy")
        now = _now()
        match = DuelMatch(
            song_id=song.id,
            mode="bot",
            bot_level=profile.level,
            status="waiting",
            rules_version=duel_rules.RULES_VERSION,
            seed=seed,
            note_count=note_count,
            note_times_ms=note_times,
            created_at=now,
            updated_at=now,
        )
        db.add(match)
        db.flush()
        db.add(
            DuelParticipant(
                match_id=match.id,
                user_id=user.id,
                side="player",
                is_bot=False,
            )
        )
        db.add(
            DuelParticipant(
                match_id=match.id,
                user_id=None,
                side="opponent",
                is_bot=True,
                bot_name=profile.name,
                bot_avatar=profile.avatar,
                ready_at=_now(),
            )
        )
        db.flush()
    else:
        rating = current_rating(db, user.id)
        paired = _find_waiting_opponent(db, user=user, song=song, rating=rating)
        if paired is not None:
            match, joiner = paired
            messages.append(
                {
                    "type": "join",
                    "match_id": match.id,
                    "user_id": user.id,
                    "side": joiner.side,
                }
            )
        else:
            now = _now()
            match = DuelMatch(
                song_id=song.id,
                mode="online",
                bot_level=None,
                status="waiting",
                rules_version=duel_rules.RULES_VERSION,
                seed=seed,
                note_count=note_count,
                note_times_ms=note_times,
                created_at=now,
                updated_at=now,
            )
            db.add(match)
            db.flush()
            db.add(
                DuelParticipant(
                    match_id=match.id,
                    user_id=user.id,
                    side="player",
                    is_bot=False,
                    rating_before=rating,
                )
            )
            db.flush()

    state = snapshot(db, match, your_user_id=user.id)
    db.commit()
    return state, messages


def join_match(
    db: Session, *, match_id: UUID, user: User
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    match = db.scalar(
        select(DuelMatch)
        .where(DuelMatch.id == match_id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    if match is None:
        raise DuelError(404, "match_not_found", "Duel match not found")

    existing = _participant(db, match.id, user.id)
    if existing is not None:
        return snapshot(db, match, your_user_id=user.id), []

    if match.mode != "online":
        raise DuelError(409, "not_joinable", "Bot matches cannot be joined")
    if match.status != "waiting":
        raise DuelError(409, "match_not_waiting", "Match is not waiting for a player")

    participants = _participants(db, match.id)
    if "opponent" in participants:
        raise DuelError(409, "match_full", "Match already has two players")

    created_at = _aware(match.created_at) or _now()
    if _now() - created_at > QUEUE_TTL:
        match.status = "cancelled"
        match.finished_at = _now()
        db.commit()
        raise DuelError(409, "no_opponent", "No opponent found in time")

    participant = DuelParticipant(
        match_id=match.id,
        user_id=user.id,
        side="opponent",
        is_bot=False,
        rating_before=current_rating(db, user.id),
    )
    db.add(participant)
    db.flush()
    state = snapshot(db, match, your_user_id=user.id)
    db.commit()
    return state, [
        {
            "type": "join",
            "match_id": match.id,
            "user_id": user.id,
            "side": "opponent",
        }
    ]


# ---------------------------------------------------------------------------
# Ready / start
# ---------------------------------------------------------------------------
def ready_match(
    db: Session, *, match_id: UUID, user: User
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    match = db.scalar(
        select(DuelMatch)
        .where(DuelMatch.id == match_id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    if match is None:
        raise DuelError(404, "match_not_found", "Duel match not found")

    participant = _participant(db, match.id, user.id)
    if participant is None:
        raise DuelError(403, "not_participant", "You are not a participant")

    if match.status in ("finished", "cancelled"):
        raise DuelError(409, "match_closed", "Match is already closed")

    if match.status == "playing":
        return snapshot(db, match, your_user_id=user.id), []

    participants = _participants(db, match.id)
    if "opponent" not in participants:
        created_at = _aware(match.created_at) or _now()
        if _now() - created_at > QUEUE_TTL:
            match.status = "cancelled"
            match.finished_at = _now()
            db.commit()
            raise DuelError(409, "no_opponent", "No opponent found in time")
        if participant.ready_at is None:
            participant.ready_at = _now()
        state = snapshot(db, match, your_user_id=user.id)
        db.commit()
        return state, [
            {"type": "ready", "match_id": match.id, "side": participant.side}
        ]

    if participant.ready_at is None:
        participant.ready_at = _now()
    # The identity map normally returns the same row, but make the readiness
    # check independent of that implementation detail.
    participants[participant.side] = participant

    all_ready = all(row.ready_at is not None for row in participants.values())
    messages: list[dict[str, Any]] = [
        {"type": "ready", "match_id": match.id, "side": participant.side}
    ]

    if all_ready and match.status == "waiting":
        now = _now()
        match.status = "playing"
        match.started_at = now + timedelta(milliseconds=COUNTDOWN_MS)
        match.next_note_index = 0
        state = snapshot(db, match, your_user_id=user.id, now=now)
        db.commit()
        messages.extend(
            [
                {
                    "type": "start",
                    "match_id": match.id,
                    "started_at": match.started_at,
                    "countdown_ms": COUNTDOWN_MS,
                    "rules_version": match.rules_version,
                    "seed": int(match.seed),
                    "note_count": int(match.note_count),
                },
                {
                    "type": "countdown",
                    "match_id": match.id,
                    "seconds": COUNTDOWN_MS // 1000,
                    "starts_in_ms": COUNTDOWN_MS,
                },
            ]
        )
        return state, messages

    state = snapshot(db, match, your_user_id=user.id)
    db.commit()
    return state, messages


# ---------------------------------------------------------------------------
# Event validation and resolution
# ---------------------------------------------------------------------------
def _expected_note_ms(match: DuelMatch, note_index: int) -> int:
    times = match.note_times_ms
    if times:
        index = max(0, min(int(note_index), len(times) - 1))
        return int(times[index])
    return int(note_index) * SYNTHETIC_NOTE_INTERVAL_MS


def _timing_error(match: DuelMatch, note_index: int, server_time_ms: int) -> str | None:
    expected = _expected_note_ms(match, note_index)
    if match.note_times_ms:
        before = EVENT_WINDOW_BEFORE_MS
        after = EVENT_WINDOW_AFTER_MS
    else:
        before = SYNTHETIC_WINDOW_BEFORE_MS
        after = SYNTHETIC_WINDOW_AFTER_MS
    if server_time_ms < expected - before:
        return "event_too_early"
    if server_time_ms > expected + after:
        return "event_too_late"
    return None


def _load_events_from(
    db: Session,
    match_id: UUID,
    from_index: int,
    user_to_side: dict[UUID, str],
) -> dict[tuple[str, int], str]:
    rows = db.execute(
        select(DuelEvent.user_id, DuelEvent.note_index, DuelEvent.quality).where(
            DuelEvent.match_id == match_id,
            DuelEvent.note_index >= from_index,
        )
    ).all()
    events: dict[tuple[str, int], str] = {}
    for user_id, note_index, quality in rows:
        side = user_to_side.get(user_id)
        if side is not None:
            events[(side, int(note_index))] = str(quality)
    return events


def _count_quality(participant: DuelParticipant, quality: str) -> None:
    attribute = QUALITY_COUNTER.get(quality, "miss_count")
    setattr(participant, attribute, int(getattr(participant, attribute) or 0) + 1)
    participant.notes_played = int(participant.notes_played or 0) + 1


def _resolve_notes(
    db: Session,
    match: DuelMatch,
    participants: dict[str, DuelParticipant],
    *,
    force: bool = False,
    now: datetime | None = None,
) -> list[dict[str, Any]]:
    """Resolve chart notes in order until the next one is not due yet.

    ``force`` resolves every remaining note as a miss for a side that did not
    report it (used by ``finish`` and the end of the chart). Damage and HP are
    updated on the participant rows; the caller commits.
    """
    now = now or _now()
    player = participants.get("player")
    opponent = participants.get("opponent")
    if player is None or opponent is None:
        return []

    user_to_side = {
        row.user_id: side
        for side, row in participants.items()
        if row.user_id is not None and not row.is_bot
    }
    events = _load_events_from(
        db, match.id, int(match.next_note_index), user_to_side
    )
    bot = opponent if opponent.is_bot else (player if player.is_bot else None)
    server_time_ms = _ms_since(match.started_at, now)
    exchanges: list[dict[str, Any]] = []

    index = int(match.next_note_index)
    while index < int(match.note_count):
        player_quality = events.get(("player", index))
        opponent_quality = events.get(("opponent", index))

        expected = _expected_note_ms(match, index)
        after = (
            EVENT_WINDOW_AFTER_MS
            if match.note_times_ms
            else SYNTHETIC_WINDOW_AFTER_MS
        )
        due = force or server_time_ms >= expected + after

        if bot is not None:
            if player_quality is None and opponent_quality is None and not due:
                break
            # The bot side is generated, never reported by a client.
            if bot is opponent:
                opponent_quality = duel_rules.duel_bot_quality(
                    match.bot_level or "easy", int(match.seed), index
                ).token
            else:
                player_quality = duel_rules.duel_bot_quality(
                    match.bot_level or "easy", int(match.seed), index
                ).token
        else:
            if player_quality is None and opponent_quality is None and not due:
                break
            if (
                (player_quality is None or opponent_quality is None)
                and not due
            ):
                break

        player_quality = player_quality or "miss"
        opponent_quality = opponent_quality or "miss"
        result = duel_rules.resolve_note(player_quality, opponent_quality)

        player.hp_final = duel_rules.apply_damage(
            float(player.hp_final), result.damage_to_player
        )
        opponent.hp_final = duel_rules.apply_damage(
            float(opponent.hp_final), result.damage_to_opponent
        )
        player.damage_dealt = float(player.damage_dealt or 0.0) + result.damage_to_opponent
        player.damage_taken = float(player.damage_taken or 0.0) + result.damage_to_player
        opponent.damage_dealt = (
            float(opponent.damage_dealt or 0.0) + result.damage_to_player
        )
        opponent.damage_taken = (
            float(opponent.damage_taken or 0.0) + result.damage_to_opponent
        )

        _count_quality(player, player_quality)
        _count_quality(opponent, opponent_quality)

        exchanges.append(
            {
                "note_index": index,
                "exchange": result.exchange.token,
                "quality": {
                    "player": player_quality,
                    "opponent": opponent_quality,
                },
                "damage_to_player": round(result.damage_to_player, 3),
                "damage_to_opponent": round(result.damage_to_opponent, 3),
                "player_hp": round(float(player.hp_final), 2),
                "opponent_hp": round(float(opponent.hp_final), 2),
                "player_stats": duel_rules.stats_payload(player),
                "opponent_stats": duel_rules.stats_payload(opponent),
            }
        )

        match.next_note_index = index + 1
        index += 1

        if float(player.hp_final) <= 0.0 or float(opponent.hp_final) <= 0.0:
            break

    db.flush()
    return exchanges


def exchange_messages(match: DuelMatch, exchanges: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    """WebSocket messages matching the documented ``note``/``hp``/``block``."""
    return exchange_messages_for_state(match.id, exchanges)


def exchange_messages_for_state(
    match_id: UUID, exchanges: Sequence[dict[str, Any]]
) -> list[dict[str, Any]]:
    """Build the ``note``/``hp``/``block`` messages for a resolved batch."""
    messages: list[dict[str, Any]] = []
    for exchange in exchanges:
        messages.append({"type": "note", "match_id": match_id, **exchange})
        if exchange.get("exchange") == "block":
            messages.append(
                {
                    "type": "block",
                    "match_id": match_id,
                    "note_index": exchange["note_index"],
                    "player_hp": exchange["player_hp"],
                    "opponent_hp": exchange["opponent_hp"],
                }
            )
        messages.append(
            {
                "type": "hp",
                "match_id": match_id,
                "note_index": exchange["note_index"],
                "player_hp": exchange["player_hp"],
                "opponent_hp": exchange["opponent_hp"],
                "player_stats": exchange["player_stats"],
                "opponent_stats": exchange["opponent_stats"],
            }
        )
    return messages


def submit_events(
    db: Session,
    *,
    match_id: UUID,
    user: User,
    events: Sequence[dict[str, Any]],
) -> tuple[
    dict[str, Any],
    list[int],
    list[int],
    list[dict[str, Any]],
    list[dict[str, Any]],
    bool,
]:
    """Validate and store a batch of client notes, then resolve due exchanges."""
    if len(events) > MAX_EVENTS_PER_BATCH:
        raise DuelError(
            422,
            "batch_too_large",
            f"At most {MAX_EVENTS_PER_BATCH} events per batch",
        )

    match = db.scalar(
        select(DuelMatch)
        .where(DuelMatch.id == match_id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    if match is None:
        raise DuelError(404, "match_not_found", "Duel match not found")

    participant = _participant(db, match.id, user.id)
    if participant is None:
        raise DuelError(403, "not_participant", "You are not a participant")
    if match.status == "finished":
        raise DuelError(409, "match_finished", "Match is already finished")
    if match.status != "playing":
        raise DuelError(409, "match_not_started", "Match has not started")
    if participant.left_at is not None:
        raise DuelError(409, "participant_left", "You have left the match")

    now = _now()
    server_time_ms = _ms_since(match.started_at, now)
    participants = _participants(db, match.id)
    user_to_side = {
        row.user_id: side
        for side, row in participants.items()
        if row.user_id is not None and not row.is_bot
    }

    requested: list[int] = []
    for event in events:
        if not isinstance(event, dict):
            continue
        try:
            requested.append(int(event.get("note_index")))
        except (TypeError, ValueError):
            continue

    existing = set(
        db.scalars(
            select(DuelEvent.note_index).where(
                DuelEvent.match_id == match.id,
                DuelEvent.user_id == user.id,
                DuelEvent.note_index.in_(requested),
            )
        ).all()
    ) if requested else set()

    accepted: list[int] = []
    duplicates: list[int] = []
    rejected: list[dict[str, Any]] = []
    rows: list[dict[str, Any]] = []
    batch_seen: set[int] = set()

    for position, event in enumerate(events):
        if not isinstance(event, dict):
            rejected.append(
                {
                    "index": position,
                    "note_index": -1,
                    "code": "invalid_event",
                    "message": "Each event must be an object",
                }
            )
            continue
        try:
            note_index = int(event.get("note_index"))
        except (TypeError, ValueError):
            rejected.append(
                {
                    "index": position,
                    "note_index": -1,
                    "code": "invalid_note_index",
                    "message": "note_index must be a non-negative integer",
                }
            )
            continue
        if note_index < 0:
            rejected.append(
                {
                    "index": position,
                    "note_index": note_index,
                    "code": "invalid_note_index",
                    "message": "note_index must be a non-negative integer",
                }
            )
            continue
        quality = str(event.get("quality") or "").strip().lower()

        if note_index >= int(match.note_count):
            rejected.append(
                {
                    "index": position,
                    "note_index": note_index,
                    "code": "event_out_of_range",
                    "message": "note_index is outside the chart",
                }
            )
            continue
        if quality not in duel_rules.QUALITY_POINTS:
            rejected.append(
                {
                    "index": position,
                    "note_index": note_index,
                    "code": "invalid_quality",
                    "message": "Unknown note quality",
                }
            )
            continue
        if note_index in existing or note_index in batch_seen:
            duplicates.append(note_index)
            continue

        timing_error = _timing_error(match, note_index, server_time_ms)
        if timing_error is not None:
            rejected.append(
                {
                    "index": position,
                    "note_index": note_index,
                    "code": timing_error,
                    "message": (
                        "Note arrived before its chart window"
                        if timing_error == "event_too_early"
                        else "Note arrived after its chart window"
                    ),
                }
            )
            continue

        batch_seen.add(note_index)
        client_time_ms = event.get("client_time_ms")
        if client_time_ms is not None:
            try:
                client_time_ms = int(client_time_ms)
            except (TypeError, ValueError):
                client_time_ms = None
            else:
                client_time_ms = max(0, client_time_ms)
        rows.append(
            {
                "id": uuid4(),
                "match_id": match.id,
                "user_id": user.id,
                "note_index": note_index,
                "quality": quality,
                "client_time_ms": client_time_ms,
                "server_time_ms": server_time_ms,
            }
        )
        accepted.append(note_index)

    if rows:
        inserted = db.execute(
            pg_insert(DuelEvent.__table__)
            .values(rows)
            .on_conflict_do_nothing(
                index_elements=["match_id", "user_id", "note_index"]
            )
            .returning(DuelEvent.__table__.c.note_index)
        ).scalars().all()
        inserted_set = {int(value) for value in inserted}
        # A concurrent retry may have won the unique race; report it honestly.
        accepted = [value for value in accepted if value in inserted_set]
        duplicates.extend(
            sorted(value for value in batch_seen if value not in inserted_set)
        )

    exchanges = _resolve_notes(db, match, participants, now=now)
    finished = False
    if match.next_note_index >= match.note_count or any(
        float(row.hp_final) <= 0.0 for row in participants.values()
    ):
        # ``_resolve_notes`` already applied every exchange that belongs to the
        # final state; resolving the remainder again would add phantom notes.
        finished = finalize_match(
            db,
            match,
            participants=participants,
            resolve_remaining=False,
            now=now,
        )

    db.commit()
    state = snapshot(db, match, your_user_id=user.id)
    return state, sorted(accepted), sorted(duplicates), rejected, exchanges, finished


def tick_match(
    db: Session, *, match_id: UUID
) -> tuple[dict[str, Any] | None, list[dict[str, Any]], bool]:
    """Advance an online match without a new event.

    Used by the WebSocket ticker: notes whose window is over are resolved as a
    miss, and the chart end / a knockout finalises the match. Idempotent and
    safe to call while another request holds the match lock.
    """
    match = db.scalar(
        select(DuelMatch)
        .where(DuelMatch.id == match_id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    if match is None:
        return None, [], False
    if match.status != "playing":
        if match.status == "finished":
            return snapshot(db, match), [], False
        return None, [], False

    participants = _participants(db, match.id)
    exchanges = _resolve_notes(db, match, participants)
    finished = False
    if match.next_note_index >= match.note_count or any(
        float(row.hp_final) <= 0.0 for row in participants.values()
    ):
        finished = finalize_match(
            db,
            match,
            participants=participants,
            resolve_remaining=False,
        )
    state = snapshot(db, match)
    db.commit()
    return state, exchanges, finished


# ---------------------------------------------------------------------------
# Finalisation: winner, XP and Elo
# ---------------------------------------------------------------------------
def _winner_side(
    participants: dict[str, DuelParticipant],
    *,
    technical_winner: str | None = None,
) -> str | None:
    if technical_winner in ("player", "opponent"):
        return technical_winner

    player = participants.get("player")
    opponent = participants.get("opponent")
    if player is None or opponent is None:
        return None

    player_hp = float(player.hp_final)
    opponent_hp = float(opponent.hp_final)

    if player_hp <= 0.0 and opponent_hp <= 0.0:
        return None
    if opponent_hp <= 0.0:
        return "player"
    if player_hp <= 0.0:
        return "opponent"
    if abs(player_hp - opponent_hp) < 0.001:
        return None
    return "player" if player_hp > opponent_hp else "opponent"


def _apply_xp(db: Session, participant: DuelParticipant, result: str | None) -> int:
    if participant.is_bot or participant.user_id is None:
        participant.xp_awarded = 0
        return 0

    xp = duel_rules.xp_for_duel(
        perfect=int(participant.perfect_count or 0),
        great=int(participant.great_count or 0),
        good=int(participant.good_count or 0),
        result=result,
    )
    if xp <= 0:
        participant.xp_awarded = 0
        return 0

    user = db.scalar(
        select(User)
        .where(User.id == participant.user_id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    if user is None:
        participant.xp_awarded = 0
        return 0

    total = int(user.xp_total or 0) + xp
    user.xp_total = total
    user.level = level_for_xp(total)
    participant.xp_awarded = xp
    return xp


def _apply_rating(
    db: Session,
    match: DuelMatch,
    participants: dict[str, DuelParticipant],
    winner_side: str | None,
) -> None:
    """Elo for online human-vs-human matches only."""
    player = participants.get("player")
    opponent = participants.get("opponent")
    if player is None or opponent is None:
        return
    if match.mode != "online" or player.is_bot or opponent.is_bot:
        return
    if player.user_id is None or opponent.user_id is None:
        return

    before_a = (
        int(player.rating_before)
        if player.rating_before is not None
        else current_rating(db, player.user_id)
    )
    before_b = (
        int(opponent.rating_before)
        if opponent.rating_before is not None
        else current_rating(db, opponent.user_id)
    )
    _a_before, after_a, _b_before, after_b = duel_rules.apply_elo(
        before_a, before_b, winner=winner_side
    )
    player.rating_before = before_a
    player.rating_after = after_a
    opponent.rating_before = before_b
    opponent.rating_after = after_b


def _submitted_note_count(
    db: Session, *, match_id: UUID, user_id: UUID | None
) -> int:
    if user_id is None:
        return 0
    return int(
        db.scalar(
            select(func.count(DuelEvent.id)).where(
                DuelEvent.match_id == match_id,
                DuelEvent.user_id == user_id,
            )
        )
        or 0
    )


def finalize_match(
    db: Session,
    match: DuelMatch,
    *,
    participants: dict[str, DuelParticipant] | None = None,
    technical_winner: str | None = None,
    resolve_remaining: bool = True,
    now: datetime | None = None,
) -> bool:
    """Finish a playing match exactly once.

    Returns ``True`` when this call finalised it, ``False`` for an already
    finished/cancelled match. The match row must already be locked by the
    caller; the status check makes a double ``finish`` a no-op.
    """
    if match.status != "playing":
        return False

    now = now or _now()
    participants = participants or _participants(db, match.id)

    if resolve_remaining:
        _resolve_notes(db, match, participants, force=True, now=now)

    winner_side = _winner_side(participants, technical_winner=technical_winner)

    for side, participant in participants.items():
        if winner_side is None:
            participant.result = "draw"
        elif side == winner_side:
            participant.result = "win"
        else:
            participant.result = "loss"

    winner = participants.get(winner_side) if winner_side else None
    match.winner_user_id = (
        winner.user_id if winner is not None and not winner.is_bot else None
    )

    for participant in participants.values():
        # A forfeit before the first resolved note is not participation: no XP
        # for it, otherwise leaving bot matches would be an XP farm.
        if technical_winner is not None and int(participant.notes_played or 0) == 0:
            participant.xp_awarded = 0
            continue
        _apply_xp(db, participant, participant.result)

    _apply_rating(db, match, participants, winner_side)

    match.status = "finished"
    match.finished_at = now
    db.flush()
    return True


# ---------------------------------------------------------------------------
# Finish / leave / result
# ---------------------------------------------------------------------------
def finish_match(
    db: Session, *, match_id: UUID, user: User
) -> tuple[dict[str, Any], bool, bool, list[dict[str, Any]]]:
    """Signal the end of the chart.

    Bot matches finish immediately. Online matches wait for both sides; a side
    that left or the chart end finalises them. Returns
    ``(state, already_finished, finished_now, exchanges)``.
    """
    match = db.scalar(
        select(DuelMatch)
        .where(DuelMatch.id == match_id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    if match is None:
        raise DuelError(404, "match_not_found", "Duel match not found")

    participant = _participant(db, match.id, user.id)
    if participant is None:
        raise DuelError(403, "not_participant", "You are not a participant")

    if match.status == "finished":
        return snapshot(db, match, your_user_id=user.id), True, False, []
    if match.status == "cancelled":
        raise DuelError(409, "match_cancelled", "Match was cancelled")
    if match.status != "playing":
        raise DuelError(409, "match_not_started", "Match has not started")

    now = _now()
    participants = _participants(db, match.id)
    participants[participant.side] = participant

    # Finishing without a single reported note is a forfeit, not a way to
    # force a draw or farm participation XP.
    submitted = _submitted_note_count(
        db, match_id=match.id, user_id=participant.user_id
    )
    if submitted == 0:
        participant.left_at = now
        participant.finish_requested_at = now
        finished_now = finalize_match(
            db,
            match,
            participants=participants,
            technical_winner=duel_rules.other_side(participant.side),
            resolve_remaining=False,
            now=now,
        )
        state = snapshot(db, match, your_user_id=user.id)
        db.commit()
        return state, False, finished_now, []

    if participant.finish_requested_at is None:
        participant.finish_requested_at = now
    db.flush()

    opponent = participants.get(duel_rules.other_side(participant.side))

    should_finish = match.mode == "bot"
    if not should_finish:
        both_requested = all(
            row.finish_requested_at is not None for row in participants.values()
        )
        opponent_left = opponent is not None and opponent.left_at is not None
        chart_done = int(match.next_note_index) >= int(match.note_count)
        should_finish = both_requested or opponent_left or chart_done

    exchanges: list[dict[str, Any]] = []
    finished_now = False
    if should_finish:
        exchanges = _resolve_notes(db, match, participants, force=True, now=now)
        finished_now = finalize_match(
            db,
            match,
            participants=participants,
            resolve_remaining=False,
            now=now,
        )
    else:
        db.flush()

    state = snapshot(db, match, your_user_id=user.id)
    db.commit()
    return state, False, finished_now, exchanges


def leave_match(
    db: Session, *, match_id: UUID, user: User
) -> tuple[dict[str, Any], bool, bool, list[dict[str, Any]]]:
    """Leave a match. Waiting matches are cancelled; playing matches finalise.

    A side with no resolved note at all loses technically. Otherwise the
    remaining HP decides, as documented for Guitar Duels.
    """
    match = db.scalar(
        select(DuelMatch)
        .where(DuelMatch.id == match_id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    if match is None:
        raise DuelError(404, "match_not_found", "Duel match not found")

    participant = _participant(db, match.id, user.id)
    if participant is None:
        raise DuelError(403, "not_participant", "You are not a participant")

    now = _now()

    if match.status == "finished":
        return snapshot(db, match, your_user_id=user.id), True, False, []
    if match.status == "cancelled":
        return snapshot(db, match, your_user_id=user.id), False, False, []

    if match.status == "waiting":
        participant.left_at = now
        match.status = "cancelled"
        match.finished_at = now
        state = snapshot(db, match, your_user_id=user.id)
        db.commit()
        return state, False, True, [{"type": "cancelled", "match_id": match.id}]

    participant.left_at = now
    participants = _participants(db, match.id)
    participants[participant.side] = participant
    had_moves = (
        _submitted_note_count(
            db, match_id=match.id, user_id=participant.user_id
        )
        > 0
    )
    technical_winner = (
        None if had_moves else duel_rules.other_side(participant.side)
    )
    finished_now = finalize_match(
        db,
        match,
        participants=participants,
        technical_winner=technical_winner,
        resolve_remaining=False,
        now=now,
    )
    state = snapshot(db, match, your_user_id=user.id)
    db.commit()
    return state, False, finished_now, [{"type": "finish", "match_id": match.id}]


def match_result(
    db: Session, *, match_id: UUID, user: User
) -> dict[str, Any]:
    match = db.scalar(select(DuelMatch).where(DuelMatch.id == match_id))
    if match is None:
        raise DuelError(404, "match_not_found", "Duel match not found")
    if _participant(db, match.id, user.id) is None:
        raise DuelError(403, "not_participant", "You are not a participant")
    if match.status != "finished":
        raise DuelError(409, "match_not_finished", "Match is not finished yet")
    return snapshot(db, match, your_user_id=user.id)


def get_match(
    db: Session, *, match_id: UUID, user: User
) -> dict[str, Any]:
    """Full state for a participant (used by REST and on WS reconnect)."""
    match = db.scalar(select(DuelMatch).where(DuelMatch.id == match_id))
    if match is None:
        raise DuelError(404, "match_not_found", "Duel match not found")
    if _participant(db, match.id, user.id) is None:
        raise DuelError(403, "not_participant", "You are not a participant")
    return snapshot(db, match, your_user_id=user.id)


def touch_participant(
    db: Session, *, match_id: UUID, user_id: UUID
) -> dict[str, Any] | None:
    """Refresh ``last_seen_at`` for a participant; return its side."""
    participant = _participant(db, match_id, user_id)
    if participant is None:
        return None
    participant.last_seen_at = _now()
    side = participant.side
    db.commit()
    return {"side": side}


# ---------------------------------------------------------------------------
# History, leaderboard and profiles
# ---------------------------------------------------------------------------
def _history_rows(
    db: Session,
    *,
    user_id: UUID,
    limit: int,
    offset: int,
    mode: str | None = None,
    result: str | None = None,
) -> tuple[list[dict[str, Any]], int]:
    opponent = aliased(DuelParticipant)
    opponent_user = aliased(User)

    filters = [DuelParticipant.user_id == user_id]
    if mode is not None:
        filters.append(DuelMatch.mode == mode)
    if result is not None:
        filters.append(DuelParticipant.result == result)

    base = (
        select(DuelParticipant, DuelMatch, Song, opponent, opponent_user)
        .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
        .join(Song, Song.id == DuelMatch.song_id)
        .outerjoin(
            opponent,
            (opponent.match_id == DuelMatch.id)
            & (opponent.side != DuelParticipant.side),
        )
        .outerjoin(opponent_user, opponent_user.id == opponent.user_id)
        .where(*filters)
    )
    total = int(
        db.scalar(
            select(func.count())
            .select_from(DuelParticipant)
            .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
            .where(*filters)
        )
        or 0
    )
    rows = db.execute(
        base.order_by(
            func.coalesce(DuelMatch.finished_at, DuelMatch.created_at).desc(),
            DuelMatch.id.desc(),
        )
        .limit(limit)
        .offset(offset)
    ).all()

    items: list[dict[str, Any]] = []
    for own, match, song, other, other_user in rows:
        opponent_payload = (
            _participant_payload(other, other_user) if other is not None else None
        )
        before = own.rating_before
        after = own.rating_after
        items.append(
            {
                "match_id": match.id,
                "mode": match.mode,
                "status": match.status,
                "song": song_info(song),
                "opponent": opponent_payload,
                "your_side": own.side,
                "hp": round(float(own.hp_final), 2),
                "opponent_hp": (
                    round(float(other.hp_final), 2) if other is not None else 0.0
                ),
                "result": own.result,
                "xp_awarded": int(own.xp_awarded or 0),
                "rating_delta": (
                    int(after) - int(before)
                    if before is not None and after is not None
                    else None
                ),
                "created_at": match.created_at,
                "finished_at": match.finished_at,
            }
        )
    return items, total


def history(
    db: Session,
    *,
    user_id: UUID,
    limit: int = 20,
    offset: int = 0,
    mode: str | None = None,
    result: str | None = None,
) -> tuple[list[dict[str, Any]], int]:
    return _history_rows(
        db,
        user_id=user_id,
        limit=limit,
        offset=offset,
        mode=mode,
        result=result,
    )


def _period_start(period: str, now: datetime | None = None) -> datetime | None:
    now = now or _now()
    if period == "week":
        return now - timedelta(days=7)
    if period == "month":
        return now - timedelta(days=30)
    return None


def leaderboard(
    db: Session,
    *,
    period: str = "all",
    limit: int = 20,
    offset: int = 0,
) -> tuple[list[dict[str, Any]], int]:
    start = _period_start(period)
    latest_rating = _latest_rating_subquery()

    filters = [
        DuelMatch.mode == "online",
        DuelMatch.status == "finished",
        DuelParticipant.is_bot.is_(False),
        DuelParticipant.user_id.is_not(None),
        User.is_active.is_(True),
    ]
    if start is not None:
        filters.append(DuelMatch.finished_at >= start)

    wins = func.sum(case((DuelParticipant.result == "win", 1), else_=0))
    losses = func.sum(case((DuelParticipant.result == "loss", 1), else_=0))
    draws = func.sum(case((DuelParticipant.result == "draw", 1), else_=0))
    points = func.sum(
        DuelParticipant.perfect_count * 100
        + DuelParticipant.great_count * 85
        + DuelParticipant.good_count * 70
    )
    notes = func.sum(DuelParticipant.notes_played)

    total = int(
        db.scalar(
            select(func.count(func.distinct(DuelParticipant.user_id)))
            .select_from(DuelParticipant)
            .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
            .join(User, User.id == DuelParticipant.user_id)
            .where(*filters)
        )
        or 0
    )

    rows = db.execute(
        select(
            User.id.label("user_id"),
            User.username,
            User.display_name,
            User.level,
            func.count(DuelParticipant.id).label("duels"),
            wins.label("wins"),
            losses.label("losses"),
            draws.label("draws"),
            func.sum(DuelParticipant.xp_awarded).label("xp_earned"),
            points.label("points"),
            notes.label("notes"),
            func.sum(DuelParticipant.damage_dealt).label("damage"),
            latest_rating.c.rating.label("rating"),
        )
        .join(DuelParticipant, DuelParticipant.user_id == User.id)
        .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
        .outerjoin(latest_rating, latest_rating.c.user_id == User.id)
        .where(*filters)
        .group_by(
            User.id,
            User.username,
            User.display_name,
            User.level,
            latest_rating.c.rating,
        )
        .order_by(
            wins.desc(),
            func.sum(DuelParticipant.xp_awarded).desc(),
            points.desc(),
            User.username.asc(),
        )
        .limit(limit)
        .offset(offset)
    ).all()

    items: list[dict[str, Any]] = []
    for position, row in enumerate(rows):
        duels = int(row.duels or 0)
        wins_count = int(row.wins or 0)
        notes_count = int(row.notes or 0)
        accuracy = (
            float(row.points or 0) / (notes_count * 100.0) * 100.0
            if notes_count > 0
            else 0.0
        )
        items.append(
            {
                "rank": offset + position + 1,
                "user_id": row.user_id,
                "username": row.username,
                "display_name": row.display_name,
                "level": int(row.level or 1),
                "rating": int(row.rating or duel_rules.ELO_START_RATING),
                "duels": duels,
                "wins": wins_count,
                "losses": int(row.losses or 0),
                "draws": int(row.draws or 0),
                "win_rate": round(100.0 * wins_count / duels, 2) if duels else 0.0,
                "xp_earned": int(row.xp_earned or 0),
                "accuracy": round(accuracy, 2),
                "damage_dealt": round(float(row.damage or 0.0), 2),
            }
        )
    return items, total


def _profile_totals(
    db: Session, *, user_id: UUID, mode: str | None
) -> dict[str, Any]:
    filters = [
        DuelParticipant.user_id == user_id,
        DuelParticipant.is_bot.is_(False),
        DuelMatch.status == "finished",
        DuelMatch.mode == "online" if mode == "online" else DuelMatch.mode == "bot",
    ]
    row = db.execute(
        select(
            func.count(DuelParticipant.id).label("duels"),
            func.sum(case((DuelParticipant.result == "win", 1), else_=0)).label(
                "wins"
            ),
            func.sum(case((DuelParticipant.result == "loss", 1), else_=0)).label(
                "losses"
            ),
            func.sum(case((DuelParticipant.result == "draw", 1), else_=0)).label(
                "draws"
            ),
            func.sum(DuelParticipant.xp_awarded).label("xp"),
            func.sum(
                DuelParticipant.perfect_count * 100
                + DuelParticipant.great_count * 85
                + DuelParticipant.good_count * 70
            ).label("points"),
            func.sum(DuelParticipant.notes_played).label("notes"),
            func.sum(DuelParticipant.damage_dealt).label("damage"),
        )
        .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
        .where(*filters)
    ).one()

    results = db.scalars(
        select(DuelParticipant.result)
        .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
        .where(*filters)
        .order_by(
            func.coalesce(DuelMatch.finished_at, DuelMatch.created_at).asc(),
            DuelMatch.id.asc(),
        )
    ).all()
    best_streak = 0
    current = 0
    for value in results:
        if value == "win":
            current += 1
            best_streak = max(best_streak, current)
        else:
            current = 0

    duels = int(row.duels or 0)
    wins = int(row.wins or 0)
    notes = int(row.notes or 0)
    return {
        "duels": duels,
        "wins": wins,
        "losses": int(row.losses or 0),
        "draws": int(row.draws or 0),
        "win_rate": round(100.0 * wins / duels, 2) if duels else 0.0,
        "xp_earned": int(row.xp or 0),
        "accuracy": (
            round(float(row.points or 0) / (notes * 100.0) * 100.0, 2)
            if notes
            else 0.0
        ),
        "damage_dealt": round(float(row.damage or 0.0), 2),
        "best_win_streak": best_streak,
    }


def profile(db: Session, *, user_id: UUID) -> dict[str, Any]:
    user = db.get(User, user_id)
    if user is None or not user.is_active:
        raise DuelError(404, "user_not_found", "Player not found")

    peak = db.scalar(
        select(func.max(DuelParticipant.rating_after)).where(
            DuelParticipant.user_id == user_id,
            DuelParticipant.is_bot.is_(False),
            DuelParticipant.rating_after.is_not(None),
        )
    )
    recent, _total = _history_rows(
        db, user_id=user_id, limit=10, offset=0
    )
    return {
        "user_id": user.id,
        "username": user.username,
        "display_name": user.display_name,
        "level": int(user.level or 1),
        "rating": current_rating(db, user_id),
        "peak_rating": int(peak) if peak is not None else duel_rules.ELO_START_RATING,
        "online": _profile_totals(db, user_id=user_id, mode="online"),
        "bot": _profile_totals(db, user_id=user_id, mode="bot"),
        "recent_matches": recent,
    }


def active_match(db: Session, *, user_id: UUID) -> dict[str, Any] | None:
    match = db.scalar(
        select(DuelMatch)
        .join(DuelParticipant, DuelParticipant.match_id == DuelMatch.id)
        .where(
            DuelParticipant.user_id == user_id,
            DuelMatch.status.in_(("waiting", "playing")),
        )
        .order_by(DuelMatch.created_at.desc(), DuelMatch.id.desc())
        .limit(1)
    )
    if match is None:
        return None
    return snapshot(db, match, your_user_id=user_id)
