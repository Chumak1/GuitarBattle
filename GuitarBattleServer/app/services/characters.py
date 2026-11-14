"""Character ownership helpers shared by the API and the campaign rewards.

Ownership rule
--------------
A character is owned by a user when it is a *starter*
(``ui_characters.is_starter``) or when a ``user_characters`` row exists. Rows are
inserted idempotently with ``ON CONFLICT DO NOTHING``, so replaying a completed
tower, reporting progress twice or registering twice can never duplicate a row
or lose an already granted reward.

The service never accepts a character *name* or a client-supplied "unlock"
request: rewards are always resolved on the server from
``campaigns.reward_character_id`` and granted by :func:`grant_character`.
"""
from __future__ import annotations

import uuid
from typing import Union

from sqlalchemy import select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.orm import Session

from app.models.character_unlocks import UserCharacter
from app.models.gameplay_visuals import UiCharacter

CharacterRef = Union[UiCharacter, uuid.UUID, int, None]


def starter_characters(db: Session) -> list[UiCharacter]:
    """Characters every account owns from registration, ordered by name."""
    return list(
        db.scalars(
            select(UiCharacter)
            .where(UiCharacter.is_starter.is_(True))
            .order_by(UiCharacter.name.asc(), UiCharacter.id.asc())
        ).all()
    )


def resolve_character(db: Session, character: CharacterRef) -> UiCharacter | None:
    """Accept an ORM row, an internal UUID or a player-facing public id."""
    if character is None:
        return None
    if isinstance(character, UiCharacter):
        return character
    if isinstance(character, uuid.UUID):
        return db.get(UiCharacter, character)
    if isinstance(character, bool):  # bool is an int subclass; never a valid id
        return None
    if isinstance(character, int):
        return db.scalar(
            select(UiCharacter).where(UiCharacter.public_id == character)
        )
    return None


def owned_character_ids(db: Session, user) -> set[uuid.UUID]:
    """Starter characters plus every explicitly granted character."""
    owned = set(
        db.scalars(
            select(UiCharacter.id).where(UiCharacter.is_starter.is_(True))
        ).all()
    )
    owned.update(
        db.scalars(
            select(UserCharacter.character_id).where(
                UserCharacter.user_id == user.id
            )
        ).all()
    )
    return owned


def is_character_owned(db: Session, user, character: CharacterRef) -> bool:
    resolved = resolve_character(db, character)
    if resolved is None:
        return False
    return resolved.id in owned_character_ids(db, user)


def grant_character(
    db: Session,
    user,
    character: CharacterRef,
    source_campaign_id: uuid.UUID | None = None,
) -> bool:
    """Add a character to the user's collection; idempotent.

    Returns ``True`` when a row was inserted and ``False`` when the character was
    already owned. The caller owns the transaction and must commit.

    The insert asks for ``RETURNING``: a bare ``rowcount`` of an
    ``ON CONFLICT DO NOTHING`` statement is not a reliable "was anything
    inserted" signal on every driver version (it may be ``-1``), while a row
    comes back exactly when the insert actually happened.
    """
    resolved = resolve_character(db, character)
    if resolved is None:
        return False

    inserted = db.execute(
        pg_insert(UserCharacter.__table__)
        .values(
            user_id=user.id,
            character_id=resolved.id,
            source_campaign_id=source_campaign_id,
        )
        .on_conflict_do_nothing(index_elements=["user_id", "character_id"])
        .returning(UserCharacter.user_id)
    ).first()
    db.flush()
    return inserted is not None
