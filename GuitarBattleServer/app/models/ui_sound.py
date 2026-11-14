"""UI sound events: the uploaded files and per-account playback preferences.

The catalog itself (keys, labels, categories) is fixed in
:mod:`app.services.ui_sound_registry` and seeded by migration ``0032_ui_sounds``.
This model only stores the mutable part:

* the optional uploaded file (``stored_name`` ... ``size_bytes``) plus the
  ``version`` the client uses to cache a file and detect a replacement;
* ``active`` and ``gain``, the two knobs the admin panel exposes.

``user_ui_sound_settings`` is 1:1 with an account and keyed by ``user_id``. A
missing row means "all defaults" (sounds on, volume 0.8, every category on), so
old accounts never need a backfill.
"""
from __future__ import annotations

import uuid
from datetime import datetime

from sqlalchemy import (
    BigInteger,
    Boolean,
    CheckConstraint,
    DateTime,
    Float,
    ForeignKey,
    Integer,
    String,
    func,
)
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class UiSoundEvent(Base):
    """One registry event and its current (optional) audio file."""

    __tablename__ = "ui_sound_events"
    __table_args__ = (
        CheckConstraint(
            "category IN ('navigation', 'action', 'system')",
            name="ck_ui_sound_events_category",
        ),
        CheckConstraint(
            "gain >= 0 AND gain <= 1", name="ck_ui_sound_events_gain"
        ),
        CheckConstraint("version >= 0", name="ck_ui_sound_events_version"),
    )

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    #: Stable identifier used by the client (``nav.section``).
    key: Mapped[str] = mapped_column(
        String(64), unique=True, index=True, nullable=False
    )
    name: Mapped[str] = mapped_column(String(120), nullable=False)
    category: Mapped[str] = mapped_column(String(16), nullable=False)
    active: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    gain: Mapped[float] = mapped_column(
        Float, nullable=False, default=1.0, server_default="1"
    )
    #: Generated file name inside ``gameplay_media_dir/ui_sounds``. Never a
    #: client-supplied path and never exposed as a filesystem location.
    stored_name: Mapped[str | None] = mapped_column(String(255), nullable=True)
    original_name: Mapped[str | None] = mapped_column(String(255), nullable=True)
    content_type: Mapped[str | None] = mapped_column(String(120), nullable=True)
    size_bytes: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    #: Bumped on every upload and every delete. The manifest carries it so the
    #: client can cache a file by URL and refetch only when it changes.
    version: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )


class UserUiSoundSettings(Base):
    """Per-account UI sound preferences. A missing row means all defaults."""

    __tablename__ = "user_ui_sound_settings"
    __table_args__ = (
        CheckConstraint(
            "volume >= 0 AND volume <= 1", name="ck_user_ui_sound_settings_volume"
        ),
    )

    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        primary_key=True,
    )
    enabled: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    volume: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.8, server_default="0.8"
    )
    navigation: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    action: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    system: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )
