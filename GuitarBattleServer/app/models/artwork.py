"""Administrator-managed artwork slots.

One row per public artwork slot (the two HOME hero backgrounds and the default
player avatar). A slot is unique, so uploading a new image replaces the row and
deletes the previous file. ``url`` is origin-relative, exactly like campaign
covers: the client resolves it against the server origin.
"""
from __future__ import annotations

from datetime import datetime

from sqlalchemy import DateTime, Integer, String, func
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class ArtworkAsset(Base):
    """An uploaded image for one slot (``slot`` is unique)."""

    __tablename__ = "artwork_assets"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    #: ``hero-battle`` | ``hero-duel`` | ``default-avatar``.
    slot: Mapped[str] = mapped_column(String(32), nullable=False, unique=True)
    #: Origin-relative URL, e.g. ``/media/artwork/hero-battle-<uuid>.png``.
    url: Mapped[str] = mapped_column(String(500), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=func.now(),
        onupdate=func.now(),
    )
