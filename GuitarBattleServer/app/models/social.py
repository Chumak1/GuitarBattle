"""Friends, blocks, privacy, rivals, activity and social notifications.

Migration ``0031_social_friends`` adds these tables. Nothing here replaces or
duplicates the existing identity, progression or duel data: every row points at
``users.id`` and the public player card is aggregated from
:mod:`app.models.user`, :mod:`app.models.duel`, :mod:`app.models.progression`,
:mod:`app.models.campaign` and :mod:`app.models.character_unlocks`.

Storage rules:

* ``friendships`` keeps one canonical row per pair: ``user_id < friend_id``, so
  friendship is mutual by construction and can never be stored twice.
* ``friend_requests`` keeps one row per attempt. Only one *pending* request may
  exist for a pair in either direction: the partial unique index enforces the
  same-direction case and :mod:`app.services.social_service` rejects the reverse
  direction before inserting.
* ``user_blocks`` is directional (``blocker_id`` -> ``blocked_id``); the social
  service treats a block in either direction as "hidden from each other".
* ``user_privacy``/``user_rival`` are 1:1 with an account, keyed by
  ``user_id``. A missing privacy row means "all defaults" (everything visible),
  so old accounts never need a backfill.
* ``player_activity`` is the feed of visible events; ``visible`` is the
  per-row switch and ``user_privacy.show_activity`` is the account-wide one.
* ``social_notifications`` uses ``UNIQUE(user_id, dedupe_key)`` so a retried
  mutation can never deliver the same notification twice.
"""
from __future__ import annotations

import uuid
from datetime import datetime
from typing import Any

from sqlalchemy import (
    Boolean,
    CheckConstraint,
    DateTime,
    ForeignKey,
    Index,
    String,
    UniqueConstraint,
    func,
    text,
)
from sqlalchemy.dialects.postgresql import JSONB, UUID
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class Friendship(Base):
    """One mutual friendship, stored once with ``user_id < friend_id``."""

    __tablename__ = "friendships"
    __table_args__ = (
        UniqueConstraint("user_id", "friend_id", name="uq_friendships_pair"),
        CheckConstraint("user_id < friend_id", name="ck_friendships_order"),
        # The unique constraint already indexes ``user_id`` as its leftmost
        # column; this one serves "friends of <user>" from the other side.
        Index("ix_friendships_friend_id", "friend_id"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    friend_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )


class FriendRequest(Base):
    """One friend request. Only one pending row per pair (either direction)."""

    __tablename__ = "friend_requests"
    __table_args__ = (
        CheckConstraint(
            "status IN ('pending', 'accepted', 'declined', 'cancelled')",
            name="ck_friend_requests_status",
        ),
        CheckConstraint(
            "from_user_id <> to_user_id", name="ck_friend_requests_not_self"
        ),
        # Only one pending request per ordered pair at a time. The reverse
        # direction is rejected by the service, because a partial index cannot
        # express both directions.
        Index(
            "uq_friend_requests_pending_pair",
            "from_user_id",
            "to_user_id",
            unique=True,
            postgresql_where=text("status = 'pending'"),
        ),
        Index("ix_friend_requests_from_user_id", "from_user_id", "status"),
        Index("ix_friend_requests_to_user_id", "to_user_id", "status"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    from_user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    to_user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    status: Mapped[str] = mapped_column(
        String(16), nullable=False, default="pending", server_default="pending"
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    responded_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )


class UserBlock(Base):
    """A directional block. Either direction hides the pair from each other."""

    __tablename__ = "user_blocks"
    __table_args__ = (
        UniqueConstraint(
            "blocker_id", "blocked_id", name="uq_user_blocks_pair"
        ),
        CheckConstraint("blocker_id <> blocked_id", name="ck_user_blocks_not_self"),
        Index("ix_user_blocks_blocked_id", "blocked_id"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    blocker_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    blocked_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )


class UserPrivacy(Base):
    """The caller's social settings. A missing row means every default."""

    __tablename__ = "user_privacy"
    __table_args__ = (
        CheckConstraint(
            "request_policy IN ('everyone', 'friends', 'nobody')",
            name="ck_user_privacy_request_policy",
        ),
    )

    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        primary_key=True,
    )
    request_policy: Mapped[str] = mapped_column(
        String(16), nullable=False, default="everyone", server_default="everyone"
    )
    show_online: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    show_profile: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    show_stats: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    show_activity: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    allow_challenges: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )


class UserRival(Base):
    """The single rival of an account; the service requires a friendship."""

    __tablename__ = "user_rival"
    __table_args__ = (
        CheckConstraint("user_id <> rival_user_id", name="ck_user_rival_not_self"),
        Index("ix_user_rival_rival_user_id", "rival_user_id"),
    )

    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        primary_key=True,
    )
    rival_user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )


class PlayerActivity(Base):
    """One visible event of a player's feed (record, tower, duel win, ...)."""

    __tablename__ = "player_activity"
    __table_args__ = (
        CheckConstraint(
            "kind IN ('record', 'tower', 'character', 'level', 'duel_win', "
            "'achievement')",
            name="ck_player_activity_kind",
        ),
        Index("ix_player_activity_user_created", "user_id", "created_at"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    kind: Mapped[str] = mapped_column(String(16), nullable=False)
    payload: Mapped[dict[str, Any]] = mapped_column(
        JSONB, nullable=False, default=dict, server_default=text("'{}'::jsonb")
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    visible: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=True, server_default="true"
    )


class SocialNotification(Base):
    """A delivered social notification, deduplicated per account."""

    __tablename__ = "social_notifications"
    __table_args__ = (
        UniqueConstraint(
            "user_id", "dedupe_key", name="uq_social_notifications_user_dedupe"
        ),
        Index(
            "ix_social_notifications_user_created", "user_id", "created_at"
        ),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    kind: Mapped[str] = mapped_column(String(32), nullable=False)
    payload: Mapped[dict[str, Any]] = mapped_column(
        JSONB, nullable=False, default=dict, server_default=text("'{}'::jsonb")
    )
    read_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    dedupe_key: Mapped[str] = mapped_column(String(160), nullable=False)
