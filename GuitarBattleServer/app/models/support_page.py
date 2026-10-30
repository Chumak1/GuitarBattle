"""Support page CMS: draft/published documents, version history and methods.

The page itself is static (``app/static/app/support.html``); every piece of
content is served from these tables. Keeping the section document in JSONB lets
the editor add fields without a migration, while support methods live in their
own table because they are independent, orderable entities shown in several
sections of the page.
"""
from __future__ import annotations

import uuid
from datetime import date, datetime
from typing import Any

from sqlalchemy import (
    Boolean,
    Date,
    DateTime,
    ForeignKey,
    Integer,
    String,
    UniqueConstraint,
    func,
    text,
)
from sqlalchemy.dialects.postgresql import JSONB, UUID
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class SupportPage(Base):
    """Single row (id=1): the draft, the published snapshot and its revision.

    ``draft_content`` is what the administrator edits. ``published_content`` is
    what visitors see, so saving a draft never changes the public page until
    ``publish`` copies one into the other. ``revision`` increments on every
    draft save and is used for optimistic locking (``409 Conflict``).
    """

    __tablename__ = "support_page"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    draft_content: Mapped[dict[str, Any]] = mapped_column(
        JSONB, nullable=False, default=dict, server_default=text("'{}'::jsonb")
    )
    published_content: Mapped[dict[str, Any]] = mapped_column(
        JSONB, nullable=False, default=dict, server_default=text("'{}'::jsonb")
    )
    revision: Mapped[int] = mapped_column(
        Integer, nullable=False, default=1, server_default=text("1")
    )
    published_revision: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default=text("0")
    )
    draft_updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
    draft_updated_by: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="SET NULL"),
        nullable=True,
    )
    published_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    published_by: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="SET NULL"),
        nullable=True,
    )
    # Seed marker: defaults are inserted once, so deleting a default support
    # method stays deleted instead of being re-created on the next request.
    seed_version: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default=text("0")
    )


class SupportPageVersion(Base):
    """Immutable snapshot written on every publish (and never rewritten)."""

    __tablename__ = "support_page_versions"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    content: Mapped[dict[str, Any]] = mapped_column(
        JSONB, nullable=False, default=dict, server_default=text("'{}'::jsonb")
    )
    revision: Mapped[int] = mapped_column(Integer, nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
    created_by: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="SET NULL"),
        nullable=True,
    )
    comment: Mapped[str] = mapped_column(
        String(500), nullable=False, default="", server_default=""
    )


class SupportMethod(Base):
    """One external support link (Boosty, DALink, ...), shown as a button."""

    __tablename__ = "support_methods"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    key: Mapped[str] = mapped_column(String(40), nullable=False, unique=True)
    title: Mapped[str] = mapped_column(
        String(120), nullable=False, default="", server_default=""
    )
    button_label: Mapped[str] = mapped_column(
        String(160), nullable=False, default="", server_default=""
    )
    # Validated as an absolute http(s) URL before it is stored.
    url: Mapped[str] = mapped_column(String(2048), nullable=False)
    description: Mapped[str] = mapped_column(
        String(500), nullable=False, default="", server_default=""
    )
    is_active: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default=text("true")
    )
    open_in_new_tab: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default=text("true")
    )
    sort_order: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default=text("0")
    )
    style: Mapped[str] = mapped_column(
        String(20), nullable=False, default="primary", server_default="primary"
    )
    # Section keys the button is rendered in: ``["hero", "cta"]``.
    sections: Mapped[list[str]] = mapped_column(
        JSONB, nullable=False, default=list, server_default=text("'[]'::jsonb")
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=func.now(),
        onupdate=func.now(),
    )


class SupportEventCounter(Base):
    """Aggregated, privacy-free support page telemetry.

    One row per (event, method key, UTC day): the counters are incremented with
    an upsert, so the table grows with days rather than with visits. No IP
    address, user id, cookie or user agent is stored.
    """

    __tablename__ = "support_event_counters"
    __table_args__ = (
        UniqueConstraint(
            "event", "method_key", "day", name="uq_support_events_event_method_day"
        ),
    )

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    event: Mapped[str] = mapped_column(String(32), nullable=False)
    method_key: Mapped[str] = mapped_column(
        String(40), nullable=False, default="", server_default=""
    )
    day: Mapped[date] = mapped_column(Date, nullable=False)
    count: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default=text("0")
    )
