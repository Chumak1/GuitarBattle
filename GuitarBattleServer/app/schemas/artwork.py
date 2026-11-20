"""Schemas of the administrator artwork slots and player avatars.

Every response is additive: an older client that does not know these fields
simply ignores them.
"""
from __future__ import annotations

from datetime import datetime

from pydantic import BaseModel, ConfigDict


class ArtworkSlotItem(BaseModel):
    """One slot of the admin artwork panel; ``url`` is ``null`` while empty."""

    slot: str
    url: str | None = None
    updated_at: datetime | None = None


class ArtworkSlotListResponse(BaseModel):
    items: list[ArtworkSlotItem]


class SiteConfigResponse(BaseModel):
    """Public artwork configuration used by the desktop client.

    ``*_url`` values are origin-relative (``/media/artwork/<name>``) or ``null``
    when the administrator has not uploaded that image yet, so the client can
    keep its procedural fallback.
    """

    model_config = ConfigDict(extra="forbid")

    hero_battle_url: str | None = None
    hero_duel_url: str | None = None
    default_avatar_url: str | None = None


class AvatarResponse(BaseModel):
    """Result of ``POST``/``DELETE /profile/avatar``."""

    avatar_image_url: str | None = None
