from datetime import datetime
from uuid import UUID
from pydantic import BaseModel, ConfigDict, Field, field_validator

class AdminSongCreate(BaseModel):
    slug: str = Field(min_length=1, max_length=120, pattern=r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
    title: str = Field(min_length=1, max_length=200)
    artist: str | None = Field(default=None, max_length=200)
    genre: str = Field(default="Other", min_length=1, max_length=80)
    bpm: int = Field(ge=1, le=400)
    difficulty: int = Field(default=1, ge=1, le=10)
    duration_seconds: int = Field(default=60, ge=1, le=86400)
    cover_key: str | None = Field(default=None, max_length=512)
    description: str | None = Field(default=None, max_length=10000)

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

class AdminSongUpdate(BaseModel):
    slug: str | None = Field(default=None, min_length=1, max_length=120, pattern=r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
    title: str | None = Field(default=None, min_length=1, max_length=200)
    artist: str | None = Field(default=None, max_length=200)
    genre: str | None = Field(default=None, min_length=1, max_length=80)
    bpm: int | None = Field(default=None, ge=1, le=400)
    difficulty: int | None = Field(default=None, ge=1, le=10)
    duration_seconds: int | None = Field(default=None, ge=1, le=86400)
    cover_key: str | None = Field(default=None, max_length=512)
    description: str | None = Field(default=None, max_length=10000)

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

class AdminSongResponse(BaseModel):
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
    is_published: bool
    description: str | None
    created_at: datetime
    updated_at: datetime

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
    created_at: datetime
