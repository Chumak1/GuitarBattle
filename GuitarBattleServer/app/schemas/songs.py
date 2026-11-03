from datetime import datetime
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field


#: Denominators accepted by MIDI time signatures (powers of two). The client
#: uses the pair for the metronome and bar lines, so anything else would make
#: it render a meter it cannot click.
TIME_SIGNATURE_DENOMINATORS: tuple[int, ...] = (1, 2, 4, 8, 16, 32)


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
    #: Meter of the song. Additive with a ``4`` default so cached/older
    #: payloads without the fields keep validating; ``4/4`` preserves the
    #: behaviour every song had before the meter was stored.
    time_signature_numerator: int = 4
    time_signature_denominator: int = 4
    difficulty: int
    duration_seconds: int
    cover_key: str | None
    version: int
    description: str | None
    created_at: datetime
    #: Additive: drafts are only returned to an admin that explicitly asked
    #: for them (``include_drafts=1``). Older clients ignore the field and keep
    #: seeing the published catalogue only.
    is_published: bool = True
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
