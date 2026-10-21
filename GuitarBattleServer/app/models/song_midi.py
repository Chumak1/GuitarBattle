import uuid
from datetime import datetime

from sqlalchemy import DateTime, Float, ForeignKey, Integer, func
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class SongMidiAnalysis(Base):
    __tablename__ = "song_midi_analyses"

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    song_file_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), ForeignKey("song_files.id", ondelete="CASCADE"),
        unique=True, nullable=False,
    )
    midi_format: Mapped[int] = mapped_column(Integer, nullable=False)
    track_count: Mapped[int] = mapped_column(Integer, nullable=False)
    ticks_per_beat: Mapped[int] = mapped_column(Integer, nullable=False)
    bpm: Mapped[float] = mapped_column(Float, nullable=False)
    duration_seconds: Mapped[float] = mapped_column(Float, nullable=False)
    note_count: Mapped[int] = mapped_column(Integer, nullable=False)
    parsed_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), onupdate=func.now(), nullable=False
    )


class SongMidiNote(Base):
    __tablename__ = "song_midi_notes"
    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    song_file_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), ForeignKey("song_files.id", ondelete="CASCADE"), index=True, nullable=False
    )
    track_index: Mapped[int] = mapped_column(Integer, nullable=False)
    channel: Mapped[int] = mapped_column(Integer, nullable=False)
    midi_note: Mapped[int] = mapped_column(Integer, nullable=False)
    velocity: Mapped[int] = mapped_column(Integer, nullable=False)
    start_tick: Mapped[int] = mapped_column(Integer, nullable=False)
    end_tick: Mapped[int] = mapped_column(Integer, nullable=False)
    start_seconds: Mapped[float] = mapped_column(Float, nullable=False)
    duration_seconds: Mapped[float] = mapped_column(Float, nullable=False)


class SongMidiTempoChange(Base):
    __tablename__ = "song_midi_tempo_changes"

    song_file_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), ForeignKey("song_files.id", ondelete="CASCADE"),
        primary_key=True,
    )
    tick: Mapped[int] = mapped_column(Integer, primary_key=True)
    bpm: Mapped[float] = mapped_column(Float, nullable=False)
