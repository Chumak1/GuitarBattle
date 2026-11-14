from datetime import datetime
from uuid import UUID
from pydantic import BaseModel, ConfigDict, Field, field_validator

from app.schemas.songs import TIME_SIGNATURE_DENOMINATORS, SongReadinessResponse


def _check_time_signature_denominator(value: int | None) -> int | None:
    """Reject denominators MIDI (and therefore the client metronome) rejects."""
    if value is None:
        return None
    if value not in TIME_SIGNATURE_DENOMINATORS:
        raise ValueError("Time signature denominator must be one of 1, 2, 4, 8, 16, 32")
    return value


class AdminSongCreate(BaseModel):
    slug: str = Field(min_length=1, max_length=120, pattern=r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
    title: str = Field(min_length=1, max_length=200)
    artist: str | None = Field(default=None, max_length=200)
    genre: str = Field(default="Other", min_length=1, max_length=80)
    bpm: int = Field(ge=1, le=400)
    #: Meter of the song; ``4/4`` when omitted.
    time_signature_numerator: int = Field(default=4, ge=1, le=32)
    time_signature_denominator: int = Field(default=4)
    difficulty: int = Field(default=1, ge=1, le=10)
    duration_seconds: int = Field(default=60, ge=1, le=86400)
    cover_key: str | None = Field(default=None, max_length=512)
    description: str | None = Field(default=None, max_length=10000)
    #: Daily Challenge slot: 0 = Monday ... 6 = Sunday, ``None`` = ordinary song.
    daily_weekday: int | None = Field(default=None, ge=0, le=6)
    #: Optional gameplay visuals. ``None`` stores no selection.
    character_id: UUID | None = None
    stage_id: UUID | None = None

    @field_validator("slug", "title", "genre")
    @classmethod
    def trim_required_text(cls, value: str) -> str:
        cleaned = value.strip()
        if not cleaned:
            raise ValueError("This field must not be blank")
        return cleaned

    @field_validator("artist", "description", "cover_key")
    @classmethod
    def trim_optional_text(cls, value: str | None) -> str | None:
        if value is None:
            return None
        cleaned = value.strip()
        return cleaned or None

    @field_validator("time_signature_denominator")
    @classmethod
    def validate_time_signature_denominator(cls, value: int | None) -> int | None:
        return _check_time_signature_denominator(value)

class AdminSongUpdate(BaseModel):
    slug: str | None = Field(default=None, min_length=1, max_length=120, pattern=r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
    title: str | None = Field(default=None, min_length=1, max_length=200)
    artist: str | None = Field(default=None, max_length=200)
    genre: str | None = Field(default=None, min_length=1, max_length=80)
    bpm: int | None = Field(default=None, ge=1, le=400)
    #: Omitted keeps the stored meter; ``null`` is rejected by the endpoint
    #: because both columns are NOT NULL.
    time_signature_numerator: int | None = Field(default=None, ge=1, le=32)
    time_signature_denominator: int | None = Field(default=None)
    difficulty: int | None = Field(default=None, ge=1, le=10)
    duration_seconds: int | None = Field(default=None, ge=1, le=86400)
    cover_key: str | None = Field(default=None, max_length=512)
    description: str | None = Field(default=None, max_length=10000)
    #: Explicit ``null`` clears the slot; omitting the field keeps it as is.
    daily_weekday: int | None = Field(default=None, ge=0, le=6)
    #: Omitted keeps the stored selection; explicit ``null`` clears it.
    character_id: UUID | None = None
    stage_id: UUID | None = None

    @field_validator("slug", "title", "genre")
    @classmethod
    def trim_required_text(cls, value: str | None) -> str | None:
        if value is None:
            return None
        cleaned = value.strip()
        if not cleaned:
            raise ValueError("This field must not be blank")
        return cleaned

    @field_validator("artist", "description", "cover_key")
    @classmethod
    def trim_optional_text(cls, value: str | None) -> str | None:
        if value is None:
            return None
        cleaned = value.strip()
        return cleaned or None

    @field_validator("time_signature_denominator")
    @classmethod
    def validate_time_signature_denominator(cls, value: int | None) -> int | None:
        return _check_time_signature_denominator(value)

class AdminSongResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)
    id: UUID
    slug: str
    title: str
    artist: str | None
    genre: str
    bpm: int
    #: Defaults keep ``model_validate`` working for rows (and objects) created
    #: before the meter existed. Only the admin panel reads these fields.
    time_signature_numerator: int = 4
    time_signature_denominator: int = 4
    difficulty: int
    duration_seconds: int
    cover_key: str | None
    version: int
    is_published: bool
    description: str | None
    daily_weekday: int | None = None
    #: ``NULL`` for an active song; a timestamp once the song is archived.
    #: Archived songs stay editable/restorable and keep all their results.
    archived_at: datetime | None = None
    created_at: datetime
    updated_at: datetime
    readiness: SongReadinessResponse = Field(default_factory=SongReadinessResponse)
    #: Admin-only download link of the active Preview Song (full mix).
    #: ``None`` until a preview is uploaded; the link needs the admin token.
    preview_url: str | None = None
    #: Selected gameplay visuals. IDs are what ``PATCH`` accepts; the names and
    #: URLs are provided so the songs table can render the selection without a
    #: second request. Every value is ``None`` when nothing is selected.
    character_id: UUID | None = None
    character_name: str | None = None
    character_sheet_url: str | None = None
    stage_id: UUID | None = None
    stage_name: str | None = None
    stage_background_url: str | None = None

class AdminSongListResponse(BaseModel):
    items: list[AdminSongResponse]
    total: int
    limit: int
    offset: int


class AdminSongFileResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)
    id: UUID
    song_id: UUID
    role: str
    object_key: str
    content_type: str
    size_bytes: int
    sha256: str
    #: ``True`` for the published version of its role, ``False`` for an archived upload.
    is_active: bool = True
    created_at: datetime
    download_url: str | None = None


class AdminSongStemsResponse(BaseModel):
    drums: AdminSongFileResponse | None = None
    bass: AdminSongFileResponse | None = None
    guitar: AdminSongFileResponse | None = None
    others: AdminSongFileResponse | None = None


class AdminSongAssetsResponse(BaseModel):
    midi: AdminSongFileResponse | None = None
    stems: AdminSongStemsResponse = Field(default_factory=AdminSongStemsResponse)
    #: Optional full-mix preview. Shown in the admin file manager with its own
    #: upload/replace/delete actions; not part of readiness or publication.
    preview: AdminSongFileResponse | None = None


class AdminSongDetailResponse(AdminSongResponse):
    assets: AdminSongAssetsResponse = Field(default_factory=AdminSongAssetsResponse)
    #: Every stored version of every role, active and archived, newest first.
    files: list[AdminSongFileResponse] = Field(default_factory=list)
