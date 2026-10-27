from datetime import datetime
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field


class SongFileResponse(BaseModel):
    """Public file metadata. Internal object-storage keys are never exposed."""
    model_config = ConfigDict(from_attributes=True)

    id: UUID
    role: str
    content_type: str
    size_bytes: int
    sha256: str
    download_url: str | None = None


class SongStemsResponse(BaseModel):
    """The four optional instrumental stems of a song."""

    drums: SongFileResponse | None = None
    bass: SongFileResponse | None = None
    guitar: SongFileResponse | None = None
    others: SongFileResponse | None = None


class SongAssetsResponse(BaseModel):
    """One stable shape for the MIDI file and all four MP3 stems."""

    midi: SongFileResponse | None = None
    stems: SongStemsResponse = Field(default_factory=SongStemsResponse)


class SongReadinessResponse(BaseModel):
    """Asset completeness for players and admins.

    ``is_playable`` only requires MIDI, so a MIDI-only song remains usable.
    ``is_complete`` and ``publish_ready`` additionally require all four stems.
    """

    state: Literal["empty", "midi_only", "partial", "complete"] = "empty"
    available_roles: list[str] = Field(default_factory=list)
    missing_roles: list[str] = Field(default_factory=list)
    has_midi: bool = False
    has_all_stems: bool = False
    is_playable: bool = False
    is_complete: bool = False
    publish_ready: bool = False


class SongSummaryResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: UUID
    slug: str
    title: str
    artist: str | None
    genre: str
    bpm: int
    difficulty: int
    duration_seconds: int
    cover_key: str | None
    version: int
    description: str | None
    created_at: datetime
    #: Absolute URL of the optional full-mix preview (``role = "preview"``),
    #: or ``None`` when the song has no preview. Additive field: the catalogue
    #: and gameplay keep working without it.
    preview_url: str | None = None
    readiness: SongReadinessResponse = Field(default_factory=SongReadinessResponse)


class SongDetailResponse(SongSummaryResponse):
    files: list[SongFileResponse] = Field(default_factory=list)
    assets: SongAssetsResponse = Field(default_factory=SongAssetsResponse)


class SongListResponse(BaseModel):
    items: list[SongSummaryResponse]
    total: int
    limit: int
    offset: int
