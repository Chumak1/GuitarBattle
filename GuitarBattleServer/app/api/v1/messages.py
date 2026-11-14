"""Private player-to-player messages: ``/api/v1/messages``.

Every endpoint requires a bearer token; all rules live in
:mod:`app.services.message_service`. Literal paths (``/conversations``,
``/unread``) are declared before ``/{user_id}`` so they are never parsed as a
UUID.
"""
from __future__ import annotations

from datetime import datetime
from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query, status
from sqlalchemy.orm import Session

from app.api.v1.auth import get_current_user
from app.database import get_db
from app.models.user import User
from app.schemas.messages import (
    ConversationsResponse,
    MessageCreate,
    MessageItem,
    ThreadResponse,
    UnreadResponse,
)
from app.services import message_service
from app.services.social_service import SocialError

router = APIRouter(prefix="/messages", tags=["messages"])


def _http(exc: SocialError) -> HTTPException:
    return HTTPException(status_code=exc.status_code, detail=exc.detail())


@router.get(
    "/conversations",
    response_model=ConversationsResponse,
    summary="Conversations of the caller, newest message first",
)
def list_conversations(
    limit: int = Query(default=50, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> ConversationsResponse:
    items, total, unread = message_service.conversations(
        db, user, limit=limit, offset=offset
    )
    return ConversationsResponse(
        items=items, total=total, unread_total=unread
    )


@router.get(
    "/unread",
    response_model=UnreadResponse,
    summary="Number of unread messages (badge endpoint)",
)
def unread(
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> UnreadResponse:
    return UnreadResponse(
        unread_total=message_service.unread_total(db, user.id)
    )


@router.get(
    "/{user_id}",
    response_model=ThreadResponse,
    summary="Messages exchanged with a player, oldest first",
)
def read_thread(
    user_id: UUID,
    limit: int = Query(default=50, ge=1, le=100),
    before: datetime | None = Query(
        default=None, description="Older-than cursor on created_at"
    ),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> ThreadResponse:
    try:
        payload = message_service.thread(
            db, user, user_id, limit=limit, before=before
        )
    except SocialError as exc:
        raise _http(exc) from exc
    return ThreadResponse.model_validate(payload)


@router.post(
    "/{user_id}",
    response_model=MessageItem,
    status_code=status.HTTP_201_CREATED,
    summary="Send a private message",
)
def send_message(
    user_id: UUID,
    payload: MessageCreate,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> MessageItem:
    try:
        message = message_service.send_message(db, user, user_id, payload.body)
    except SocialError as exc:
        raise _http(exc) from exc
    return MessageItem.model_validate(message)


@router.post(
    "/{user_id}/read",
    response_model=UnreadResponse,
    summary="Mark the whole thread from this peer read",
)
def mark_thread_read(
    user_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> UnreadResponse:
    try:
        unread_total = message_service.mark_thread_read(db, user, user_id)
    except SocialError as exc:
        raise _http(exc) from exc
    return UnreadResponse(unread_total=unread_total)
