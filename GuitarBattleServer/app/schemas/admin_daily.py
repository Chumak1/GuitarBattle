"""Admin schemas for the weekly Daily Challenge schedule."""
from __future__ import annotations

from datetime import datetime
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field


class AdminDailySong(BaseModel):
    """Song shown on one slot. Same shape the public client reads."""

    model_config = ConfigDict(from_attributes=True)

    id: UUID
    slug: str
    title: str
    artist: str | None = None
    genre: str
    bpm: int
    difficulty: int
    duration_seconds: int
    #: A draft can be scheduled, but players only get it once published.
    is_published: bool = False


class AdminDailySlot(BaseModel):
    weekday: int = Field(ge=0, le=6, description="0 = Monday ... 6 = Sunday (UTC)")
    weekday_name: str
    song: AdminDailySong | None = None
    updated_at: datetime | None = None


class AdminDailyResponse(BaseModel):
    items: list[AdminDailySlot]
    total: int


class AdminDailySlotUpdate(BaseModel):
    """``{"song_id": "<uuid>"}`` assigns, ``{"song_id": null}`` clears."""

    song_id: UUID | None = None
