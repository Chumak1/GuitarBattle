"""Pydantic schemas for ``/api/v1/messages`` (private player messages).

The JUCE client is written against these exact names, so fields are never
renamed: ``user``/``last_message``/``unread`` in a conversation row,
``peer``/``items``/``has_more`` in a thread and ``mine``/``read`` on a message.
"""
from __future__ import annotations

from datetime import datetime
from uuid import UUID

from pydantic import BaseModel, Field, field_validator

from app.models.messages import MAX_BODY_LENGTH
from app.schemas.social import CompactPlayer


class MessageCreate(BaseModel):
    """The only accepted request body of ``POST /messages/{user_id}``."""

    body: str = Field(min_length=1)

    @field_validator("body")
    @classmethod
    def _clean(cls, value: str) -> str:
        cleaned = value.strip()
        if not cleaned:
            raise ValueError("body must not be empty")
        if len(cleaned) > MAX_BODY_LENGTH:
            raise ValueError(
                f"body must be at most {MAX_BODY_LENGTH} characters"
            )
        return cleaned


class MessageItem(BaseModel):
    """One message. ``mine`` is relative to the caller; ``read`` means the
    recipient has opened it (always true for the caller's inbound messages
    once the thread was opened)."""

    id: UUID
    body: str
    created_at: datetime
    mine: bool
    read: bool = False


class LastMessage(BaseModel):
    """The newest message of a conversation, without the read marker."""

    id: UUID
    body: str
    created_at: datetime
    mine: bool


class ConversationItem(BaseModel):
    user: CompactPlayer
    last_message: LastMessage
    #: Unread messages from ``user`` to the caller.
    unread: int = 0


class ConversationsResponse(BaseModel):
    items: list[ConversationItem]
    total: int
    #: Unread messages over every conversation, independent of paging.
    unread_total: int = 0


class ThreadResponse(BaseModel):
    peer: CompactPlayer
    #: Oldest first, so the client appends new pages at the bottom.
    items: list[MessageItem]
    has_more: bool = False


class UnreadResponse(BaseModel):
    unread_total: int = 0
