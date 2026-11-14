"""Administrator-only account card, character grants and account deletion.

The card aggregates three groups (see :mod:`app.schemas.admin_users`) with
constant-cost SQL: one query per group, never one query per tower or per match.
Character grants and revocations reuse :mod:`app.services.characters`, so the
panel follows exactly the same ownership rule as tower rewards: starters are
always owned, everything else is a ``user_characters`` row.

Deleting an account is a hard delete. Every foreign key that points at
``users.id`` is ``ON DELETE CASCADE`` (game sessions and their scores, app
sessions, game launches, email codes, competitions), so one statement removes
the player and all of their data. The audit trail records ids only: no login,
email or other personal data is written to the log.
"""
import logging
from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Response, status
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.database import get_db
from app.models.character_unlocks import UserCharacter
from app.models.gameplay_visuals import UiCharacter
from app.models.user import User
from app.schemas.admin_users import (
    AdminBattleStats,
    AdminCharacterItem,
    AdminOnlineStats,
    AdminSelectedCharacter,
    AdminUserBasics,
    AdminUserCharacterChange,
    AdminUserCharacterRemoval,
    AdminUserCharacters,
    AdminUserOverview,
)
from app.services.admin_user_stats import battle_stats, online_stats
from app.services.characters import grant_character
from app.services.gameplay_media import character_sheet_url

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/admin/users", tags=["admin: users"])


def _character_item(
    character: UiCharacter, ownership: UserCharacter | None
) -> AdminCharacterItem:
    return AdminCharacterItem(
        id=character.id,
        public_id=character.public_id,
        name=character.name,
        sheet_url=character_sheet_url(character.sheet_key),
        is_starter=character.is_starter,
        unlocked_at=ownership.unlocked_at if ownership is not None else None,
        source_campaign_id=(
            ownership.source_campaign_id if ownership is not None else None
        ),
        can_remove=not character.is_starter,
    )


def _selected_character_id(
    db: Session, character_id: UUID | None
) -> int | None:
    """Public id of a selected character; ``NULL`` when nothing is selected."""
    if character_id is None:
        return None
    character = db.get(UiCharacter, character_id)
    return character.public_id if character is not None else None


def _require_user(db: Session, user_id: UUID) -> User:
    user = db.get(User, user_id)
    if user is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="user_not_found"
        )
    return user


def _require_character(db: Session, character_id: UUID) -> UiCharacter:
    character = db.get(UiCharacter, character_id)
    if character is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="character_not_found"
        )
    return character


@router.get(
    "/{user_id}/overview",
    response_model=AdminUserOverview,
    summary="One account's details: duels, towers and characters",
)
def user_overview(
    user_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminUserOverview:
    """Full card of one account for the admin panel.

    Three groups are aggregated independently (see
    :mod:`app.services.admin_user_stats`), and the catalogue is loaded once with
    the account's ownership rows, so the number of queries does not grow with
    the number of towers or characters.
    """
    user = _require_user(db, user_id)

    characters = db.scalars(
        select(UiCharacter).order_by(UiCharacter.name.asc(), UiCharacter.id.asc())
    ).all()
    ownership = {
        row.character_id: row
        for row in db.scalars(
            select(UserCharacter).where(UserCharacter.user_id == user.id)
        ).all()
    }

    items = [_character_item(character, ownership.get(character.id)) for character in characters]
    owned = [
        item
        for item, character in zip(items, characters)
        if character.is_starter or character.id in ownership
    ]
    selected = next(
        (
            AdminSelectedCharacter(
                id=character.id,
                public_id=character.public_id,
                name=character.name,
                sheet_url=character_sheet_url(character.sheet_key),
            )
            for character in characters
            if character.id == user.character_id
        ),
        None,
    )

    return AdminUserOverview(
        basics=AdminUserBasics(
            user_id=user.id,
            username=user.username,
            display_name=user.display_name,
            email=user.email,
            level=int(user.level or 1),
            xp_total=int(user.xp_total or 0),
            created_at=user.created_at,
            is_active=user.is_active,
            selected_character=selected,
        ),
        online=AdminOnlineStats(**online_stats(db, user_id=user.id)),
        battle=AdminBattleStats(**battle_stats(db, user_id=user.id)),
        characters=AdminUserCharacters(
            owned=owned,
            selected_character_id=selected.public_id if selected is not None else None,
            available=items,
        ),
    )


@router.post(
    "/{user_id}/characters/{character_id}",
    response_model=AdminUserCharacterChange,
    responses={
        201: {"description": "Character added to the collection"},
        200: {"description": "Character was already owned (idempotent no-op)"},
    },
    summary="Grant a character to an account",
)
def grant_user_character(
    user_id: UUID,
    character_id: UUID,
    response: Response,
    db: Session = Depends(get_db),
    current_user: User = Depends(require_admin),
) -> AdminUserCharacterChange:
    """Add one catalogue character to the account's collection.

    Idempotent: an already owned character answers 200 with ``created=false``.
    A starter is always owned, so granting it never writes a row and also
    answers 200 — the collection is already complete for it.
    """
    user = _require_user(db, user_id)
    character = _require_character(db, character_id)

    created = False
    if not character.is_starter:
        created = grant_character(db, user, character)
        db.commit()

    if created:
        response.status_code = status.HTTP_201_CREATED
        logger.info(
            "admin %s granted character %s to user %s",
            current_user.id,
            character.id,
            user.id,
        )

    row = db.scalar(
        select(UserCharacter).where(
            UserCharacter.user_id == user.id,
            UserCharacter.character_id == character.id,
        )
    )
    return AdminUserCharacterChange(
        character=_character_item(character, row),
        created=created,
        selected_character_id=_selected_character_id(db, user.character_id),
    )


@router.delete(
    "/{user_id}/characters/{character_id}",
    response_model=AdminUserCharacterRemoval,
    summary="Revoke a non-starter character from an account",
)
def revoke_user_character(
    user_id: UUID,
    character_id: UUID,
    db: Session = Depends(get_db),
    current_user: User = Depends(require_admin),
) -> AdminUserCharacterRemoval:
    """Remove one owned character and clear the selection if it pointed at it.

    Starters are owned by design (no ``user_characters`` row exists for them),
    so revoking one answers 409 ``starter_character`` instead of pretending to
    change the collection. A character the account does not own is a 404.
    """
    user = _require_user(db, user_id)
    character = _require_character(db, character_id)

    if character.is_starter:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail="starter_character"
        )

    row = db.scalar(
        select(UserCharacter).where(
            UserCharacter.user_id == user.id,
            UserCharacter.character_id == character.id,
        )
    )
    if row is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="not_owned"
        )

    db.delete(row)
    cleared_selected = user.character_id == character.id
    if cleared_selected:
        user.character_id = None
    db.commit()

    logger.info(
        "admin %s revoked character %s from user %s (selection cleared: %s)",
        current_user.id,
        character.id,
        user.id,
        cleared_selected,
    )
    return AdminUserCharacterRemoval(
        removed=True,
        cleared_selected=cleared_selected,
        selected_character_id=_selected_character_id(db, user.character_id),
    )


@router.delete(
    "/{user_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete a user account and all of its data",
)
def delete_user(
    user_id: UUID,
    db: Session = Depends(get_db),
    current_user: User = Depends(require_admin),
) -> Response:
    """Permanently delete one account.

    An administrator cannot delete their own account: that would lock the panel
    out mid-session and is almost always a misclick. Data that a future schema
    change protects with ``RESTRICT`` is reported as 409 instead of a 500.
    """
    if user_id == current_user.id:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="You cannot delete your own account",
        )

    user = db.scalar(select(User).where(User.id == user_id))
    if user is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="User not found")

    db.delete(user)
    try:
        db.commit()
    except IntegrityError:
        db.rollback()
        logger.warning(
            "admin %s could not delete user %s: related data blocks the delete",
            current_user.id,
            user_id,
        )
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="User is referenced by data that cannot be deleted",
        )

    logger.info("admin %s deleted user %s", current_user.id, user_id)
    return Response(status_code=status.HTTP_204_NO_CONTENT)
