"""Gameplay visuals: UI-level characters and stage backgrounds.

A *UI level* is a character the player can pick: one transparent PNG spritesheet
with eight poses laid out as a 4x2 grid. A *stage level* is a standalone
background image with a scene name; it is not tied to a character.

Both are catalogue content managed from the admin panel. Songs reference them
optionally and independently, so the public catalogue can hand the client the
spritesheet and the background of the selected song.

Deletion rules (mirrored by the foreign keys in migrations ``0022`` and
``0025``):

* ``songs.character_id`` / ``songs.stage_id`` -> ``SET NULL``: deleting a visual
  never breaks a song, it only clears the optional selection.
* ``users.character_id`` -> ``SET NULL``: deleting a character only clears the
  player's choice.
* ``user_characters.character_id`` -> ``CASCADE`` (migration ``0029``): deleting
  a character also removes it from every collection.
"""
import uuid
from datetime import datetime

from sqlalchemy import (
    BigInteger,
    Boolean,
    DateTime,
    Identity,
    Integer,
    String,
    func,
)
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class UiCharacter(Base):
    """A playable character: a display name plus its 8-pose spritesheet."""

    __tablename__ = "ui_characters"

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    #: Stable player-facing identifier (``GET /characters`` and
    #: ``PATCH /auth/me``). The internal primary key stays a UUID so the admin
    #: API and ``songs.character_id`` keep their existing contract; the desktop
    #: client only ever sees this integer.
    public_id: Mapped[int] = mapped_column(
        BigInteger, Identity(), unique=True, nullable=False
    )
    #: Unique display name shown in the admin panel and in the song payload
    #: (``character_name``), e.g. ``Bard``.
    name: Mapped[str] = mapped_column(
        String(120), unique=True, index=True, nullable=False
    )
    #: Starter characters belong to every account from registration, so a new
    #: player can pick them without clearing a tower. Migration ``0029`` flags
    #: ``Ray`` and ``Luna`` and backfills the collection of existing users.
    is_starter: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=False, server_default="false"
    )
    #: Generated file name inside ``settings.gameplay_media_dir/characters``.
    #: The original upload name is never used as a path.
    sheet_key: Mapped[str] = mapped_column(String(255), nullable=False)
    original_name: Mapped[str | None] = mapped_column(String(255), nullable=True)
    content_type: Mapped[str] = mapped_column(
        String(120), nullable=False, default="image/png"
    )
    size_bytes: Mapped[int] = mapped_column(Integer, nullable=False)
    sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )


class StageLevel(Base):
    """A standalone stage background with a scene name (no character link)."""

    __tablename__ = "stage_levels"

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    name: Mapped[str] = mapped_column(String(120), nullable=False)
    #: Generated file name inside ``settings.gameplay_media_dir/stages``.
    background_key: Mapped[str] = mapped_column(String(255), nullable=False)
    original_name: Mapped[str | None] = mapped_column(String(255), nullable=True)
    content_type: Mapped[str] = mapped_column(
        String(120), nullable=False, default="image/png"
    )
    size_bytes: Mapped[int] = mapped_column(Integer, nullable=False)
    sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )
