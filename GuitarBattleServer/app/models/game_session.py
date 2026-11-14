import uuid
from datetime import datetime
from sqlalchemy import DateTime, Float, ForeignKey, Index, Integer, String, func
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column
from app.database import Base

class GameSession(Base):
    __tablename__ = "game_sessions"
    __table_args__ = (
        # Supports the "attempts of this song today" query that drives the
        # repeat XP factor, and the per-mode history of a player.
        Index("ix_game_sessions_user_song_mode", "user_id", "song_id", "mode"),
    )
    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    user_id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), ForeignKey("users.id", ondelete="CASCADE"), index=True, nullable=False)
    song_id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), ForeignKey("songs.id", ondelete="RESTRICT"), index=True, nullable=False)
    song_version: Mapped[int] = mapped_column(Integer, nullable=False)
    speed_percent: Mapped[int] = mapped_column(Integer, nullable=False, default=100)
    # "normal" for free play and practice on the daily song, "daily" for the one
    # official daily challenge attempt, "preview" for an admin draft preview and
    # "practice" for a Practice-section run (stored for audit, never ranked).
    mode: Mapped[str] = mapped_column(String(16), nullable=False, default="normal")
    started_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    finished_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    status: Mapped[str] = mapped_column(String(24), nullable=False, default="started")

class Score(Base):
    __tablename__ = "scores"
    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    session_id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), ForeignKey("game_sessions.id", ondelete="CASCADE"), unique=True, nullable=False)
    score_points: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    #: ``score_points`` extrapolated to a full song by the played fraction.
    #: ``NULL`` when the attempt ended too early to extrapolate safely, or when
    #: the row predates migration 0030. The actual score is never overwritten.
    normalized_score: Mapped[int | None] = mapped_column(Integer, nullable=True)
    accuracy_percent: Mapped[float] = mapped_column(Float, nullable=False, default=0.0)
    max_combo: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    perfect_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    good_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    miss_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    # Progression inputs. ``completion_percent`` is clamped by the note counters
    # on the server, so a client cannot claim more progress than it played.
    completion_percent: Mapped[float] = mapped_column(Float, nullable=False, default=100.0)
    avg_timing_error_ms: Mapped[float | None] = mapped_column(Float, nullable=True)
    notes_total: Mapped[int | None] = mapped_column(Integer, nullable=True)
    notes_played: Mapped[int | None] = mapped_column(Integer, nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
