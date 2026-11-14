"""Private player-to-player messages: the rules behind ``/api/v1/messages``.

Only the message-specific logic lives here. Every social rule is reused from
:mod:`app.services.social_service`, so a player card, a block or an "active
account" check can never disagree between the two slices:

* :func:`social_service.active_user` — 404 for unknown or deactivated accounts;
* :func:`social_service.block_exists` — a block in either direction stops new
  messages;
* :func:`social_service.compact_cards` — the same compact player card the
  friends list and the search results return.

Behaviour notes:

* Sending to a non-friend is allowed: that is how a conversation starts.
* Reading a thread is allowed even after a block, so old messages stay
  visible; only *new* messages are rejected.
* Opening a thread marks every message from that peer read inside the same
  transaction that returns them.
* ``unread_total`` is global: it counts every unread message addressed to the
  caller, whatever the conversation.
"""
from __future__ import annotations

from datetime import datetime, timezone
from typing import Any
from uuid import UUID

from sqlalchemy import and_, func, or_, select, union_all, update
from sqlalchemy.orm import Session

from app.models.messages import MAX_BODY_LENGTH, DirectMessage
from app.models.user import User
from app.services.social_service import (
    SocialError,
    active_user,
    block_exists,
    compact_cards,
)


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _aware(value: datetime) -> datetime:
    """PostgreSQL compares ``timestamptz``; a naive cursor is taken as UTC."""
    return value if value.tzinfo is not None else value.replace(
        tzinfo=timezone.utc
    )


def _clean_body(body: str) -> str:
    """Trim, then enforce 1..2000 characters (the schema checks it too)."""
    cleaned = (body or "").strip()
    if not cleaned:
        raise SocialError(422, "invalid_body", "Message must not be empty")
    if len(cleaned) > MAX_BODY_LENGTH:
        raise SocialError(
            422,
            "invalid_body",
            f"Message must be at most {MAX_BODY_LENGTH} characters",
        )
    return cleaned


def _payload(row: DirectMessage, viewer_id: UUID) -> dict[str, Any]:
    return {
        "id": row.id,
        "body": row.body,
        "created_at": row.created_at,
        "mine": row.sender_id == viewer_id,
        "read": row.read_at is not None,
    }


def _peer(db: Session, user_id: UUID) -> User:
    """The other side of the thread; unknown or inactive accounts are 404."""
    return active_user(db, user_id)


# ---------------------------------------------------------------------------
# Badge
# ---------------------------------------------------------------------------
def unread_total(db: Session, user_id: UUID) -> int:
    """Every message addressed to ``user_id`` that was never read."""
    return int(
        db.scalar(
            select(func.count(DirectMessage.id)).where(
                DirectMessage.recipient_id == user_id,
                DirectMessage.read_at.is_(None),
            )
        )
        or 0
    )


# ---------------------------------------------------------------------------
# Send
# ---------------------------------------------------------------------------
def send_message(
    db: Session, sender: User, recipient_id: UUID, body: str
) -> dict[str, Any]:
    if recipient_id == sender.id:
        raise SocialError(
            400, "cannot_message_self", "You cannot message yourself"
        )
    recipient = _peer(db, recipient_id)
    if block_exists(db, sender.id, recipient.id):
        raise SocialError(
            403, "blocked", "One of the players blocked the other"
        )
    row = DirectMessage(
        sender_id=sender.id,
        recipient_id=recipient.id,
        body=_clean_body(body),
    )
    db.add(row)
    db.commit()
    db.refresh(row)
    return _payload(row, sender.id)


# ---------------------------------------------------------------------------
# Conversations
# ---------------------------------------------------------------------------
def _message_pairs(viewer_id: UUID):
    """Both directions of every message of the viewer, with a peer column."""
    sent = select(
        DirectMessage.sender_id.label("sender_id"),
        DirectMessage.recipient_id.label("peer_id"),
        DirectMessage.id.label("id"),
        DirectMessage.body.label("body"),
        DirectMessage.created_at.label("created_at"),
    ).where(DirectMessage.sender_id == viewer_id)
    received = select(
        DirectMessage.sender_id.label("sender_id"),
        DirectMessage.sender_id.label("peer_id"),
        DirectMessage.id.label("id"),
        DirectMessage.body.label("body"),
        DirectMessage.created_at.label("created_at"),
    ).where(DirectMessage.recipient_id == viewer_id)
    return union_all(sent, received).subquery("message_pairs")


def conversations(
    db: Session, viewer: User, *, limit: int, offset: int
) -> tuple[list[dict[str, Any]], int, int]:
    """Every peer with at least one message, newest thread first.

    Deactivated accounts are skipped: their thread answers 404, so listing
    them would give the client a row it cannot open. Blocks do not remove a
    conversation — history stays readable, only new messages are rejected.
    """
    pairs = _message_pairs(viewer.id)
    peers = (
        select(
            pairs.c.peer_id.label("peer_id"),
            func.max(pairs.c.created_at).label("last_at"),
        )
        .join(User, User.id == pairs.c.peer_id)
        .where(User.is_active.is_(True))
        .group_by(pairs.c.peer_id)
        .subquery("conversation_peers")
    )
    total = int(db.scalar(select(func.count()).select_from(peers)) or 0)
    page = db.execute(
        select(peers.c.peer_id)
        .order_by(peers.c.last_at.desc(), peers.c.peer_id.asc())
        .limit(limit)
        .offset(offset)
    ).all()
    peer_ids = [row.peer_id for row in page]
    if not peer_ids:
        return [], total, unread_total(db, viewer.id)

    # One row per peer: the newest message of that conversation.
    last_rows = db.execute(
        select(
            pairs.c.peer_id,
            pairs.c.id,
            pairs.c.body,
            pairs.c.created_at,
            pairs.c.sender_id,
        )
        .where(pairs.c.peer_id.in_(peer_ids))
        .distinct(pairs.c.peer_id)
        .order_by(
            pairs.c.peer_id,
            pairs.c.created_at.desc(),
            pairs.c.id.desc(),
        )
    ).all()
    last_by_peer = {row.peer_id: row for row in last_rows}

    unread_rows = db.execute(
        select(DirectMessage.sender_id, func.count(DirectMessage.id))
        .where(
            DirectMessage.recipient_id == viewer.id,
            DirectMessage.read_at.is_(None),
            DirectMessage.sender_id.in_(peer_ids),
        )
        .group_by(DirectMessage.sender_id)
    ).all()
    unread_by_peer = {sender_id: int(count) for sender_id, count in unread_rows}

    users = {
        user.id: user
        for user in db.scalars(
            select(User).where(User.id.in_(peer_ids))
        ).all()
    }
    ordered_users = [users[peer_id] for peer_id in peer_ids if peer_id in users]
    cards = {
        card["user_id"]: card
        for card in compact_cards(db, viewer, ordered_users)
    }

    items: list[dict[str, Any]] = []
    for peer_id in peer_ids:
        row = last_by_peer.get(peer_id)
        card = cards.get(peer_id)
        if row is None or card is None:
            continue
        items.append(
            {
                "user": card,
                "last_message": {
                    "id": row.id,
                    "body": row.body,
                    "created_at": row.created_at,
                    "mine": row.sender_id == viewer.id,
                },
                "unread": unread_by_peer.get(peer_id, 0),
            }
        )
    return items, total, unread_total(db, viewer.id)


# ---------------------------------------------------------------------------
# One thread
# ---------------------------------------------------------------------------
def thread(
    db: Session,
    viewer: User,
    peer_id: UUID,
    *,
    limit: int,
    before: datetime | None = None,
) -> dict[str, Any]:
    """The newest ``limit`` messages with ``peer_id``, oldest first.

    ``before`` is an older-than cursor on ``created_at``. The page is read
    before the read marker is written, so the response already reflects it.
    """
    peer = _peer(db, peer_id)
    stmt = select(DirectMessage).where(
        or_(
            and_(
                DirectMessage.sender_id == viewer.id,
                DirectMessage.recipient_id == peer.id,
            ),
            and_(
                DirectMessage.sender_id == peer.id,
                DirectMessage.recipient_id == viewer.id,
            ),
        )
    )
    if before is not None:
        stmt = stmt.where(DirectMessage.created_at < _aware(before))
    rows = list(
        db.scalars(
            stmt.order_by(
                DirectMessage.created_at.desc(), DirectMessage.id.desc()
            ).limit(limit + 1)
        ).all()
    )
    has_more = len(rows) > limit
    rows = rows[:limit]
    rows.reverse()

    now = _now()
    items: list[dict[str, Any]] = []
    for row in rows:
        payload = _payload(row, viewer.id)
        if row.recipient_id == viewer.id:
            # This call marks it read, so it is read from the caller's side.
            payload["read"] = True
        items.append(payload)

    db.execute(
        update(DirectMessage)
        .where(
            DirectMessage.recipient_id == viewer.id,
            DirectMessage.sender_id == peer.id,
            DirectMessage.read_at.is_(None),
        )
        .values(read_at=now)
        .execution_options(synchronize_session=False)
    )
    db.commit()

    card = compact_cards(db, viewer, [peer])[0]
    return {"peer": card, "items": items, "has_more": has_more}


def mark_thread_read(db: Session, viewer: User, peer_id: UUID) -> int:
    """Mark everything from ``peer_id`` read; return the remaining badge."""
    peer = _peer(db, peer_id)
    db.execute(
        update(DirectMessage)
        .where(
            DirectMessage.recipient_id == viewer.id,
            DirectMessage.sender_id == peer.id,
            DirectMessage.read_at.is_(None),
        )
        .values(read_at=_now())
        .execution_options(synchronize_session=False)
    )
    db.commit()
    return unread_total(db, viewer.id)
