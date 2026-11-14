import uuid
from datetime import datetime
from sqlalchemy import Boolean, CheckConstraint, DateTime, ForeignKey, Index, Integer, SmallInteger, String, Text, func, true
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column, relationship
from app.database import Base
from app.models.gameplay_visuals import StageLevel, UiCharacter

class Song(Base):
    __tablename__ = "songs"
    __table_args__ = (
        CheckConstraint(
            "daily_weekday IS NULL OR (daily_weekday >= 0 AND daily_weekday <= 6)",
            name="ck_songs_daily_weekday_range",
        ),
        CheckConstraint(
            "time_signature_numerator >= 1 AND time_signature_numerator <= 32",
            name="ck_songs_time_signature_numerator",
        ),
        CheckConstraint(
            "time_signature_denominator IN (1, 2, 4, 8, 16, 32)",
            name="ck_songs_time_signature_denominator",
        ),
    )

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    slug: Mapped[str] = mapped_column(String(120), unique=True, index=True, nullable=False)
    title: Mapped[str] = mapped_column(String(200), nullable=False)
    artist: Mapped[str | None] = mapped_column(String(200), nullable=True)
    genre: Mapped[str] = mapped_column(String(80), nullable=False, default="Other")
    bpm: Mapped[int] = mapped_column(Integer, nullable=False)
    #: Meter of the song, used by the client for the metronome and bar lines.
    #: ``4/4`` is the default so existing songs keep their previous behaviour.
    #: The denominator is a power of two because MIDI time signatures only
    #: allow 1, 2, 4, 8, 16, 32.
    time_signature_numerator: Mapped[int] = mapped_column(
        SmallInteger, nullable=False, default=4, server_default="4"
    )
    time_signature_denominator: Mapped[int] = mapped_column(
        SmallInteger, nullable=False, default=4, server_default="4"
    )
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
    #: Optional gameplay visuals. Both foreign keys use ``ON DELETE SET NULL``:
    #: deleting a character or a stage never removes a song, it only clears the
    #: selection. ``character_id`` is the source of ``character_name`` and
    #: ``character_sheet_url`` in the public catalogue; ``stage_id`` is the
    #: source of ``stage_name`` and ``stage_background_url``.
    character_id: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("ui_characters.id", ondelete="SET NULL"),
        index=True,
        nullable=True,
    )
    stage_id: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("stage_levels.id", ondelete="SET NULL"),
        index=True,
        nullable=True,
    )
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), onupdate=func.now(), nullable=False)

    character: Mapped[UiCharacter | None] = relationship(
        "UiCharacter", lazy="selectin", passive_deletes=True
    )
    stage: Mapped[StageLevel | None] = relationship(
        "StageLevel", lazy="selectin", passive_deletes=True
    )

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
