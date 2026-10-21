from datetime import datetime
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


class SongDetailResponse(SongSummaryResponse):
    files: list[SongFileResponse] = Field(default_factory=list)


class SongListResponse(BaseModel):
    items: list[SongSummaryResponse]
    total: int
    limit: int
    offset: int
