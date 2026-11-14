"""Guitar Skills rating: configuration, events, snapshots and history.

Guitar Skills replaces the old "sum of best scores per song" global rating as
the main competitive metric. The rating is a weighted blend of five components
(accuracy, challenge, score efficiency, duel performance and progression) and
is recalculated from a bounded, idempotent event log.

Tables are additive (migration ``0035_guitar_skills``); no existing table is
rewritten. ``duel_participants.guitar_skills_before`` is the only new column on
an existing table: it freezes both players' pre-match rating so a later
recalculation stays historically correct.
"""
from __future__ import annotations

import uuid
from datetime import datetime
from decimal import Decimal
from typing import Any

from sqlalchemy import (
    BigInteger,
    Boolean,
    CheckConstraint,
    DateTime,
    Float,
    ForeignKey,
    Index,
    Integer,
    Numeric,
    String,
    func,
)
from sqlalchemy.dialects.postgresql import JSONB, UUID
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base

#: The single configuration row always uses this primary key.
CONFIG_ID = 1

#: Default XP needed to reach level 50 (``progression._level_start(50)``).
DEFAULT_XP_REFERENCE = 344_756


class GuitarSkillsConfig(Base):
    """The single active tuning row of the rating.

    ``calculation_version`` is bumped on every saved change; a stored
    ``user_guitar_skills`` row records the version it was computed with so a
    mismatch can be detected (and a full recalculation is scheduled).
    """

    __tablename__ = "guitar_skills_config"
    __table_args__ = (
        CheckConstraint(
            "accuracy_weight >= 0 AND accuracy_weight <= 1",
            name="ck_guitar_skills_config_accuracy_weight",
        ),
        CheckConstraint(
            "challenge_weight >= 0 AND challenge_weight <= 1",
            name="ck_guitar_skills_config_challenge_weight",
        ),
        CheckConstraint(
            "score_weight >= 0 AND score_weight <= 1",
            name="ck_guitar_skills_config_score_weight",
        ),
        CheckConstraint(
            "duel_weight >= 0 AND duel_weight <= 1",
            name="ck_guitar_skills_config_duel_weight",
        ),
        CheckConstraint(
            "progression_weight >= 0 AND progression_weight <= 1",
            name="ck_guitar_skills_config_progression_weight",
        ),
        CheckConstraint(
            "window_size >= 1 AND window_size <= 200",
            name="ck_guitar_skills_config_window_size",
        ),
        CheckConstraint(
            "recency_decay > 0", name="ck_guitar_skills_config_recency_decay"
        ),
        CheckConstraint(
            "difficulty_easy_max >= 1 AND difficulty_easy_max <= 10",
            name="ck_guitar_skills_config_easy_max",
        ),
        CheckConstraint(
            "difficulty_normal_max >= difficulty_easy_max "
            "AND difficulty_normal_max <= 10",
            name="ck_guitar_skills_config_normal_max",
        ),
        CheckConstraint(
            "note_factor_min >= 0 AND note_factor_max >= note_factor_min",
            name="ck_guitar_skills_config_note_factor",
        ),
        CheckConstraint(
            "level_normalization_max >= 1",
            name="ck_guitar_skills_config_level_normalization",
        ),
        CheckConstraint(
            "xp_reference >= 1", name="ck_guitar_skills_config_xp_reference"
        ),
        CheckConstraint(
            "provisional_threshold >= 0 AND provisional_threshold <= 1",
            name="ck_guitar_skills_config_provisional_threshold",
        ),
        CheckConstraint(
            "min_ranked_events >= 0",
            name="ck_guitar_skills_config_min_ranked_events",
        ),
    )

    id: Mapped[int] = mapped_column(
        Integer, primary_key=True, default=CONFIG_ID, autoincrement=False
    )

    #: The five weights, validated to sum to ``1.0``.
    accuracy_weight: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.35, server_default="0.35"
    )
    challenge_weight: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.20, server_default="0.20"
    )
    score_weight: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.10, server_default="0.10"
    )
    duel_weight: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.25, server_default="0.25"
    )
    progression_weight: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.10, server_default="0.10"
    )

    #: How many newest valid events feed accuracy/challenge/score/duel.
    window_size: Mapped[int] = mapped_column(
        Integer, nullable=False, default=30, server_default="30"
    )
    #: ``RecencyWeight(i) = 2 ** (-i / recency_decay)``.
    recency_decay: Mapped[float] = mapped_column(
        Float, nullable=False, default=10.0, server_default="10"
    )

    #: Difficulty tiers: 1..easy_max Easy, ..normal_max Normal, ..10 Hard.
    difficulty_easy_max: Mapped[int] = mapped_column(
        Integer, nullable=False, default=3, server_default="3"
    )
    difficulty_normal_max: Mapped[int] = mapped_column(
        Integer, nullable=False, default=7, server_default="7"
    )
    difficulty_easy_factor: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.60, server_default="0.60"
    )
    difficulty_normal_factor: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.80, server_default="0.80"
    )
    difficulty_hard_factor: Mapped[float] = mapped_column(
        Float, nullable=False, default=1.00, server_default="1.00"
    )

    #: ``NoteFactor = clamp(sqrt(note_count / median), min, max)``.
    note_factor_min: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.75, server_default="0.75"
    )
    note_factor_max: Mapped[float] = mapped_column(
        Float, nullable=False, default=1.25, server_default="1.25"
    )
    #: Median note count of published non-daily competitive songs. Computed
    #: once by the migration / first read; an administrator may override it.
    median_note_count: Mapped[int | None] = mapped_column(Integer, nullable=True)

    #: ``NormalizedLevel = 100 * clamp(level / level_normalization_max, 0, 1)``.
    level_normalization_max: Mapped[int] = mapped_column(
        Integer, nullable=False, default=50, server_default="50"
    )
    #: Fixed XP reference of the progression component (level 50 by default).
    xp_reference: Mapped[int] = mapped_column(
        Integer,
        nullable=False,
        default=DEFAULT_XP_REFERENCE,
        server_default=str(DEFAULT_XP_REFERENCE),
    )

    #: Duel strength mix: guitar skills and normalized level.
    duel_strength_guitar_weight: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.70, server_default="0.70"
    )
    duel_strength_level_weight: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.30, server_default="0.30"
    )
    #: Elo-style scale of the expected score.
    expected_score_scale: Mapped[float] = mapped_column(
        Float, nullable=False, default=25.0, server_default="25"
    )
    #: ``Reliability = matches / (matches + reliability_constant)``.
    reliability_constant: Mapped[float] = mapped_column(
        Float, nullable=False, default=10.0, server_default="10"
    )
    #: Anti-farm cap: one song may contribute at most N average attempts.
    per_song_attempt_cap: Mapped[float] = mapped_column(
        Float, nullable=False, default=2.0, server_default="2"
    )

    #: Confidence expectations and the provisional threshold.
    confidence_expected_events: Mapped[int] = mapped_column(
        Integer, nullable=False, default=10, server_default="10"
    )
    confidence_expected_duels: Mapped[int] = mapped_column(
        Integer, nullable=False, default=10, server_default="10"
    )
    provisional_threshold: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.35, server_default="0.35"
    )
    min_ranked_events: Mapped[int] = mapped_column(
        Integer, nullable=False, default=5, server_default="5"
    )

    calculation_version: Mapped[int] = mapped_column(
        Integer, nullable=False, default=1, server_default="1"
    )
    updated_by: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="SET NULL"),
        nullable=True,
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )


class GuitarSkillsConfigHistory(Base):
    """Append-only snapshot of every saved configuration change."""

    __tablename__ = "guitar_skills_config_history"
    __table_args__ = (
        Index(
            "ix_guitar_skills_config_history_created_at", "created_at"
        ),
    )

    id: Mapped[int] = mapped_column(
        BigInteger, primary_key=True, autoincrement=True
    )
    #: Full new configuration (same keys as ``GuitarSkillsConfig``).
    config: Mapped[dict[str, Any]] = mapped_column(JSONB, nullable=False)
    admin_user_id: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="SET NULL"),
        nullable=True,
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )


class UserGuitarSkills(Base):
    """Current Guitar Skills snapshot of one player (1:1 with ``users``)."""

    __tablename__ = "user_guitar_skills"
    __table_args__ = (
        CheckConstraint(
            "guitar_skills >= 0 AND guitar_skills <= 100",
            name="ck_user_guitar_skills_total",
        ),
        CheckConstraint(
            "accuracy_score >= 0 AND accuracy_score <= 100",
            name="ck_user_guitar_skills_accuracy",
        ),
        CheckConstraint(
            "challenge_score >= 0 AND challenge_score <= 100",
            name="ck_user_guitar_skills_challenge",
        ),
        CheckConstraint(
            "score_performance >= 0 AND score_performance <= 100",
            name="ck_user_guitar_skills_score",
        ),
        CheckConstraint(
            "duel_performance >= 0 AND duel_performance <= 100",
            name="ck_user_guitar_skills_duel",
        ),
        CheckConstraint(
            "progression_score >= 0 AND progression_score <= 100",
            name="ck_user_guitar_skills_progression",
        ),
        CheckConstraint(
            "confidence >= 0 AND confidence <= 1",
            name="ck_user_guitar_skills_confidence",
        ),
        CheckConstraint("rank IS NULL OR rank >= 1", name="ck_user_guitar_skills_rank"),
        # Global leaderboard: total, then duel performance, then XP.
        Index(
            "ix_user_guitar_skills_leaderboard",
            "guitar_skills",
            "duel_performance",
        ),
        Index("ix_user_guitar_skills_rank", "rank"),
        Index("ix_user_guitar_skills_provisional", "provisional"),
    )

    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        primary_key=True,
    )
    guitar_skills: Mapped[Decimal] = mapped_column(
        Numeric(5, 1), nullable=False, default=50.0, server_default="50"
    )
    accuracy_score: Mapped[Decimal] = mapped_column(
        Numeric(5, 1), nullable=False, default=50.0, server_default="50"
    )
    challenge_score: Mapped[Decimal] = mapped_column(
        Numeric(5, 1), nullable=False, default=50.0, server_default="50"
    )
    score_performance: Mapped[Decimal] = mapped_column(
        Numeric(5, 1), nullable=False, default=50.0, server_default="50"
    )
    duel_performance: Mapped[Decimal] = mapped_column(
        Numeric(5, 1), nullable=False, default=50.0, server_default="50"
    )
    progression_score: Mapped[Decimal] = mapped_column(
        Numeric(5, 1), nullable=False, default=50.0, server_default="50"
    )

    confidence: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0, server_default="0"
    )
    provisional: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    events_counted: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    duel_matches: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    #: Plain (unweighted) mean accuracy of the counted window, in percent.
    accuracy_percent: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0, server_default="0"
    )
    duel_wins: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    duel_losses: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    duel_draws: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    duel_win_rate: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0, server_default="0"
    )

    rank: Mapped[int | None] = mapped_column(Integer, nullable=True)
    previous_rank: Mapped[int | None] = mapped_column(Integer, nullable=True)

    #: Per-component sample counts and confidences (used by the detail API).
    accuracy_events: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    challenge_events: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    score_events: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    duel_events: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    progression_events: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    accuracy_confidence: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0, server_default="0"
    )
    challenge_confidence: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0, server_default="0"
    )
    score_confidence: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0, server_default="0"
    )
    duel_confidence: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0, server_default="0"
    )
    progression_confidence: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0, server_default="0"
    )

    calculation_version: Mapped[int] = mapped_column(
        Integer, nullable=False, default=1, server_default="1"
    )
    last_calculated_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )


class GuitarSkillsEvent(Base):
    """One ranked result that feeds the rating.

    ``event_key`` is the idempotency guard: ``solo:<session_id>`` or
    ``duel:<match_id>:<user_id>``. Reprocessing the same session or match is a
    no-op (``ON CONFLICT DO NOTHING``).
    """

    __tablename__ = "guitar_skills_events"
    __table_args__ = (
        Index(
            "ix_guitar_skills_events_user_occurred",
            "user_id",
            "occurred_at",
        ),
        Index("ix_guitar_skills_events_song_user", "song_id", "user_id"),
        Index("ix_guitar_skills_events_match", "match_id"),
        CheckConstraint(
            "accuracy >= 0 AND accuracy <= 100",
            name="ck_guitar_skills_events_accuracy",
        ),
        CheckConstraint("score >= 0", name="ck_guitar_skills_events_score"),
        CheckConstraint(
            "note_count IS NULL OR note_count > 0",
            name="ck_guitar_skills_events_note_count",
        ),
        CheckConstraint(
            "difficulty IS NULL OR (difficulty >= 1 AND difficulty <= 10)",
            name="ck_guitar_skills_events_difficulty",
        ),
        CheckConstraint(
            "result IS NULL OR result IN ('win', 'loss', 'draw')",
            name="ck_guitar_skills_events_result",
        ),
        CheckConstraint(
            "score_margin IS NULL OR (score_margin >= 0 AND score_margin <= 1)",
            name="ck_guitar_skills_events_margin",
        ),
    )

    id: Mapped[int] = mapped_column(
        BigInteger, primary_key=True, autoincrement=True
    )
    event_key: Mapped[str] = mapped_column(
        String(80), unique=True, nullable=False
    )
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    match_id: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("duel_matches.id", ondelete="SET NULL"),
        nullable=True,
    )
    song_id: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("songs.id", ondelete="SET NULL"),
        nullable=True,
    )
    #: ``normal`` / ``daily`` for solo, ``online_duel`` for duels.
    mode: Mapped[str] = mapped_column(String(16), nullable=False)
    accuracy: Mapped[float] = mapped_column(Float, nullable=False)
    score: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    note_count: Mapped[int | None] = mapped_column(Integer, nullable=True)
    difficulty: Mapped[int | None] = mapped_column(Integer, nullable=True)

    opponent_id: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="SET NULL"),
        nullable=True,
    )
    opponent_level: Mapped[int | None] = mapped_column(Integer, nullable=True)
    opponent_pre_match_rating: Mapped[int | None] = mapped_column(
        Integer, nullable=True
    )
    #: ``NULL`` for historical matches: the engine then uses the rating proxy.
    opponent_pre_match_guitar_skills: Mapped[float | None] = mapped_column(
        Float, nullable=True
    )
    #: Own frozen pre-match values (extra columns, needed by the D formula).
    player_pre_match_rating: Mapped[int | None] = mapped_column(
        Integer, nullable=True
    )
    player_pre_match_guitar_skills: Mapped[float | None] = mapped_column(
        Float, nullable=True
    )

    result: Mapped[str | None] = mapped_column(String(8), nullable=True)
    #: ``my_score / (my_score + opponent_score)``; ``NULL`` for solo.
    score_margin: Mapped[float | None] = mapped_column(Float, nullable=True)
    occurred_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )


class GuitarSkillsHistory(Base):
    """One row per real change of a player's total (no no-op history)."""

    __tablename__ = "guitar_skills_history"
    __table_args__ = (
        Index(
            "ix_guitar_skills_history_user_created",
            "user_id",
            "created_at",
        ),
    )

    id: Mapped[int] = mapped_column(
        BigInteger, primary_key=True, autoincrement=True
    )
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    old_guitar_skills: Mapped[Decimal | None] = mapped_column(
        Numeric(5, 1), nullable=True
    )
    new_guitar_skills: Mapped[Decimal] = mapped_column(
        Numeric(5, 1), nullable=False
    )
    old_rank: Mapped[int | None] = mapped_column(Integer, nullable=True)
    new_rank: Mapped[int | None] = mapped_column(Integer, nullable=True)
    component_values: Mapped[dict[str, Any]] = mapped_column(
        JSONB, nullable=False
    )
    calculation_version: Mapped[int] = mapped_column(
        Integer, nullable=False
    )
    reason: Mapped[str] = mapped_column(String(64), nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
