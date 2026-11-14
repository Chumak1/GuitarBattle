"""Social API: player cards, friends, blocks, privacy, rival, feed, inbox.

All endpoints are authenticated and live under ``/api/v1/social``. Mutations
verify ownership server-side (only the addressee accepts/declines, only the
sender cancels) and every social rule lives in
:mod:`app.services.social_service`.
"""
from __future__ import annotations

from typing import Literal
from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query, Response, status
from sqlalchemy.orm import Session

from app.api.v1.auth import get_current_user
from app.database import get_db
from app.models.social import FriendRequest
from app.models.user import User
from app.schemas.social import (
    ActivityResponse,
    FriendRequestItem,
    FriendsResponse,
    LeaderboardResponse,
    NotificationsReadRequest,
    NotificationsReadResponse,
    NotificationsResponse,
    PrivacySettings,
    PrivacyUpdate,
    PublicPlayerCard,
    RequestsResponse,
    RivalItem,
    RivalResponse,
    RivalUpdate,
    SearchResponse,
    SocialStats,
)
from app.services import social_service

router = APIRouter(prefix="/social", tags=["social"])


def _http(exc: social_service.SocialError) -> HTTPException:
    return HTTPException(status_code=exc.status_code, detail=exc.detail())


def _target(db: Session, user_id: UUID) -> User:
    try:
        return social_service.active_user(db, user_id)
    except social_service.SocialError as exc:
        raise _http(exc) from exc


def _request_item(
    db: Session, viewer: User, request: FriendRequest
) -> FriendRequestItem:
    other_id = (
        request.from_user_id
        if request.to_user_id == viewer.id
        else request.to_user_id
    )
    other = db.get(User, other_id)
    if other is None:
        raise HTTPException(
            status_code=404,
            detail={"code": "user_not_found", "message": "Player not found"},
        )
    card = social_service.compact_cards(db, viewer, [other])[0]
    payload = social_service.request_payload(request, other, card)
    return FriendRequestItem.model_validate(payload)


# ---------------------------------------------------------------------------
# Player cards
# ---------------------------------------------------------------------------
@router.get(
    "/players/{user_id}",
    response_model=PublicPlayerCard,
    summary="Aggregated public card of a player (privacy-aware)",
)
def player_card(
    user_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> PublicPlayerCard:
    target = _target(db, user_id)
    try:
        return PublicPlayerCard.model_validate(
            social_service.public_card(db, user, target)
        )
    except social_service.SocialError as exc:
        raise _http(exc) from exc


@router.get(
    "/players/{user_id}/card",
    response_model=PublicPlayerCard,
    summary="Alias of the aggregated public card of a player",
)
def player_card_alias(
    user_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> PublicPlayerCard:
    return player_card(user_id, db=db, user=user)


@router.get(
    "/players/{user_id}/stats",
    response_model=SocialStats,
    summary="Extended statistics block of a player",
)
def player_stats(
    user_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> SocialStats:
    target = _target(db, user_id)
    try:
        return SocialStats.model_validate(
            social_service.player_stats(db, user, target)
        )
    except social_service.SocialError as exc:
        raise _http(exc) from exc


@router.get(
    "/search",
    response_model=SearchResponse,
    summary="Find players by username or display name",
)
def search_players(
    q: str = Query(default="", max_length=64),
    limit: int = Query(default=20, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> SearchResponse:
    items, total = social_service.search_players(
        db, user, query=q, limit=limit, offset=offset
    )
    return SearchResponse(
        items=items, total=total, limit=limit, offset=offset, query=q.strip()
    )


# ---------------------------------------------------------------------------
# Friends
# ---------------------------------------------------------------------------
@router.get(
    "/friends",
    response_model=FriendsResponse,
    summary="Friends of the caller with presence",
)
def list_friends(
    limit: int = Query(default=50, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> FriendsResponse:
    items, total = social_service.list_friends(
        db, user, limit=limit, offset=offset
    )
    return FriendsResponse(
        items=items, total=total, limit=limit, offset=offset
    )


@router.post(
    "/friends/{user_id}",
    response_model=FriendRequestItem,
    status_code=status.HTTP_201_CREATED,
    summary="Send a friend request",
)
def send_friend_request(
    user_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> FriendRequestItem:
    try:
        payload = social_service.send_friend_request(db, user, user_id)
    except social_service.SocialError as exc:
        raise _http(exc) from exc
    return FriendRequestItem.model_validate(payload)


@router.delete(
    "/friends/{user_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Remove a friendship (either direction)",
)
def remove_friend(
    user_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> Response:
    try:
        social_service.remove_friend(db, user, user_id)
    except social_service.SocialError as exc:
        raise _http(exc) from exc
    return Response(status_code=status.HTTP_204_NO_CONTENT)


# ---------------------------------------------------------------------------
# Requests
# ---------------------------------------------------------------------------
@router.get(
    "/requests",
    response_model=RequestsResponse,
    summary="Pending friend requests of the caller",
)
def list_requests(
    direction: Literal["incoming", "outgoing"] = Query(default="incoming"),
    limit: int = Query(default=50, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> RequestsResponse:
    items, total = social_service.list_requests(
        db, user, direction=direction, limit=limit, offset=offset
    )
    return RequestsResponse(
        items=items, total=total, limit=limit, offset=offset
    )


@router.post(
    "/requests/{request_id}/accept",
    response_model=FriendRequestItem,
    summary="Accept a friend request (addressee only, idempotent)",
)
def accept_request(
    request_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> FriendRequestItem:
    try:
        request = social_service.accept_request(db, user, request_id)
    except social_service.SocialError as exc:
        raise _http(exc) from exc
    return _request_item(db, user, request)


@router.post(
    "/requests/{request_id}/decline",
    response_model=FriendRequestItem,
    summary="Decline a friend request (addressee only, idempotent)",
)
def decline_request(
    request_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> FriendRequestItem:
    try:
        request = social_service.decline_request(db, user, request_id)
    except social_service.SocialError as exc:
        raise _http(exc) from exc
    return _request_item(db, user, request)


@router.delete(
    "/requests/{request_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Cancel a sent friend request (sender only, idempotent)",
)
def cancel_request(
    request_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> Response:
    try:
        social_service.cancel_request(db, user, request_id)
    except social_service.SocialError as exc:
        raise _http(exc) from exc
    return Response(status_code=status.HTTP_204_NO_CONTENT)


# ---------------------------------------------------------------------------
# Blocks
# ---------------------------------------------------------------------------
@router.post(
    "/blocks/{user_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Block a player: ends friendship and pending requests",
)
def block_user(
    user_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> Response:
    try:
        social_service.block_user(db, user, user_id)
    except social_service.SocialError as exc:
        raise _http(exc) from exc
    return Response(status_code=status.HTTP_204_NO_CONTENT)


@router.delete(
    "/blocks/{user_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Unblock a player (idempotent)",
)
def unblock_user(
    user_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> Response:
    try:
        social_service.unblock_user(db, user, user_id)
    except social_service.SocialError as exc:
        raise _http(exc) from exc
    return Response(status_code=status.HTTP_204_NO_CONTENT)


# ---------------------------------------------------------------------------
# Leaderboard
# ---------------------------------------------------------------------------
@router.get(
    "/leaderboard",
    response_model=LeaderboardResponse,
    summary="Friends and self ranked by rating, level, wins or win rate",
)
def friends_leaderboard(
    sort: Literal["rating", "level", "wins", "win_rate", "guitar_skills"] = Query(
        default="rating"
    ),
    limit: int = Query(default=50, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> LeaderboardResponse:
    items, total = social_service.leaderboard(
        db, user, sort=sort, limit=limit, offset=offset
    )
    return LeaderboardResponse(
        sort=sort, items=items, total=total, limit=limit, offset=offset
    )


# ---------------------------------------------------------------------------
# Privacy
# ---------------------------------------------------------------------------
@router.get(
    "/privacy",
    response_model=PrivacySettings,
    summary="The caller's social privacy settings",
)
def get_privacy(
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> PrivacySettings:
    social_service.ensure_privacy_row(db, user.id)
    db.commit()
    return PrivacySettings.model_validate(
        social_service.privacy_settings(db, user.id)
    )


@router.patch(
    "/privacy",
    response_model=PrivacySettings,
    summary="Update the caller's social privacy settings",
)
def update_privacy(
    payload: PrivacyUpdate,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> PrivacySettings:
    try:
        settings = social_service.update_privacy(
            db, user.id, payload.model_dump(exclude_unset=True)
        )
    except social_service.SocialError as exc:
        raise _http(exc) from exc
    return PrivacySettings.model_validate(settings)


# ---------------------------------------------------------------------------
# Rival
# ---------------------------------------------------------------------------
@router.get(
    "/rival",
    response_model=RivalResponse,
    summary="The caller's rival (must be a friend)",
)
def get_rival(
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> RivalResponse:
    rival = social_service.get_rival(db, user)
    return RivalResponse(
        rival=RivalItem.model_validate(rival) if rival is not None else None
    )


@router.put(
    "/rival",
    response_model=RivalItem,
    summary="Set the single rival (must be a friend)",
)
def set_rival(
    payload: RivalUpdate,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> RivalItem:
    try:
        rival = social_service.set_rival(db, user, payload.user_id)
    except social_service.SocialError as exc:
        raise _http(exc) from exc
    return RivalItem.model_validate(rival)


@router.delete(
    "/rival",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Clear the rival (idempotent)",
)
def clear_rival(
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> Response:
    social_service.clear_rival(db, user)
    return Response(status_code=status.HTTP_204_NO_CONTENT)


# ---------------------------------------------------------------------------
# Activity and notifications
# ---------------------------------------------------------------------------
@router.get(
    "/activity",
    response_model=ActivityResponse,
    summary="Activity of friends that privacy allows",
)
def activity_feed(
    limit: int = Query(default=30, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> ActivityResponse:
    items, total = social_service.activity_feed(
        db, user, limit=limit, offset=offset
    )
    return ActivityResponse(
        items=items, total=total, limit=limit, offset=offset
    )


@router.get(
    "/notifications",
    response_model=NotificationsResponse,
    summary="Social notifications of the caller",
)
def list_notifications(
    unread_only: bool = Query(default=False),
    limit: int = Query(default=50, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> NotificationsResponse:
    rows, total, unread = social_service.list_notifications(
        db, user, unread_only=unread_only, limit=limit, offset=offset
    )
    return NotificationsResponse(
        items=[
            {
                "id": row.id,
                "kind": row.kind,
                "payload": row.payload or {},
                "read_at": row.read_at,
                "created_at": row.created_at,
            }
            for row in rows
        ],
        total=total,
        unread=unread,
        limit=limit,
        offset=offset,
    )


@router.post(
    "/notifications/read",
    response_model=NotificationsReadResponse,
    summary="Mark notifications read by ids or all unread",
)
def mark_notifications_read(
    payload: NotificationsReadRequest,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> NotificationsReadResponse:
    try:
        updated = social_service.mark_notifications_read(
            db, user, ids=payload.ids, all_unread=payload.all
        )
    except social_service.SocialError as exc:
        raise _http(exc) from exc
    return NotificationsReadResponse(updated=updated)
