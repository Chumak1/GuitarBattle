from datetime import datetime
from uuid import UUID

from pydantic import BaseModel, ConfigDict


class MidiNoteResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)
    id: UUID
    track_index: int
    channel: int
    midi_note: int
    velocity: int
    start_tick: int
    end_tick: int
    start_seconds: float
    duration_seconds: float


class MidiTempoChangeResponse(BaseModel):
    tick: int
    bpm: float


class MidiAnalysisResponse(BaseModel):
    song_file_id: UUID
    midi_format: int
    track_count: int
    ticks_per_beat: int
    bpm: float
    duration_seconds: float
    note_count: int
    notes_returned: int
    notes_truncated: bool
    tempo_changes: list[MidiTempoChangeResponse]
    notes: list[MidiNoteResponse]
    parsed_at: datetime
