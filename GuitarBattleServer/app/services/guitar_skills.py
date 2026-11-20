"""Guitar Skills: the competitive rating of a player.

The rating is a weighted blend of five components, all in ``0..100``::

    GuitarSkills = 0.35*A + 0.20*C + 0.10*S + 0.25*D + 0.10*P

* **A (accuracy)** — recency/difficulty/length weighted accuracy of the last
  :class:`GuitarSkillsConfig.window_size` ranked events.
* **C (challenge)** — the same weighted average of
  ``accuracy * difficulty * note / 1.25``.
* **S (score)** — ``actual_score / (note_count * 100)``. ``100`` is the real
  maximum of the existing scoring rules: the duel rules award ``100`` points
  per perfect note (``duel_rules.QUALITY_POINTS``) and the solo
  ``scores.score_points`` ledger uses the same quality-point scale. There is no
  combo multiplier anywhere on the server, so ``notes_total * 100`` is the
  exact chart maximum. Events with an unknown note count are skipped.
* **D (duel)** — online human-vs-human duels only, Elo-style expectation from
  the frozen pre-match strengths, scaled by a reliability factor.
* **P (progression)** — 70% level, 30% fixed-reference XP.

The engine never invents data: a missing history yields the neutral value
``50`` with confidence ``0``, an unknown note count keeps the note factor
neutral, and a historical duel without frozen Guitar Skills uses the documented
rating proxy while lowering confidence.

Everything is idempotent: ranked events are keyed by ``event_key`` and a
recalculation only writes history when the rounded total actually changes.
"""
from __future__ import annotations

import logging
import math
import threading
from dataclasses import dataclass
from datetime import datetime, timezone
from decimal import Decimal
from statistics import median
from typing import Any, Iterable, Mapping, Sequence
from uuid import UUID

from sqlalchemy import and_, exists, func, or_, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.orm import Session, aliased

from app.database import SessionLocal
from app.models.duel import DuelMatch, DuelParticipant
from app.models.game_session import GameSession, Score
from app.models.progression import UserAchievement
from app.models.guitar_skills import (
    CONFIG_ID,
    DEFAULT_XP_REFERENCE,
    GuitarSkillsConfig,
    GuitarSkillsConfigHistory,
    GuitarSkillsEvent,
    GuitarSkillsHistory,
    UserGuitarSkills,
)
from app.models.song import Song, SongFile
from app.models.song_midi import SongMidiAnalysis
from app.models.user import User
from app.services import duel_rules

logger = logging.getLogger(__name__)

#: A solo result counts for the rating only from this completion upwards.
MIN_RANKED_COMPLETION = 80.0
#: Solo modes that are competitive. ``practice`` and ``preview`` never count.
SOLO_MODES = ("normal", "daily")
#: ``guitar_skills_events.mode`` of a duel row.
DUEL_MODE = "online_duel"
#: Neutral component value used when there is no history at all.
NEUTRAL = 50.0
#: A first-time player starts from the neutral Guitar Skills value.
INITIAL_SKILLS = 50.0


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
def default_config_values() -> dict[str, Any]:
    """The documented defaults, used to seed the single configuration row."""
    return {
        "accuracy_weight": 0.35,
        "challenge_weight": 0.20,
        "score_weight": 0.10,
        "duel_weight": 0.25,
        "progression_weight": 0.10,
        "window_size": 30,
        "recency_decay": 10.0,
        "difficulty_easy_max": 3,
        "difficulty_normal_max": 7,
        "difficulty_easy_factor": 0.60,
        "difficulty_normal_factor": 0.80,
        "difficulty_hard_factor": 1.00,
        "note_factor_min": 0.75,
        "note_factor_max": 1.25,
        "median_note_count": None,
        "level_normalization_max": 50,
        "xp_reference": DEFAULT_XP_REFERENCE,
        "duel_strength_guitar_weight": 0.70,
        "duel_strength_level_weight": 0.30,
        "expected_score_scale": 25.0,
        "reliability_constant": 10.0,
        "per_song_attempt_cap": 2.0,
        "confidence_expected_events": 10,
        "confidence_expected_duels": 10,
        "provisional_threshold": 0.35,
        "min_ranked_events": 5,
        "calculation_version": 1,
    }


#: Editable fields stored on the single configuration row.
CONFIG_FIELDS: tuple[str, ...] = tuple(default_config_values())


@dataclass(frozen=True)
class SkillsConfig:
    """Immutable snapshot of :class:`GuitarSkillsConfig` used by the formulas."""

    accuracy_weight: float = 0.35
    challenge_weight: float = 0.20
    score_weight: float = 0.10
    duel_weight: float = 0.25
    progression_weight: float = 0.10
    window_size: int = 30
    recency_decay: float = 10.0
    difficulty_easy_max: int = 3
    difficulty_normal_max: int = 7
    difficulty_easy_factor: float = 0.60
    difficulty_normal_factor: float = 0.80
    difficulty_hard_factor: float = 1.00
    note_factor_min: float = 0.75
    note_factor_max: float = 1.25
    median_note_count: int | None = None
    level_normalization_max: int = 50
    xp_reference: int = DEFAULT_XP_REFERENCE
    duel_strength_guitar_weight: float = 0.70
    duel_strength_level_weight: float = 0.30
    expected_score_scale: float = 25.0
    reliability_constant: float = 10.0
    per_song_attempt_cap: float = 2.0
    confidence_expected_events: int = 10
    confidence_expected_duels: int = 10
    provisional_threshold: float = 0.35
    min_ranked_events: int = 5
    calculation_version: int = 1

    @classmethod
    def from_row(cls, row: GuitarSkillsConfig) -> "SkillsConfig":
        values = {
            field: getattr(row, field)
            for field in CONFIG_FIELDS
            if field != "median_note_count"
        }
        return cls(
            **values,
            median_note_count=(
                int(row.median_note_count)
                if row.median_note_count is not None
                else None
            ),
        )


def _clamp(low: float, high: float, value: float) -> float:
    if value < low:
        return low
    if value > high:
        return high
    return value


def round1(value: float) -> float:
    """Round half up to one decimal (the stored/returned precision)."""
    return math.floor(float(value) * 10.0 + 0.5) / 10.0


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _aware(value: datetime | None) -> datetime | None:
    if value is None:
        return None
    if value.tzinfo is None:
        return value.replace(tzinfo=timezone.utc)
    return value


def compute_median_note_count(db: Session) -> int | None:
    """Median chart length of published, non-daily, non-archived songs.

    Uses the analysed active MIDI file. When the catalogue has no analysed
    charts the function returns ``None`` instead of inventing a number; the
    engine then treats the note factor as neutral.
    """
    per_song = (
        # Only the aggregated note count is selected: ``db.scalars`` returns the
        # first column, so selecting ``Song.id`` as well would yield UUIDs.
        select(func.max(SongMidiAnalysis.note_count))
        .select_from(Song)
        .join(SongFile, SongFile.song_id == Song.id)
        .join(
            SongMidiAnalysis,
            SongMidiAnalysis.song_file_id == SongFile.id,
        )
        .where(
            SongFile.role == "midi",
            SongFile.is_active.is_(True),
            Song.is_published.is_(True),
            Song.daily_weekday.is_(None),
            Song.archived_at.is_(None),
            SongMidiAnalysis.note_count > 0,
        )
        .group_by(Song.id)
    )
    values = [int(value) for value in db.scalars(per_song).all() if value]
    if values:
        return int(round(median(values)))

    # Fallback for a catalogue without MIDI analyses: the per-song note totals
    # the clients actually reported. This is real data, never a guess.
    fallback = (
        select(func.max(Score.notes_total))
        .select_from(GameSession)
        .join(Score, Score.session_id == GameSession.id)
        .join(Song, Song.id == GameSession.song_id)
        .where(
            GameSession.status == "finished",
            GameSession.mode.in_(SOLO_MODES),
            Score.notes_total.isnot(None),
            Score.notes_total > 0,
            Song.is_published.is_(True),
            Song.daily_weekday.is_(None),
            Song.archived_at.is_(None),
        )
        .group_by(GameSession.song_id)
    )
    values = [int(value) for value in db.scalars(fallback).all() if value]
    if values:
        return int(round(median(values)))
    return None


def get_config(db: Session) -> GuitarSkillsConfig:
    """Return the single active configuration row, creating/repairing it."""
    row = db.get(GuitarSkillsConfig, CONFIG_ID)
    if row is None:
        values = default_config_values()
        values["median_note_count"] = compute_median_note_count(db)
        row = GuitarSkillsConfig(id=CONFIG_ID, **values)
        db.add(row)
        db.flush()
        return row
    if row.median_note_count is None:
        computed = compute_median_note_count(db)
        if computed is not None:
            row.median_note_count = computed
            db.flush()
    return row


def config_snapshot(row: GuitarSkillsConfig) -> SkillsConfig:
    return SkillsConfig.from_row(row)


def config_values(row: GuitarSkillsConfig) -> dict[str, Any]:
    """JSON-serialisable values (used by the history snapshot)."""
    values: dict[str, Any] = {}
    for field in CONFIG_FIELDS:
        value = getattr(row, field)
        if field == "median_note_count":
            value = int(value) if value is not None else None
        elif field in {
            "window_size",
            "difficulty_easy_max",
            "difficulty_normal_max",
            "level_normalization_max",
            "xp_reference",
            "confidence_expected_events",
            "confidence_expected_duels",
            "min_ranked_events",
            "calculation_version",
        }:
            value = int(value)
        else:
            value = float(value)
        values[field] = value
    return values


def config_payload(row: GuitarSkillsConfig) -> dict[str, Any]:
    return {
        **config_values(row),
        "updated_at": row.updated_at,
        "updated_by": row.updated_by,
    }


# ---------------------------------------------------------------------------
# Formula input/output
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class EventRecord:
    """One ranked result, already normalised for the formulas."""

    event_key: str
    song_id: UUID | None
    accuracy: float  # percent, 0..100
    score: int
    note_count: int | None
    difficulty: int | None
    occurred_at: datetime
    mode: str = "normal"
    match_id: UUID | None = None
    opponent_id: UUID | None = None
    opponent_level: int | None = None
    opponent_pre_match_rating: int | None = None
    opponent_pre_match_guitar_skills: float | None = None
    player_pre_match_rating: int | None = None
    player_pre_match_guitar_skills: float | None = None
    result: str | None = None
    score_margin: float | None = None

    @property
    def is_duel(self) -> bool:
        return self.match_id is not None

    @property
    def song_key(self) -> str:
        return str(self.song_id) if self.song_id is not None else self.event_key


@dataclass(frozen=True)
class ComponentResult:
    value: float
    events: int
    confidence: float


@dataclass(frozen=True)
class SkillsResult:
    total: float
    accuracy: ComponentResult
    challenge: ComponentResult
    score: ComponentResult
    duel: ComponentResult
    progression: ComponentResult
    confidence: float
    provisional: bool
    events_counted: int
    duel_matches: int
    accuracy_percent: float
    duel_wins: int
    duel_losses: int
    duel_draws: int
    duel_win_rate: float

    def component_values(self) -> dict[str, Any]:
        return {
            "guitar_skills": self.total,
            "accuracy": self.accuracy.value,
            "challenge": self.challenge.value,
            "score": self.score.value,
            "duel": self.duel.value,
            "progression": self.progression.value,
            "confidence": round(self.confidence, 4),
            "provisional": self.provisional,
            "events_counted": self.events_counted,
            "duel_matches": self.duel_matches,
        }


def _recency_weight(index: int, config: SkillsConfig) -> float:
    return 2.0 ** (-float(index) / float(config.recency_decay))


def _difficulty_factor(difficulty: int | None, config: SkillsConfig) -> float:
    """Tier multiplier; an unknown difficulty is neutral (1.0)."""
    if difficulty is None:
        return 1.0
    value = int(difficulty)
    if value <= int(config.difficulty_easy_max):
        return float(config.difficulty_easy_factor)
    if value <= int(config.difficulty_normal_max):
        return float(config.difficulty_normal_factor)
    return float(config.difficulty_hard_factor)


def _note_factor(note_count: int | None, config: SkillsConfig) -> float:
    """``clamp(sqrt(note_count / median), min, max)``; unknown stays neutral."""
    reference = config.median_note_count
    if note_count is None or int(note_count) <= 0:
        return 1.0
    if reference is None or int(reference) <= 0:
        return 1.0
    value = math.sqrt(float(note_count) / float(reference))
    return _clamp(
        float(config.note_factor_min),
        float(config.note_factor_max),
        value,
    )


def _cap_song_weights(
    weights: Sequence[float],
    song_keys: Sequence[str],
    cap_multiplier: float,
) -> list[float]:
    """Clamp one song's summed weight to ``cap * average attempt weight``.

    Nothing is redistributed: when a song exceeds the cap, every event of that
    song is scaled by the same factor so its internal proportions (and therefore
    the weighted average) stay meaningful.
    """
    total = float(sum(weights))
    if total <= 0.0 or not weights:
        return list(weights)
    average = total / float(len(weights))
    cap = max(0.0, float(cap_multiplier)) * average
    per_song: dict[str, float] = {}
    for weight, key in zip(weights, song_keys):
        per_song[key] = per_song.get(key, 0.0) + float(weight)
    scales = {
        key: min(1.0, cap / summed) if summed > 0.0 else 1.0
        for key, summed in per_song.items()
    }
    return [
        float(weight) * scales.get(key, 1.0)
        for weight, key in zip(weights, song_keys)
    ]


def _weighted_average(values: Sequence[float], weights: Sequence[float]) -> float:
    denominator = float(sum(weights))
    if denominator <= 0.0:
        return 0.0
    return float(sum(value * weight for value, weight in zip(values, weights))) / (
        denominator
    )


def _normalized_level(level: int | None, config: SkillsConfig) -> float:
    maximum = max(1, int(config.level_normalization_max))
    return 100.0 * _clamp(0.0, 1.0, float(level or 0) / float(maximum))


def _strength(
    guitar_skills: float,
    level: int | None,
    config: SkillsConfig,
) -> float:
    return (
        float(config.duel_strength_guitar_weight) * float(guitar_skills)
        + float(config.duel_strength_level_weight) * _normalized_level(level, config)
    )


def _duel_outcome(result: str | None) -> float:
    if result == "win":
        return 1.0
    if result == "draw":
        return 0.5
    return 0.0


def _duel_strength_pair(
    event: EventRecord, config: SkillsConfig, own_level: int | None
) -> tuple[float, float, bool]:
    """``(player_strength, opponent_strength, pre_match_skills_known)``.

    Historical matches have no frozen Guitar Skills. Both sides then use the
    documented Elo-rating proxy ``100 * clamp(rating_before / 2000, 0, 1)`` and
    the caller lowers the duel confidence. The player's own level is the
    current account level (it is not frozen per match), the opponent level is
    frozen on the event.
    """
    known = (
        event.player_pre_match_guitar_skills is not None
        and event.opponent_pre_match_guitar_skills is not None
    )
    if known:
        player_skills = float(event.player_pre_match_guitar_skills)  # type: ignore[arg-type]
        opponent_skills = float(event.opponent_pre_match_guitar_skills)  # type: ignore[arg-type]
    else:
        player_skills = 100.0 * _clamp(
            0.0,
            1.0,
            float(event.player_pre_match_rating or duel_rules.ELO_START_RATING)
            / 2000.0,
        )
        opponent_skills = 100.0 * _clamp(
            0.0,
            1.0,
            float(event.opponent_pre_match_rating or duel_rules.ELO_START_RATING)
            / 2000.0,
        )
    player = _strength(player_skills, own_level, config)
    opponent = _strength(opponent_skills, event.opponent_level, config)
    return player, opponent, known


def compute_skills(
    events: Sequence[EventRecord],
    *,
    level: int,
    xp_total: int,
    config: SkillsConfig,
    player_level: int | None = None,
    total_events: int | None = None,
    duel_record: Mapping[str, int] | None = None,
    duel_events: Sequence[EventRecord] | None = None,
) -> SkillsResult:
    """Compute the five components from an already bounded event window.

    ``events`` must be ordered newest first and may contain solo and duel rows;
    it feeds accuracy, challenge and score. ``duel_events`` is the separate
    newest-first duel window (the caller bounds it by the same ``window_size``);
    when omitted the duel rows of ``events`` are used. ``player_level`` defaults
    to ``level`` and is used as the player's own normalized level in the duel
    strength (the opponent level is frozen per event).
    """
    level = max(1, int(level or 1))
    xp_total = max(0, int(xp_total or 0))
    own_level = level if player_level is None else max(1, int(player_level))
    events = list(events)

    # --- Accuracy (A) and Challenge (C) --------------------------------
    accuracy_weights: list[float] = []
    challenge_values: list[float] = []
    challenge_weights: list[float] = []
    accuracy_values: list[float] = []
    song_keys: list[str] = []
    raw_accuracies: list[float] = []
    for index, event in enumerate(events):
        accuracy_fraction = _clamp(0.0, 1.0, float(event.accuracy) / 100.0)
        difficulty = _difficulty_factor(event.difficulty, config)
        note = _note_factor(event.note_count, config)
        weight = _recency_weight(index, config) * difficulty * note
        accuracy_weights.append(weight)
        accuracy_values.append(accuracy_fraction)
        challenge_values.append(accuracy_fraction * difficulty * note / 1.25)
        challenge_weights.append(weight)
        song_keys.append(event.song_key)
        raw_accuracies.append(float(event.accuracy))

    capped_weights = _cap_song_weights(
        accuracy_weights, song_keys, config.per_song_attempt_cap
    )
    if events:
        accuracy_value = 100.0 * _weighted_average(accuracy_values, capped_weights)
        challenge_value = 100.0 * _weighted_average(
            challenge_values, capped_weights
        )
        accuracy_component = ComponentResult(
            value=round1(_clamp(0.0, 100.0, accuracy_value)),
            events=len(events),
            confidence=min(
                1.0, len(events) / float(max(1, config.confidence_expected_events))
            ),
        )
        challenge_component = ComponentResult(
            value=round1(_clamp(0.0, 100.0, challenge_value)),
            events=len(events),
            confidence=min(
                1.0, len(events) / float(max(1, config.confidence_expected_events))
            ),
        )
    else:
        accuracy_component = ComponentResult(NEUTRAL, 0, 0.0)
        challenge_component = ComponentResult(NEUTRAL, 0, 0.0)

    # --- Score efficiency (S) ------------------------------------------
    score_values: list[float] = []
    score_weights: list[float] = []
    score_songs: list[str] = []
    for index, event in enumerate(events):
        if event.note_count is None or int(event.note_count) <= 0:
            # An unknown chart length is never guessed; the event is skipped.
            continue
        maximum = float(event.note_count) * 100.0
        efficiency = _clamp(0.0, 1.0, float(event.score) / maximum)
        score_values.append(efficiency)
        score_weights.append(_recency_weight(index, config))
        score_songs.append(event.song_key)
    if score_values:
        capped = _cap_song_weights(
            score_weights, score_songs, config.per_song_attempt_cap
        )
        score_component = ComponentResult(
            value=round1(
                _clamp(0.0, 100.0, 100.0 * _weighted_average(score_values, capped))
            ),
            events=len(score_values),
            confidence=min(
                1.0,
                len(score_values)
                / float(max(1, config.confidence_expected_events)),
            ),
        )
    else:
        score_component = ComponentResult(NEUTRAL, 0, 0.0)

    # --- Duel performance (D) ------------------------------------------
    duels = (
        list(duel_events)
        if duel_events is not None
        else [event for event in events if event.is_duel]
    )
    if duels:
        deltas: list[float] = []
        weights: list[float] = []
        known_weight = 0.0
        for index, event in enumerate(duels):
            player_strength, opponent_strength, known = _duel_strength_pair(
                event, config, own_level
            )
            expected = 1.0 / (
                1.0
                + 10.0
                ** (
                    (opponent_strength - player_strength)
                    / float(config.expected_score_scale)
                )
            )
            if event.score_margin is not None:
                margin = _clamp(0.0, 1.0, float(event.score_margin))
            else:
                margin = 0.5
            actual = 0.70 * _duel_outcome(event.result) + 0.30 * margin
            weight = _recency_weight(index, config)
            deltas.append(actual - expected)
            weights.append(weight)
            known_weight += weight if known else 0.5 * weight
        average_delta = _weighted_average(deltas, weights)
        reliability = len(duels) / (
            len(duels) + float(config.reliability_constant)
        )
        duel_value = _clamp(
            0.0, 100.0, 50.0 + 80.0 * average_delta * reliability
        )
        denominator = float(sum(weights)) or 1.0
        duel_component = ComponentResult(
            value=round1(duel_value),
            events=len(duels),
            confidence=min(
                1.0,
                known_weight
                / denominator
                * len(duels)
                / float(max(1, config.confidence_expected_duels)),
            ),
        )
    else:
        duel_component = ComponentResult(NEUTRAL, 0, 0.0)

    # --- Progression (P) -----------------------------------------------
    level_score = 100.0 * _clamp(
        0.0, 1.0, float(level) / float(max(1, config.level_normalization_max))
    )
    xp_score = 100.0 * _clamp(
        0.0, 1.0, float(xp_total) / float(max(1, config.xp_reference))
    )
    progression_component = ComponentResult(
        value=round1(_clamp(0.0, 100.0, 0.70 * level_score + 0.30 * xp_score)),
        events=1,
        confidence=1.0,
    )

    # --- Total, confidence, provisional --------------------------------
    total = (
        float(config.accuracy_weight) * accuracy_component.value
        + float(config.challenge_weight) * challenge_component.value
        + float(config.score_weight) * score_component.value
        + float(config.duel_weight) * duel_component.value
        + float(config.progression_weight) * progression_component.value
    )
    confidence = (
        float(config.accuracy_weight) * accuracy_component.confidence
        + float(config.challenge_weight) * challenge_component.confidence
        + float(config.score_weight) * score_component.confidence
        + float(config.duel_weight) * duel_component.confidence
        + float(config.progression_weight) * progression_component.confidence
    )
    counted = int(total_events if total_events is not None else len(events))
    record = dict(duel_record or {})
    wins = int(record.get("wins", 0))
    losses = int(record.get("losses", 0))
    draws = int(record.get("draws", 0))
    matches = int(record.get("matches", wins + losses + draws))
    provisional = (
        confidence < float(config.provisional_threshold)
        or counted < int(config.min_ranked_events)
    )
    return SkillsResult(
        total=round1(_clamp(0.0, 100.0, total)),
        accuracy=accuracy_component,
        challenge=challenge_component,
        score=score_component,
        duel=duel_component,
        progression=progression_component,
        confidence=round(_clamp(0.0, 1.0, confidence), 4),
        provisional=provisional,
        events_counted=counted,
        duel_matches=matches,
        accuracy_percent=(
            round(sum(raw_accuracies) / len(raw_accuracies), 2)
            if raw_accuracies
            else 0.0
        ),
        duel_wins=wins,
        duel_losses=losses,
        duel_draws=draws,
        duel_win_rate=round(100.0 * wins / matches, 2) if matches else 0.0,
    )


# ---------------------------------------------------------------------------
# Event construction
# ---------------------------------------------------------------------------
def _event_record(row: GuitarSkillsEvent) -> EventRecord:
    return EventRecord(
        event_key=row.event_key,
        song_id=row.song_id,
        accuracy=float(row.accuracy),
        score=int(row.score or 0),
        note_count=int(row.note_count) if row.note_count is not None else None,
        difficulty=int(row.difficulty) if row.difficulty is not None else None,
        occurred_at=row.occurred_at,
        mode=row.mode,
        match_id=row.match_id,
        opponent_id=row.opponent_id,
        opponent_level=(
            int(row.opponent_level) if row.opponent_level is not None else None
        ),
        opponent_pre_match_rating=row.opponent_pre_match_rating,
        opponent_pre_match_guitar_skills=row.opponent_pre_match_guitar_skills,
        player_pre_match_rating=row.player_pre_match_rating,
        player_pre_match_guitar_skills=row.player_pre_match_guitar_skills,
        result=row.result,
        score_margin=row.score_margin,
    )


def _duel_score(participant: DuelParticipant) -> int:
    if participant.score_points is not None:
        return max(0, int(participant.score_points))
    return max(
        0,
        int(participant.perfect_count or 0) * 100
        + int(participant.great_count or 0) * 85
        + int(participant.good_count or 0) * 70,
    )


def _duel_event_values(
    *,
    match: DuelMatch,
    own: DuelParticipant,
    other: DuelParticipant,
    own_level: int | None,
    other_level: int | None,
    song_difficulty: int | None,
    occurred_at: datetime,
) -> dict[str, Any]:
    """One ``guitar_skills_events`` row for ``own`` in a finished duel."""
    notes = max(0, int(own.notes_played or 0))
    accuracy = (
        duel_rules.accuracy_factor(
            int(own.perfect_count or 0),
            int(own.great_count or 0),
            int(own.good_count or 0),
            notes,
        )
        * 100.0
    )
    my_score = _duel_score(own)
    other_score = _duel_score(other)
    margin = (
        my_score / float(my_score + other_score)
        if (my_score + other_score) > 0
        else 0.5
    )
    # Chart length first; a chart snapshot without a length falls back to the
    # notes this side actually resolved, never to an invented number.
    note_count = int(match.note_count or 0) or notes or None
    return {
        "event_key": f"duel:{match.id}:{own.user_id}",
        "user_id": own.user_id,
        "match_id": match.id,
        "song_id": match.song_id,
        "mode": DUEL_MODE,
        "accuracy": round(_clamp(0.0, 100.0, accuracy), 4),
        "score": my_score,
        "note_count": note_count,
        "difficulty": int(song_difficulty) if song_difficulty is not None else None,
        "opponent_id": other.user_id,
        "opponent_level": int(other_level) if other_level is not None else None,
        "opponent_pre_match_rating": other.rating_before,
        "opponent_pre_match_guitar_skills": other.guitar_skills_before,
        "player_pre_match_rating": own.rating_before,
        "player_pre_match_guitar_skills": own.guitar_skills_before,
        "result": own.result,
        "score_margin": round(_clamp(0.0, 1.0, margin), 6),
        "occurred_at": occurred_at,
    }


def record_solo_event(
    db: Session,
    *,
    session: GameSession,
    score: Score,
    song: Song,
) -> bool:
    """Store the ranked event of one finished solo attempt (idempotent)."""
    if session.mode not in SOLO_MODES:
        return False
    completion = float(score.completion_percent or 0.0)
    if completion < MIN_RANKED_COMPLETION:
        return False
    occurred_at = _aware(session.finished_at) or _aware(session.started_at) or _utcnow()
    values = {
        "event_key": f"solo:{session.id}",
        "user_id": session.user_id,
        "match_id": None,
        "song_id": session.song_id,
        "mode": session.mode,
        "accuracy": round(_clamp(0.0, 100.0, float(score.accuracy_percent)), 4),
        "score": max(0, int(score.score_points or 0)),
        "note_count": (
            int(score.notes_total) if score.notes_total and score.notes_total > 0 else None
        ),
        "difficulty": int(song.difficulty) if song.difficulty is not None else None,
        "occurred_at": occurred_at,
    }
    inserted = db.execute(
        pg_insert(GuitarSkillsEvent.__table__)
        .values(**values)
        .on_conflict_do_nothing(index_elements=["event_key"])
    ).rowcount
    return bool(inserted)


def record_duel_events(
    db: Session,
    *,
    match: DuelMatch,
    player: DuelParticipant,
    opponent: DuelParticipant,
    player_level: int | None,
    opponent_level: int | None,
    song_difficulty: int | None,
    occurred_at: datetime,
) -> int:
    """Store both sides of one finished online duel (idempotent)."""
    rows = [
        _duel_event_values(
            match=match,
            own=player,
            other=opponent,
            own_level=player_level,
            other_level=opponent_level,
            song_difficulty=song_difficulty,
            occurred_at=occurred_at,
        ),
        _duel_event_values(
            match=match,
            own=opponent,
            other=player,
            own_level=opponent_level,
            other_level=player_level,
            song_difficulty=song_difficulty,
            occurred_at=occurred_at,
        ),
    ]
    inserted = 0
    for row in rows:
        inserted += int(
            db.execute(
                pg_insert(GuitarSkillsEvent.__table__)
                .values(**row)
                .on_conflict_do_nothing(index_elements=["event_key"])
            ).rowcount
            or 0
        )
    return inserted


def _pre_match_skills(db: Session, user_id: UUID) -> float:
    row = db.get(UserGuitarSkills, user_id)
    if row is None:
        return INITIAL_SKILLS
    return float(row.guitar_skills)


def on_duel_finalized(
    db: Session,
    match: DuelMatch,
    participants: Mapping[str, DuelParticipant],
    *,
    now: datetime | None = None,
) -> None:
    """Freeze both pre-match values, store events and recalculate both players.

    Runs inside the duel finalisation transaction. Bot matches and matches
    without two real humans are ignored.
    """
    if match.mode != "online":
        return
    player = participants.get("player")
    opponent = participants.get("opponent")
    if player is None or opponent is None:
        return
    if player.is_bot or opponent.is_bot:
        return
    if player.user_id is None or opponent.user_id is None:
        return
    if player.result is None or opponent.result is None:
        return
    if player.rating_before is None or opponent.rating_before is None:
        return

    now = now or _utcnow()
    # Snapshot both ratings *before* any recalculation of this match.
    player.guitar_skills_before = _pre_match_skills(db, player.user_id)
    opponent.guitar_skills_before = _pre_match_skills(db, opponent.user_id)
    db.flush()

    song = db.get(Song, match.song_id)
    player_level = db.scalar(select(User.level).where(User.id == player.user_id))
    opponent_level = db.scalar(
        select(User.level).where(User.id == opponent.user_id)
    )
    record_duel_events(
        db,
        match=match,
        player=player,
        opponent=opponent,
        player_level=int(player_level) if player_level is not None else None,
        opponent_level=(
            int(opponent_level) if opponent_level is not None else None
        ),
        song_difficulty=int(song.difficulty) if song is not None else None,
        occurred_at=_aware(match.finished_at) or now,
    )
    db.flush()

    config = config_snapshot(get_config(db))
    # A stable order avoids two simultaneous matches updating the same two
    # user_guitar_skills rows in opposite order.
    for user_id in sorted({player.user_id, opponent.user_id}, key=str):
        recalculate_user(
            db, user_id, reason="duel_finish", config=config
        )


# ---------------------------------------------------------------------------
# Recalculation
# ---------------------------------------------------------------------------
def _window_events(
    db: Session, user_id: UUID, config: SkillsConfig, *, duels_only: bool = False
) -> list[GuitarSkillsEvent]:
    stmt = select(GuitarSkillsEvent).where(GuitarSkillsEvent.user_id == user_id)
    if duels_only:
        stmt = stmt.where(GuitarSkillsEvent.match_id.isnot(None))
    stmt = stmt.order_by(
        GuitarSkillsEvent.occurred_at.desc(), GuitarSkillsEvent.id.desc()
    ).limit(int(config.window_size))
    return list(db.scalars(stmt).all())


def _compute_for_user(
    db: Session, user: User, config: SkillsConfig
) -> SkillsResult:
    window = _window_events(db, user.id, config)
    duel_window = _window_events(db, user.id, config, duels_only=True)
    total_events = int(
        db.scalar(
            select(func.count(GuitarSkillsEvent.id)).where(
                GuitarSkillsEvent.user_id == user.id
            )
        )
        or 0
    )
    record_rows = db.execute(
        select(GuitarSkillsEvent.result, func.count(GuitarSkillsEvent.id))
        .where(
            GuitarSkillsEvent.user_id == user.id,
            GuitarSkillsEvent.match_id.isnot(None),
        )
        .group_by(GuitarSkillsEvent.result)
    ).all()
    record = {"wins": 0, "losses": 0, "draws": 0, "matches": 0}
    for result, count in record_rows:
        value = int(count or 0)
        record["matches"] += value
        if result == "win":
            record["wins"] += value
        elif result == "loss":
            record["losses"] += value
        elif result == "draw":
            record["draws"] += value
    return compute_skills(
        [_event_record(row) for row in window],
        level=int(user.level or 1),
        xp_total=int(user.xp_total or 0),
        config=config,
        player_level=int(user.level or 1),
        total_events=total_events,
        duel_record=record,
        duel_events=[_event_record(row) for row in duel_window],
    )


def _ranked_player_predicate():
    """SQL predicate: the active user has at least one ranked event.

    The public board ranks only players who actually produced a ranked result.
    Never-played accounts, practice/preview-only accounts and admins still get a
    neutral snapshot row (so the admin panel can report coverage), but they are
    not part of the ranking.
    """
    return exists(
        select(GuitarSkillsEvent.id).where(
            GuitarSkillsEvent.user_id == User.id
        )
    )


def _user_has_ranked_events(db: Session, user_id: UUID) -> bool:
    return bool(
        db.scalar(
            select(func.count(GuitarSkillsEvent.id)).where(
                GuitarSkillsEvent.user_id == user_id
            )
        )
        or 0
    )


def _ranked_players_count(db: Session) -> int:
    return int(
        db.scalar(
            select(func.count(User.id)).where(
                User.is_active.is_(True), _ranked_player_predicate()
            )
        )
        or 0
    )


def rank_for_user(db: Session, user_id: UUID) -> int | None:
    """Live rank of one player using the public leaderboard ordering.

    Only players with at least one ranked event occupy a real position. An
    account that never produced a ranked result (fresh signup, practice-only,
    preview-only, admin) is placed after every ranked player, tied with the
    other unranked accounts, and is not part of the public board.
    """
    row = db.get(UserGuitarSkills, user_id)
    user = db.get(User, user_id)
    if row is None or user is None:
        return None
    if not _user_has_ranked_events(db, user_id):
        return _ranked_players_count(db) + 1
    better = or_(
        UserGuitarSkills.guitar_skills > row.guitar_skills,
        and_(
            UserGuitarSkills.guitar_skills == row.guitar_skills,
            UserGuitarSkills.duel_performance > row.duel_performance,
        ),
        and_(
            UserGuitarSkills.guitar_skills == row.guitar_skills,
            UserGuitarSkills.duel_performance == row.duel_performance,
            User.xp_total > int(user.xp_total or 0),
        ),
        and_(
            UserGuitarSkills.guitar_skills == row.guitar_skills,
            UserGuitarSkills.duel_performance == row.duel_performance,
            User.xp_total == int(user.xp_total or 0),
            User.username < user.username,
        ),
    )
    count = db.scalar(
        select(func.count())
        .select_from(UserGuitarSkills)
        .join(User, User.id == UserGuitarSkills.user_id)
        .where(User.is_active.is_(True), _ranked_player_predicate(), better)
    )
    return int(count or 0) + 1


def _apply_result(
    db: Session,
    *,
    user: User,
    result: SkillsResult,
    existing: UserGuitarSkills | None,
    rank: int,
    previous_rank: int | None,
    config: SkillsConfig,
    reason: str,
) -> bool:
    now = _utcnow()
    old_value = float(existing.guitar_skills) if existing is not None else None
    old_rank = existing.rank if existing is not None else None
    if existing is None:
        existing = UserGuitarSkills(user_id=user.id)
        db.add(existing)

    existing.guitar_skills = Decimal(str(result.total))
    existing.accuracy_score = Decimal(str(result.accuracy.value))
    existing.challenge_score = Decimal(str(result.challenge.value))
    existing.score_performance = Decimal(str(result.score.value))
    existing.duel_performance = Decimal(str(result.duel.value))
    existing.progression_score = Decimal(str(result.progression.value))
    existing.confidence = float(result.confidence)
    existing.provisional = bool(result.provisional)
    existing.events_counted = int(result.events_counted)
    existing.duel_matches = int(result.duel_matches)
    existing.accuracy_percent = float(result.accuracy_percent)
    existing.duel_wins = int(result.duel_wins)
    existing.duel_losses = int(result.duel_losses)
    existing.duel_draws = int(result.duel_draws)
    existing.duel_win_rate = float(result.duel_win_rate)
    existing.accuracy_events = int(result.accuracy.events)
    existing.challenge_events = int(result.challenge.events)
    existing.score_events = int(result.score.events)
    existing.duel_events = int(result.duel.events)
    existing.progression_events = int(result.progression.events)
    existing.accuracy_confidence = float(result.accuracy.confidence)
    existing.challenge_confidence = float(result.challenge.confidence)
    existing.score_confidence = float(result.score.confidence)
    existing.duel_confidence = float(result.duel.confidence)
    existing.progression_confidence = float(result.progression.confidence)
    existing.rank = int(rank)
    existing.previous_rank = previous_rank if previous_rank is not None else old_rank
    existing.calculation_version = int(config.calculation_version)
    existing.last_calculated_at = now
    db.flush()

    changed = old_value is None or round1(old_value) != result.total
    if changed:
        db.add(
            GuitarSkillsHistory(
                user_id=user.id,
                old_guitar_skills=(
                    Decimal(str(round1(old_value)))
                    if old_value is not None
                    else None
                ),
                new_guitar_skills=Decimal(str(result.total)),
                old_rank=old_rank,
                new_rank=int(rank),
                component_values=result.component_values(),
                calculation_version=int(config.calculation_version),
                reason=str(reason)[:64],
            )
        )
        db.flush()
    return changed


def recalculate_user(
    db: Session,
    user_id: UUID,
    reason: str = "manual",
    config: SkillsConfig | None = None,
) -> UserGuitarSkills:
    """Recalculate one player from the bounded event window (fast, idempotent)."""
    user = db.get(User, user_id)
    if user is None:
        raise ValueError(f"User {user_id} not found")
    snapshot = config or config_snapshot(get_config(db))
    result = _compute_for_user(db, user, snapshot)
    existing = db.get(UserGuitarSkills, user_id)
    previous_rank = existing.rank if existing is not None else None
    if existing is None:
        existing = UserGuitarSkills(user_id=user_id)
        db.add(existing)
    # Set the value first so the live rank sees this player's new total.
    existing.guitar_skills = Decimal(str(result.total))
    existing.duel_performance = Decimal(str(result.duel.value))
    db.flush()
    rank = rank_for_user(db, user_id) or 1
    _apply_result(
        db,
        user=user,
        result=result,
        existing=existing,
        rank=rank,
        previous_rank=previous_rank,
        config=snapshot,
        reason=reason,
    )
    return existing


def _sort_key(result: SkillsResult, user: User) -> tuple:
    return (
        -float(result.total),
        -float(result.duel.value),
        -int(user.xp_total or 0),
        user.username,
    )


def recalculate_all(
    db: Session,
    reason: str = "manual",
    dry_run: bool = False,
) -> dict[str, Any]:
    """Recalculate every active player and (optionally) persist the result.

    Dry run computes the new ranking without writing user values or history; it
    returns at most the 100 largest absolute changes for the admin preview.
    """
    config_row = get_config(db)
    config = config_snapshot(config_row)
    users = list(
        db.scalars(
            select(User).where(User.is_active.is_(True)).order_by(User.username)
        ).all()
    )
    computed: list[tuple[User, SkillsResult, UserGuitarSkills | None]] = []
    for user in users:
        computed.append(
            (user, _compute_for_user(db, user, config), db.get(UserGuitarSkills, user.id))
        )
    computed.sort(key=lambda item: _sort_key(item[1], item[0]))

    # Only players with at least one ranked event get a real leaderboard
    # position; everyone else is stored at the bottom, tied with the other
    # unranked accounts (``_ranked_players_count() + 1``), and is excluded from
    # the public board.
    ranked_ids = set(
        db.scalars(select(GuitarSkillsEvent.user_id).distinct()).all()
    )
    unranked_position = (
        sum(1 for user in users if user.id in ranked_ids) + 1
    )

    changes: list[dict[str, Any]] = []
    position = 0
    for user, result, existing in computed:
        if user.id in ranked_ids:
            position += 1
            rank = position
        else:
            rank = unranked_position
        old_value = float(existing.guitar_skills) if existing is not None else None
        old_rank = existing.rank if existing is not None else None
        if dry_run:
            delta = (
                result.total - round1(old_value)
                if old_value is not None
                else result.total
            )
            if (
                old_value is None
                or round1(old_value) != result.total
                or old_rank != rank
            ):
                changes.append(
                    {
                        "user_id": user.id,
                        "username": user.username,
                        "old_skills": (
                            round1(old_value) if old_value is not None else None
                        ),
                        "new_skills": result.total,
                        "old_rank": old_rank,
                        "new_rank": rank,
                        "delta": round(delta, 2),
                    }
                )
            continue
        changed = _apply_result(
            db,
            user=user,
            result=result,
            existing=existing,
            rank=rank,
            previous_rank=old_rank,
            config=config,
            reason=reason,
        )
        if changed:
            changes.append(
                {
                    "user_id": user.id,
                    "username": user.username,
                    "old_skills": (
                        round1(old_value) if old_value is not None else None
                    ),
                    "new_skills": result.total,
                    "old_rank": old_rank,
                    "new_rank": rank,
                    "delta": (
                        round(result.total - round1(old_value), 2)
                        if old_value is not None
                        else result.total
                    ),
                }
            )

    if not dry_run:
        db.commit()
    changes.sort(key=lambda item: -abs(float(item["delta"])))
    return {
        "users": len(users),
        "changed": len(changes),
        "items": changes[:100],
    }


# ---------------------------------------------------------------------------
# Backfill
# ---------------------------------------------------------------------------
def _chunks(items: Sequence[dict[str, Any]], size: int = 500) -> Iterable[Sequence[dict[str, Any]]]:
    for start in range(0, len(items), size):
        yield items[start : start + size]


def _insert_events(db: Session, rows: Sequence[dict[str, Any]]) -> int:
    """Insert event rows, counting only the ones that were really added.

    psycopg3 leaves ``rowcount`` at ``-1`` after ``executemany()``, so the
    count comes from ``RETURNING`` instead: ``ON CONFLICT DO NOTHING`` returns
    just the inserted rows.
    """
    inserted = 0
    for chunk in _chunks(rows):
        result = db.execute(
            pg_insert(GuitarSkillsEvent.__table__)
            .on_conflict_do_nothing(index_elements=["event_key"])
            .returning(GuitarSkillsEvent.__table__.c.id),
            list(chunk),
        )
        inserted += len(result.all())
    return inserted


def backfill_events(db: Session) -> dict[str, int]:
    """Rebuild the ranked event log from the existing history (idempotent).

    * solo: finished ``normal``/``daily`` sessions with a score and at least
      80% completion (practice/preview are excluded);
    * duels: finished ``online`` matches with two real human participants whose
      result and Elo snapshot are set (bot and unfinished matches excluded).

    Existing events are never updated or deleted; the unique ``event_key``
    makes a repeated run a no-op.
    """
    get_config(db)

    solo_rows = db.execute(
        select(
            GameSession.id,
            GameSession.user_id,
            GameSession.song_id,
            GameSession.mode,
            GameSession.started_at,
            GameSession.finished_at,
            Score.accuracy_percent,
            Score.score_points,
            Score.notes_total,
            Song.difficulty,
        )
        .join(Score, Score.session_id == GameSession.id)
        .join(Song, Song.id == GameSession.song_id)
        .where(
            GameSession.mode.in_(SOLO_MODES),
            GameSession.status == "finished",
            Score.completion_percent >= MIN_RANKED_COMPLETION,
        )
    ).all()
    solo_values: list[dict[str, Any]] = []
    for row in solo_rows:
        occurred = _aware(row.finished_at) or _aware(row.started_at) or _utcnow()
        solo_values.append(
            {
                "event_key": f"solo:{row.id}",
                "user_id": row.user_id,
                "match_id": None,
                "song_id": row.song_id,
                "mode": row.mode,
                "accuracy": round(
                    _clamp(0.0, 100.0, float(row.accuracy_percent)), 4
                ),
                "score": max(0, int(row.score_points or 0)),
                "note_count": (
                    int(row.notes_total)
                    if row.notes_total is not None and int(row.notes_total) > 0
                    else None
                ),
                "difficulty": (
                    int(row.difficulty) if row.difficulty is not None else None
                ),
                "occurred_at": occurred,
            }
        )
    solo_inserted = _insert_events(db, solo_values)

    player = aliased(DuelParticipant)
    opponent = aliased(DuelParticipant)
    player_user = aliased(User)
    opponent_user = aliased(User)
    duel_rows = db.execute(
        select(
            DuelMatch,
            player,
            opponent,
            player_user.level,
            opponent_user.level,
            Song.difficulty,
        )
        .join(
            player,
            and_(player.match_id == DuelMatch.id, player.side == "player"),
        )
        .join(
            opponent,
            and_(opponent.match_id == DuelMatch.id, opponent.side == "opponent"),
        )
        .join(player_user, player_user.id == player.user_id)
        .join(opponent_user, opponent_user.id == opponent.user_id)
        .join(Song, Song.id == DuelMatch.song_id)
        .where(
            DuelMatch.mode == "online",
            DuelMatch.status == "finished",
            player.user_id.isnot(None),
            opponent.user_id.isnot(None),
            player.is_bot.is_(False),
            opponent.is_bot.is_(False),
            player.result.isnot(None),
            opponent.result.isnot(None),
            player.rating_before.isnot(None),
            opponent.rating_before.isnot(None),
        )
    ).all()
    duel_values: list[dict[str, Any]] = []
    for match, player_row, opponent_row, player_level, opponent_level, difficulty in duel_rows:
        occurred = _aware(match.finished_at) or _aware(match.created_at) or _utcnow()
        for own, other, own_level, other_level in (
            (player_row, opponent_row, player_level, opponent_level),
            (opponent_row, player_row, opponent_level, player_level),
        ):
            duel_values.append(
                _duel_event_values(
                    match=match,
                    own=own,
                    other=other,
                    own_level=int(own_level) if own_level is not None else None,
                    other_level=(
                        int(other_level) if other_level is not None else None
                    ),
                    song_difficulty=(
                        int(difficulty) if difficulty is not None else None
                    ),
                    occurred_at=occurred,
                )
            )
    duel_inserted = _insert_events(db, duel_values)
    db.commit()
    return {
        "solo": solo_inserted,
        "duel": duel_inserted,
        "total": solo_inserted + duel_inserted,
    }


# ---------------------------------------------------------------------------
# Leaderboard and player detail
# ---------------------------------------------------------------------------
def _legacy_rating_subquery():
    """Legacy per-player rating keys (best score per song, summed)."""
    best = (
        select(
            GameSession.user_id.label("user_id"),
            GameSession.song_id.label("song_id"),
            func.max(Score.score_points).label("best_score"),
            func.max(Score.accuracy_percent).label("best_accuracy"),
        )
        .join(Score, Score.session_id == GameSession.id)
        .join(Song, Song.id == GameSession.song_id)
        .where(
            GameSession.status == "finished",
            GameSession.mode == "normal",
            Song.daily_weekday.is_(None),
        )
        .group_by(GameSession.user_id, GameSession.song_id)
        .subquery()
    )
    return (
        select(
            best.c.user_id,
            func.coalesce(func.sum(best.c.best_score), 0).label("rating_points"),
            func.count(best.c.song_id).label("games_played"),
            func.max(best.c.best_accuracy).label("best_accuracy"),
        )
        .group_by(best.c.user_id)
        .subquery()
    )


def _ensure_user_rows(db: Session, config: SkillsConfig, user_id: UUID | None = None) -> int:
    """Create the neutral row of active players that have never been rated."""
    missing_stmt = select(User.id).where(
        User.is_active.is_(True),
        ~select(UserGuitarSkills.user_id)
        .where(UserGuitarSkills.user_id == User.id)
        .exists(),
    )
    if user_id is not None:
        missing_stmt = missing_stmt.where(User.id == user_id)
    missing = list(db.scalars(missing_stmt).all())
    for missing_id in missing:
        recalculate_user(db, missing_id, reason="lazy", config=config)
    if missing:
        db.commit()
    return len(missing)


def _leaderboard_item(
    user: User,
    row: UserGuitarSkills,
    *,
    rank: int,
    legacy: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    legacy = legacy or {}
    return {
        "rank": int(rank),
        "user_id": user.id,
        "username": user.username,
        "display_name": user.display_name or user.username,
        "avatar_image_url": user.avatar_image_url or None,
        "level": int(user.level or 1),
        "xp_total": int(user.xp_total or 0),
        "guitar_skills": round1(float(row.guitar_skills)),
        "accuracy_score": round1(float(row.accuracy_score)),
        "challenge_score": round1(float(row.challenge_score)),
        "score_performance": round1(float(row.score_performance)),
        "duel_performance": round1(float(row.duel_performance)),
        "progression_score": round1(float(row.progression_score)),
        "confidence": round(float(row.confidence or 0.0), 4),
        "provisional": bool(row.provisional),
        "accuracy_percent": round(float(row.accuracy_percent or 0.0), 2),
        "duel_wins": int(row.duel_wins or 0),
        "duel_losses": int(row.duel_losses or 0),
        "duel_draws": int(row.duel_draws or 0),
        "duel_win_rate": round(float(row.duel_win_rate or 0.0), 2),
        "duel_matches": int(row.duel_matches or 0),
        "position_change": (
            int(row.previous_rank) - int(rank)
            if row.previous_rank is not None
            else None
        ),
        "events_counted": int(row.events_counted or 0),
        "rating_points": int(legacy.get("rating_points") or 0),
        "games_played": int(legacy.get("games_played") or 0),
        "best_accuracy": round(float(legacy.get("best_accuracy") or 0.0), 2),
        "achievements_unlocked": int(legacy.get("achievements_unlocked") or 0),
    }


def _achievements_subquery():
    return (
        select(
            UserAchievement.user_id.label("user_id"),
            func.count(UserAchievement.id).label("achievements_unlocked"),
        )
        .group_by(UserAchievement.user_id)
        .subquery()
    )


def leaderboard(
    db: Session, limit: int = 10, offset: int = 0
) -> dict[str, Any]:
    """Public Guitar Skills leaderboard, including players with zero events."""
    config = config_snapshot(get_config(db))
    _ensure_user_rows(db, config)

    legacy = _legacy_rating_subquery()
    achievements = _achievements_subquery()
    stmt = (
        select(
            User,
            UserGuitarSkills,
            legacy.c.rating_points,
            legacy.c.games_played,
            legacy.c.best_accuracy,
            achievements.c.achievements_unlocked,
        )
        .join(UserGuitarSkills, UserGuitarSkills.user_id == User.id)
        .outerjoin(legacy, legacy.c.user_id == User.id)
        .outerjoin(achievements, achievements.c.user_id == User.id)
        .where(User.is_active.is_(True), _ranked_player_predicate())
        .order_by(
            UserGuitarSkills.guitar_skills.desc(),
            UserGuitarSkills.duel_performance.desc(),
            User.xp_total.desc(),
            User.username.asc(),
        )
        .limit(int(limit))
        .offset(int(offset))
    )
    rows = db.execute(stmt).all()
    total = int(
        db.scalar(
            select(func.count(User.id)).where(
                User.is_active.is_(True), _ranked_player_predicate()
            )
        )
        or 0
    )
    calculated_at = db.scalar(
        select(func.max(UserGuitarSkills.last_calculated_at))
        .join(User, User.id == UserGuitarSkills.user_id)
        .where(User.is_active.is_(True), _ranked_player_predicate())
    )
    items = [
        _leaderboard_item(
            row[0],
            row[1],
            rank=offset + index + 1,
            legacy={
                "rating_points": row[2],
                "games_played": row[3],
                "best_accuracy": row[4],
                "achievements_unlocked": row[5],
            },
        )
        for index, row in enumerate(rows)
    ]
    return {
        "items": items,
        "limit": int(limit),
        "offset": int(offset),
        "total": total,
        "calculated_at": calculated_at,
    }


def player_detail(db: Session, user_id: UUID) -> dict[str, Any] | None:
    """Public detail block of one player, or ``None`` for unknown/inactive."""
    user = db.scalar(
        select(User).where(User.id == user_id, User.is_active.is_(True))
    )
    if user is None:
        return None
    config = config_snapshot(get_config(db))
    _ensure_user_rows(db, config, user_id=user_id)
    row = db.get(UserGuitarSkills, user_id)
    if row is None:  # pragma: no cover - _ensure_user_rows guarantees it
        row = recalculate_user(db, user_id, reason="lazy_detail", config=config)
        db.commit()
    rank = rank_for_user(db, user_id) or int(row.rank or 1)
    legacy_query = _legacy_rating_subquery()
    achievements = _achievements_subquery()
    legacy_row = db.execute(
        select(
            legacy_query.c.rating_points,
            legacy_query.c.games_played,
            legacy_query.c.best_accuracy,
            achievements.c.achievements_unlocked,
        )
        .outerjoin(achievements, achievements.c.user_id == legacy_query.c.user_id)
        .where(legacy_query.c.user_id == user_id)
    ).first()
    legacy = (
        {
            "rating_points": legacy_row[0],
            "games_played": legacy_row[1],
            "best_accuracy": legacy_row[2],
            "achievements_unlocked": legacy_row[3],
        }
        if legacy_row is not None
        else None
    )
    if legacy is None:
        achievements_row = db.execute(
            select(achievements.c.achievements_unlocked).where(
                achievements.c.user_id == user_id
            )
        ).first()
        if achievements_row is not None:
            legacy = {"achievements_unlocked": achievements_row[0]}
    item = _leaderboard_item(user, row, rank=rank, legacy=legacy)
    item.update(
        {
            "last_calculated_at": row.last_calculated_at,
            "calculation_version": int(row.calculation_version or 1),
            "components": {
                "accuracy": {
                    "value": round1(float(row.accuracy_score)),
                    "events": int(row.accuracy_events or 0),
                    "confidence": round(float(row.accuracy_confidence or 0.0), 4),
                },
                "challenge": {
                    "value": round1(float(row.challenge_score)),
                    "events": int(row.challenge_events or 0),
                    "confidence": round(float(row.challenge_confidence or 0.0), 4),
                },
                "score": {
                    "value": round1(float(row.score_performance)),
                    "events": int(row.score_events or 0),
                    "confidence": round(float(row.score_confidence or 0.0), 4),
                },
                "duel": {
                    "value": round1(float(row.duel_performance)),
                    "events": int(row.duel_events or 0),
                    "confidence": round(float(row.duel_confidence or 0.0), 4),
                },
                "progression": {
                    "value": round1(float(row.progression_score)),
                    "events": int(row.progression_events or 0),
                    "confidence": round(float(row.progression_confidence or 0.0), 4),
                },
            },
        }
    )
    return item


# ---------------------------------------------------------------------------
# Admin helpers: status, config update, background recalculation
# ---------------------------------------------------------------------------
_recalc_lock = threading.Lock()
_recalc_state: dict[str, Any] = {
    "running": False,
    "started_at": None,
    "finished_at": None,
    "users": 0,
    "reason": None,
    "dry_run": False,
    "error": None,
}


def recalculation_running() -> bool:
    return bool(_recalc_state["running"])


def run_recalculation_job(
    reason: str = "config_change", dry_run: bool = False
) -> bool:
    """Run ``recalculate_all`` in its own session, guarded by a process lock."""
    if not _recalc_lock.acquire(blocking=False):
        logger.info("Guitar Skills recalculation already running; skipping")
        return False
    _recalc_state.update(
        {
            "running": True,
            "started_at": _utcnow(),
            "finished_at": None,
            "users": 0,
            "reason": reason,
            "dry_run": dry_run,
            "error": None,
        }
    )
    db = SessionLocal()
    try:
        summary = recalculate_all(db, reason=reason, dry_run=dry_run)
        _recalc_state["users"] = int(summary["users"])
        _recalc_state["changed"] = int(summary["changed"])
        logger.info(
            "Guitar Skills recalculation finished: %s users, %s changed",
            summary["users"],
            summary["changed"],
        )
        return True
    except Exception as exc:  # pragma: no cover - defensive
        db.rollback()
        _recalc_state["error"] = str(exc)
        logger.exception("Guitar Skills recalculation failed")
        return False
    finally:
        db.close()
        _recalc_state["running"] = False
        _recalc_state["finished_at"] = _utcnow()
        _recalc_lock.release()


def validate_config_values(values: Mapping[str, Any]) -> None:
    """Reject an inconsistent tuning row with a clear ``ValueError``."""
    weight_fields = (
        "accuracy_weight",
        "challenge_weight",
        "score_weight",
        "duel_weight",
        "progression_weight",
    )
    total = sum(float(values[field]) for field in weight_fields)
    if abs(total - 1.0) > 0.0001:
        raise ValueError("The five component weights must sum to 1.0")
    strength_total = float(values["duel_strength_guitar_weight"]) + float(
        values["duel_strength_level_weight"]
    )
    if abs(strength_total - 1.0) > 0.0001:
        raise ValueError("duel_strength_guitar_weight + duel_strength_level_weight must equal 1.0")
    if int(values["difficulty_normal_max"]) < int(values["difficulty_easy_max"]):
        raise ValueError("difficulty_normal_max must be >= difficulty_easy_max")
    if float(values["note_factor_max"]) < float(values["note_factor_min"]):
        raise ValueError("note_factor_max must be >= note_factor_min")
    if float(values["difficulty_easy_factor"]) > float(values["difficulty_normal_factor"]):
        raise ValueError("difficulty_easy_factor must not exceed difficulty_normal_factor")
    if float(values["difficulty_normal_factor"]) > float(values["difficulty_hard_factor"]):
        raise ValueError("difficulty_normal_factor must not exceed difficulty_hard_factor")
    if float(values["recency_decay"]) <= 0:
        raise ValueError("recency_decay must be positive")
    if float(values["expected_score_scale"]) <= 0:
        raise ValueError("expected_score_scale must be positive")
    if int(values["xp_reference"]) <= 0:
        raise ValueError("xp_reference must be positive")
    if int(values["window_size"]) < 1:
        raise ValueError("window_size must be at least 1")


def update_config(
    db: Session,
    values: Mapping[str, Any],
    *,
    admin_user_id: UUID | None,
) -> GuitarSkillsConfig:
    """Apply a validated configuration change and append its history row."""
    row = get_config(db)
    prospective = {
        field: values.get(field, getattr(row, field))
        for field in CONFIG_FIELDS
        if field != "calculation_version"
    }
    validate_config_values(prospective)
    for field in CONFIG_FIELDS:
        if field == "calculation_version":
            continue
        if field in values:
            setattr(row, field, values[field])
    row.calculation_version = int(row.calculation_version or 1) + 1
    row.updated_by = admin_user_id
    db.flush()
    snapshot = {
        **config_values(row),
        "updated_at": _utcnow().isoformat(),
    }
    db.add(
        GuitarSkillsConfigHistory(
            config=snapshot,
            admin_user_id=admin_user_id,
        )
    )
    db.commit()
    db.refresh(row)
    return row


def config_history(db: Session, limit: int = 50) -> list[dict[str, Any]]:
    rows = db.scalars(
        select(GuitarSkillsConfigHistory)
        .order_by(GuitarSkillsConfigHistory.created_at.desc())
        .limit(int(limit))
    ).all()
    return [
        {
            "id": row.id,
            "config": row.config,
            "admin_user_id": row.admin_user_id,
            "created_at": row.created_at,
        }
        for row in rows
    ]


def status(db: Session) -> dict[str, Any]:
    config_row = get_config(db)
    config = config_snapshot(config_row)
    active = User.is_active.is_(True)
    users = int(
        db.scalar(
            select(func.count())
            .select_from(UserGuitarSkills)
            .join(User, User.id == UserGuitarSkills.user_id)
            .where(active)
        )
        or 0
    )
    provisional = int(
        db.scalar(
            select(func.count())
            .select_from(UserGuitarSkills)
            .join(User, User.id == UserGuitarSkills.user_id)
            .where(active, UserGuitarSkills.provisional.is_(True))
        )
        or 0
    )
    incomplete = int(
        db.scalar(
            select(func.count())
            .select_from(UserGuitarSkills)
            .join(User, User.id == UserGuitarSkills.user_id)
            .where(
                active,
                UserGuitarSkills.accuracy_events
                < int(config.confidence_expected_events),
            )
        )
        or 0
    )
    last_calculated_at = db.scalar(
        select(func.max(UserGuitarSkills.last_calculated_at))
        .join(User, User.id == UserGuitarSkills.user_id)
        .where(active)
    )
    return {
        "running": recalculation_running(),
        "last_calculated_at": last_calculated_at,
        "calculation_version": int(config.calculation_version),
        "users": users,
        "provisional": provisional,
        "incomplete_history": incomplete,
    }
