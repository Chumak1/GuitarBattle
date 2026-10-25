from datetime import datetime
from typing import Any

from sqlalchemy import BigInteger, Boolean, DateTime, Integer, String, func, text
from sqlalchemy.dialects.postgresql import JSONB
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class LandingSettings(Base):
    """A single row (id=1) holding the landing page overrides.

    ``content`` keeps two keys: ``blocks`` (free-form CMS overrides served by
    ``GET /api/v1/landing``) and ``hero_background`` (type + urls). Keeping the
    whole document in one JSONB column makes adding new CMS fields a client-only
    change: unknown keys round-trip without a migration.
    """

    __tablename__ = "landing_settings"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    content: Mapped[dict[str, Any]] = mapped_column(
        JSONB,
        nullable=False,
        default=dict,
        server_default=text("'{}'::jsonb"),
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=func.now(),
        onupdate=func.now(),
    )


class GameBuild(Base):
    """An uploaded desktop installer. At most one build is active at a time."""

    __tablename__ = "game_builds"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    version: Mapped[str] = mapped_column(
        String(64), nullable=False, default="", server_default=""
    )
    # Original client-facing file name; never used as a storage path.
    filename: Mapped[str] = mapped_column(String(255), nullable=False)
    # Random server-side name inside settings.game_builds_dir.
    stored_name: Mapped[str] = mapped_column(String(255), nullable=False)
    content_type: Mapped[str] = mapped_column(
        String(128), nullable=False, default="application/octet-stream"
    )
    size_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    is_active: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=False, server_default=text("false"), index=True
    )
    uploaded_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
