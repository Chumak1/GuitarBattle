import uuid
from datetime import datetime
from sqlalchemy import Boolean, CheckConstraint, DateTime, ForeignKey, Index, Integer, SmallInteger, String, Text, func, true
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column
from app.database import Base

class Song(Base):
    __tablename__ = "songs"
    __table_args__ = (
        CheckConstraint(
            "daily_weekday IS NULL OR (daily_weekday >= 0 AND daily_weekday <= 6)",
            name="ck_songs_daily_weekday_range",
        ),
    )

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    slug: Mapped[str] = mapped_column(String(120), unique=True, index=True, nullable=False)
    title: Mapped[str] = mapped_column(String(200), nullable=False)
    artist: Mapped[str | None] = mapped_column(String(200), nullable=True)
    genre: Mapped[str] = mapped_column(String(80), nullable=False, default="Other")
    bpm: Mapped[int] = mapped_column(Integer, nullable=False)
    difficulty: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    duration_seconds: Mapped[int] = mapped_column(Integer, nullable=False, default=60)
    cover_key: Mapped[str | None] = mapped_column(String(512), nullable=True)
    version: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    is_published: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False)
    description: Mapped[str | None] = mapped_column(Text, nullable=True)
    #: Daily-challenge slot of this song: ``0`` = Monday ... ``6`` = Sunday (UTC).
    #: ``NULL`` means an ordinary catalogue song. A non-null value hides the song
    #: from the public catalogue, the global rating and personal-best lists, but
    #: its assets stay downloadable by slug so the official attempt can be played.
    daily_weekday: Mapped[int | None] = mapped_column(
        SmallInteger, nullable=True, index=True
    )
    #: ``NULL`` while the song is a working catalogue entry. Archiving is the
    #: default outcome of ``DELETE /admin/songs/{id}``: the row keeps its stable
    #: ``id`` so every user result (``song_best_scores``, ``game_sessions``,
    #: ``daily_results`` through ``daily_challenges``) stays attached, but the
    #: song leaves the public catalogue and cannot start new sessions.
    archived_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True, index=True
    )
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), onupdate=func.now(), nullable=False)

class SongFile(Base):
    __tablename__ = "song_files"
    __table_args__ = (
        Index("ix_song_files_song_id_role_is_active", "song_id", "role", "is_active"),
    )

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    song_id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), ForeignKey("songs.id", ondelete="CASCADE"), index=True, nullable=False)
    role: Mapped[str] = mapped_column(String(32), nullable=False)
    object_key: Mapped[str] = mapped_column(String(1024), nullable=False)
    content_type: Mapped[str] = mapped_column(String(120), nullable=False)
    size_bytes: Mapped[int] = mapped_column(Integer, nullable=False)
    sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    #: Published version of this ``(song_id, role)`` pair. Public endpoints serve
    #: only active files; older uploads stay downloadable as an archive and can
    #: be restored with ``POST /admin/songs/{id}/files/{file_id}/activate``.
    is_active: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default=true()
    )
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
