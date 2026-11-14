"""Schemas for the UI sound events, manifest and per-user preferences."""
from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, Field

UiSoundCategory = Literal["navigation", "action", "system"]


class UiSoundAdminItem(BaseModel):
    """One registry event as shown in the admin panel."""

    key: str
    name: str
    category: UiSoundCategory
    active: bool
    gain: float
    version: int
    has_file: bool
    #: Authorized admin preview/download URL, ``None`` while no file is stored.
    preview_url: str | None = None
    original_name: str | None = None
    content_type: str | None = None
    size_bytes: int | None = None
    updated_at: datetime


class UiSoundAdminList(BaseModel):
    items: list[UiSoundAdminItem]


class UiSoundAdminUpdate(BaseModel):
    """PATCH body of an event. Both fields are optional; gain is clamped."""

    active: bool | None = None
    gain: float | None = None


class UiSoundManifestItem(BaseModel):
    """One playable event in the public manifest.

    The shape is the client contract: only active events that have a file are
    listed, so ``active`` is not repeated per item.
    """

    key: str
    name: str
    category: UiSoundCategory
    gain: float
    url: str
    version: int


class UiSoundManifestResponse(BaseModel):
    #: Opaque number that changes whenever any event's file or settings change.
    version: int
    items: list[UiSoundManifestItem]


class UiSoundEventInfo(BaseModel):
    """A registry entry for the client's settings screen."""

    key: str
    name: str
    category: UiSoundCategory
    active: bool
    gain: float
    version: int
    has_file: bool


class UiSoundUserSettings(BaseModel):
    enabled: bool = True
    volume: float = 0.8
    navigation: bool = True
    action: bool = True
    system: bool = True
    updated_at: datetime | None = None


class UiSoundUserSettingsUpdate(BaseModel):
    """PATCH body of ``/profile/ui-sounds``; every field is optional."""

    enabled: bool | None = None
    volume: float | None = None
    navigation: bool | None = None
    action: bool | None = None
    system: bool | None = None


class UiSoundEventsResponse(BaseModel):
    items: list[UiSoundEventInfo]
    settings: UiSoundUserSettings = Field(default_factory=UiSoundUserSettings)
