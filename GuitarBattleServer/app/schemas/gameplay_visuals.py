"""Admin schemas for gameplay visuals (characters and stage backgrounds)."""
from datetime import datetime
from uuid import UUID

from pydantic import BaseModel


class UiCharacterResponse(BaseModel):
    """A UI-level character and the public URL of its 8-pose spritesheet."""

    id: UUID
    name: str
    #: Origin-relative URL, e.g. ``/media/characters/<uuid>.png``.
    sheet_url: str
    content_type: str
    size_bytes: int
    original_name: str | None
    created_at: datetime
    updated_at: datetime


class UiCharacterListResponse(BaseModel):
    items: list[UiCharacterResponse]


class PublicCharacterResponse(BaseModel):
    """One pickable character as the player client sees it.

    ``id`` is :attr:`app.models.gameplay_visuals.UiCharacter.public_id`: a
    stable integer. The admin API keeps exposing the internal UUID under the
    same ``id`` name for its own collection.

    The unlock fields are additive. ``unlocked`` is true for anonymous requests
    only for starter characters; a signed-in request also sees everything in its
    collection. ``unlock_condition`` is the administrator text or the derived
    ``"Complete <campaign name>"``; it is ``None`` when no tower rewards the
    character.
    """

    id: int
    name: str
    #: Origin-relative URL, e.g. ``/media/characters/<uuid>.png``.
    sheet_url: str
    #: Whether the requesting player may select it right now.
    unlocked: bool = True
    #: Campaign that rewards this character (public UUID), if any.
    unlock_campaign_id: UUID | None = None
    unlock_campaign_name: str | None = None
    #: Custom administrator text or ``"Complete <campaign name>"``.
    unlock_condition: str | None = None


class StageLevelResponse(BaseModel):
    """A standalone stage background (scene name plus uploaded image)."""

    id: UUID
    name: str
    #: Origin-relative URL, e.g. ``/media/stages/<uuid>.png``.
    background_url: str
    content_type: str
    size_bytes: int
    original_name: str | None
    created_at: datetime
    updated_at: datetime


class StageLevelListResponse(BaseModel):
    items: list[StageLevelResponse]
