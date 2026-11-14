"""Private player-to-player messages.

Migration ``0033_direct_messages`` adds the single ``direct_messages`` table.
Nothing here replaces the social tables: a message only points at ``users.id``
of both sides and the rest of the player card is aggregated by
:mod:`app.services.social_service`.

Storage rules:

* one row is one message in one direction, so a thread is the union of both
  directions and no conversation row has to be maintained;
* ``body`` is stored already trimmed by the service and the check constraint
  keeps it inside ``1..2000`` characters;
* ``read_at`` is NULL until the recipient opens the thread. It is the only
  read marker: the sender never writes it;
* ``(sender_id, recipient_id, created_at)`` serves "my messages to that peer in
  order" and ``(recipient_id, read_at)`` serves the unread badge.
"""
from __future__ import annotations

import uuid
from datetime import datetime

from sqlalchemy import (
    CheckConstraint,
    DateTime,
    ForeignKey,
    Index,
    Text,
    func,
)
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base

#: Longest accepted message body, in characters.
MAX_BODY_LENGTH = 2000


class DirectMessage(Base):
    """One private message between two players."""

    __tablename__ = "direct_messages"
    __table_args__ = (
        CheckConstraint(
            "sender_id <> recipient_id", name="ck_direct_messages_not_self"
        ),
        CheckConstraint(
            f"char_length(body) >= 1 AND char_length(body) <= {MAX_BODY_LENGTH}",
            name="ck_direct_messages_body_length",
        ),
        Index(
            "ix_direct_messages_sender_recipient_created",
            "sender_id",
            "recipient_id",
            "created_at",
        ),
        Index("ix_direct_messages_recipient_read", "recipient_id", "read_at"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    sender_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    recipient_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    body: Mapped[str] = mapped_column(Text, nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    read_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
