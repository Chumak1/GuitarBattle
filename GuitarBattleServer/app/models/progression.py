"""Progression tables: XP, personal bests, achievements and daily challenges."""
import uuid
from datetime import date, datetime
from typing import Any

from sqlalchemy import (
    Boolean,
    CheckConstraint,
    Date,
    DateTime,
    Float,
    ForeignKey,
    Integer,
    SmallInteger,
    String,
    UniqueConstraint,
    func,
)
from sqlalchemy.dialects.postgresql import JSONB, UUID
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class SongBestScore(Base):
    """One row per (player, song): the attempt that counts for the rating.

    The global leaderboard keeps its own aggregation over ``scores``; this table
    is the progression view of the same player best, extended with the mastery
    inputs and turned into an O(1) lookup instead of a per-request aggregate.
    """

    __tablename__ = "song_best_scores"
    __table_args__ = (
        UniqueConstraint("user_id", "song_id", name="uq_song_best_scores_user_song"),
    )

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), ForeignKey("users.id", ondelete="CASCADE"), index=True, nullable=False
    )
    song_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        # A personal best is user data: deleting the song must never delete it.
        # Hard deletion of a song is only possible while no result references it.
        ForeignKey("songs.id", ondelete="RESTRICT"),
        index=True,
        nullable=False,
    )
    best_score: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    best_accuracy: Mapped[float] = mapped_column(Float, nullable=False, default=0.0)
    best_max_combo: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    best_avg_timing_error_ms: Mapped[float | None] = mapped_column(Float, nullable=True)
    attempts: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), onupdate=func.now(), nullable=False
    )


class XpAward(Base):
    """Idempotency ledger for attempt XP: one row per finished session."""

    __tablename__ = "xp_awards"

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), ForeignKey("users.id", ondelete="CASCADE"), index=True, nullable=False
    )
    session_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("game_sessions.id", ondelete="CASCADE"),
        unique=True,
        nullable=False,
    )
    amount: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    breakdown: Mapped[dict[str, Any] | None] = mapped_column(JSONB, nullable=True)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )


class Achievement(Base):
    """Static catalog, seeded by migration 0008 from the code catalog."""

    __tablename__ = "achievements"

    code: Mapped[str] = mapped_column(String(64), primary_key=True)
    title: Mapped[str] = mapped_column(String(200), nullable=False)
    description: Mapped[str] = mapped_column(String(500), nullable=False)
    category: Mapped[str] = mapped_column(String(32), nullable=False)
    xp_bonus: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    goal: Mapped[int | None] = mapped_column(Integer, nullable=True)
    #: File name under ``app/static/media/achievements`` (served at
    #: ``/media/achievements/<icon>``). Kept as a bare name so the API can build
    #: the public URL and the web/desktop clients share one catalog.
    icon: Mapped[str] = mapped_column(String(128), nullable=False, default="star.svg")
    is_active: Mapped[bool] = mapped_column(Boolean, nullable=False, default=True)
    sort_order: Mapped[int] = mapped_column(Integer, nullable=False, default=0)


class UserAchievement(Base):
    __tablename__ = "user_achievements"
    __table_args__ = (
        UniqueConstraint(
            "user_id", "achievement_code", name="uq_user_achievements_user_code"
        ),
    )

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), ForeignKey("users.id", ondelete="CASCADE"), index=True, nullable=False
    )
    achievement_code: Mapped[str] = mapped_column(
        String(64),
        ForeignKey("achievements.code", ondelete="CASCADE"),
        nullable=False,
    )
    awarded_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    progress: Mapped[int] = mapped_column(Integer, nullable=False, default=0)


class DailyChallenge(Base):
    """The song chosen for one UTC day. One row per day, immutable afterwards."""

    __tablename__ = "daily_challenges"

    challenge_date: Mapped[date] = mapped_column(Date, primary_key=True)
    song_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), ForeignKey("songs.id", ondelete="RESTRICT"), nullable=False
    )
    rules_version: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )


class DailySlot(Base):
    """Weekly schedule of the daily challenge: one song per weekday (UTC).

    ``weekday`` uses the Python convention: ``0`` = Monday ... ``6`` = Sunday.
    The schedule is editable by administrators; a concrete day is frozen into
    :class:`DailyChallenge` on the first request for that date, so changing the
    slot never rewrites results that players have already submitted.

    ``songs.daily_weekday`` mirrors this table and is kept in sync by
    ``app.services.daily_slots``; it is the value the catalogue filters read.
    """

    __tablename__ = "daily_slots"
    __table_args__ = (
        CheckConstraint(
            "weekday >= 0 AND weekday <= 6", name="ck_daily_slots_weekday_range"
        ),
    )

    weekday: Mapped[int] = mapped_column(SmallInteger, primary_key=True)
    song_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        # Scheduling data, but RESTRICT keeps a stray hard delete from silently
        # rewriting the schedule; archiving a song clears its slot explicitly.
        ForeignKey("songs.id", ondelete="RESTRICT"),
        nullable=False,
        index=True,
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )


class DailyResult(Base):
    """The single official attempt of a player for one daily challenge."""

    __tablename__ = "daily_results"
    __table_args__ = (
        UniqueConstraint(
            "challenge_date", "user_id", name="uq_daily_results_date_user"
        ),
    )

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    challenge_date: Mapped[date] = mapped_column(
        Date, ForeignKey("daily_challenges.challenge_date", ondelete="CASCADE"), index=True, nullable=False
    )
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), ForeignKey("users.id", ondelete="CASCADE"), nullable=False
    )
    session_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("game_sessions.id", ondelete="CASCADE"),
        unique=True,
        nullable=False,
    )
    score_points: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    accuracy_percent: Mapped[float] = mapped_column(Float, nullable=False, default=0.0)
    avg_timing_error_ms: Mapped[float | None] = mapped_column(Float, nullable=True)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
