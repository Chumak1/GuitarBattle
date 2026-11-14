"""Public player-facing character catalogue (``/api/v1/characters``).

The admin panel manages characters under ``/admin/ui-levels``; this router is
the only player-facing view of them. It is public: the desktop client lists the
choices before signing in and downloads the spritesheets from
``/media/characters/``.

``Authorization: Bearer ...`` is optional and additive. Anonymous requests see
every character, but only starter characters are marked ``unlocked``. A signed-in
request also sees everything in its ``user_characters`` collection unlocked, and
each character carries the tower that rewards it (``unlock_campaign_id`` /
``unlock_campaign_name``) plus the human-readable ``unlock_condition``.

The response is a bare JSON array, ordered by ``name``. ``id`` is the stable
integer ``UiCharacter.public_id`` — the internal primary key remains a UUID and
never leaks into this contract.
"""
from __future__ import annotations

from uuid import UUID

from fastapi import APIRouter, Depends
from sqlalchemy import select
from sqlalchemy.orm import Session

from app.api.v1.auth import get_optional_current_user
from app.database import get_db
from app.models.campaign import Campaign
from app.models.gameplay_visuals import UiCharacter
from app.models.user import User
from app.schemas.gameplay_visuals import PublicCharacterResponse
from app.services.characters import owned_character_ids
from app.services.gameplay_media import character_sheet_url

router = APIRouter(prefix="/characters", tags=["characters"])


def _reward_map(db: Session) -> dict[UUID, tuple[UUID, str, str]]:
    """Reward character id -> (campaign id, campaign name, condition text)."""
    rows = db.execute(
        select(
            Campaign.id,
            Campaign.name,
            Campaign.reward_character_id,
            Campaign.reward_condition,
        )
        .where(Campaign.reward_character_id.is_not(None))
        .order_by(Campaign.name.asc(), Campaign.id.asc())
    ).all()

    rewards: dict[UUID, tuple[UUID, str, str]] = {}
    for campaign_id, name, character_id, condition in rows:
        if character_id is None or character_id in rewards:
            continue
        text = (condition or "").strip() or f"Complete {name}"
        rewards[character_id] = (campaign_id, name, text)
    return rewards


@router.get(
    "",
    response_model=list[PublicCharacterResponse],
    summary="List playable characters (public, optional bearer)",
)
def list_characters(
    db: Session = Depends(get_db),
    current_user: User | None = Depends(get_optional_current_user),
) -> list[PublicCharacterResponse]:
    characters = db.scalars(
        select(UiCharacter).order_by(UiCharacter.name.asc(), UiCharacter.id.asc())
    ).all()

    owned = owned_character_ids(db, current_user) if current_user is not None else set()
    rewards = _reward_map(db)

    result: list[PublicCharacterResponse] = []
    for character in characters:
        unlocked = (
            character.id in owned
            if current_user is not None
            else bool(character.is_starter)
        )
        reward = rewards.get(character.id)
        result.append(
            PublicCharacterResponse(
                id=character.public_id,
                name=character.name,
                sheet_url=character_sheet_url(character.sheet_key),
                unlocked=unlocked,
                unlock_campaign_id=reward[0] if reward is not None else None,
                unlock_campaign_name=reward[1] if reward is not None else None,
                unlock_condition=reward[2] if reward is not None else None,
            )
        )
    return result
