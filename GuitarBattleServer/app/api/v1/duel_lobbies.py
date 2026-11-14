"""Shared duel lobbies REST API (``/api/v1/duels/lobbies``).

The three phases of one room:

* ``POST /duels/lobbies`` — the host creates a named room (optional password),
  no song yet;
* chat + song picker while the room waits for the second player;
* ``POST /duels/lobbies/{id}/start/request`` — the host opens the "start the
  duel?" confirmation; both players answer with ``/start/confirm`` (the match
  is created only when both agreed, and it stays ``waiting`` until both
  downloads finish and both post ``/duels/{match_id}/ready``) or
  ``/start/decline`` (the room keeps working);
* ``POST /duels/lobbies/{id}/start`` — legacy immediate start kept for
  clients that do not know the confirmation step.

This router is registered *before* the ``/duels`` router so the literal
``/duels/lobbies`` path is matched before ``GET /duels/{match_id}`` (which would
otherwise try to parse "lobbies" as a UUID).

Errors use the same envelope as the rest of the duels API:
``{"detail": {"code": "...", "message": "..."}}``.
"""
from __future__ import annotations

from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query, status
from sqlalchemy.orm import Session

from app.api.v1.auth import get_current_user
from app.database import get_db
from app.models.user import User
from app.schemas.duels import (
    DuelChatHistoryResponse,
    DuelChatMessageOut,
    DuelRoomChatIn,
    DuelRoomCreate,
    DuelRoomJoin,
    DuelRoomListResponse,
    DuelRoomSongSelect,
    DuelRoomStartResponse,
    DuelRoomState,
)
from app.services import duel_lobby_service, duel_service
from app.services.duel_rooms import duel_chat_limiter, duel_lobby_create_limiter

router = APIRouter(prefix="/duels/lobbies", tags=["duels"])


def _http(exc: duel_service.DuelError) -> HTTPException:
    return HTTPException(status_code=exc.status_code, detail=exc.detail())


def _rate_limited(message: str) -> HTTPException:
    return HTTPException(
        status_code=status.HTTP_429_TOO_MANY_REQUESTS,
        detail={"code": "rate_limited", "message": message},
    )


@router.post(
    "",
    response_model=DuelRoomState,
    status_code=status.HTTP_201_CREATED,
    summary="Create a shared duel lobby (no song yet)",
)
def create_lobby(
    payload: DuelRoomCreate,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelRoomState:
    if not duel_lobby_create_limiter.allow(user.id):
        raise _rate_limited("Too many lobbies created, try again later")
    try:
        state = duel_lobby_service.create_room(
            db, user=user, name=payload.name, password=payload.password
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelRoomState.model_validate(state)


@router.get(
    "",
    response_model=DuelRoomListResponse,
    summary="Open duel lobbies (name, password flag, players, song, status)",
)
def list_lobbies(
    include_full: bool = Query(default=False),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelRoomListResponse:
    items, total = duel_lobby_service.list_rooms(db, include_full=include_full)
    return DuelRoomListResponse(
        items=items, total=total, include_full=include_full
    )


@router.get(
    "/{lobby_id}",
    response_model=DuelRoomState,
    summary="State of one lobby (members only)",
)
def get_lobby(
    lobby_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelRoomState:
    try:
        state = duel_lobby_service.get_room_state(
            db, lobby_id=lobby_id, user=user
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelRoomState.model_validate(state)


@router.post(
    "/{lobby_id}/join",
    response_model=DuelRoomState,
    summary="Join a lobby as the guest (password check, idempotent)",
)
def join_lobby(
    lobby_id: UUID,
    payload: DuelRoomJoin | None = None,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelRoomState:
    password = payload.password if payload is not None else None
    try:
        state = duel_lobby_service.join_room(
            db, lobby_id=lobby_id, user=user, password=password
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelRoomState.model_validate(state)


@router.post(
    "/{lobby_id}/leave",
    response_model=DuelRoomState,
    summary="Leave a lobby (the host closes the room)",
)
def leave_lobby(
    lobby_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelRoomState:
    try:
        state = duel_lobby_service.leave_room(
            db, lobby_id=lobby_id, user=user
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelRoomState.model_validate(state)


@router.post(
    "/{lobby_id}/song",
    response_model=DuelRoomState,
    summary="Choose the song for the match (host only, waiting lobby)",
)
def select_lobby_song(
    lobby_id: UUID,
    payload: DuelRoomSongSelect,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelRoomState:
    try:
        state = duel_lobby_service.select_song(
            db, lobby_id=lobby_id, user=user, song_id=payload.song_id
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelRoomState.model_validate(state)


@router.post(
    "/{lobby_id}/start",
    response_model=DuelRoomStartResponse,
    summary="Start the match (host only; both players and a song required)",
)
def start_lobby_match(
    lobby_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelRoomStartResponse:
    try:
        _state, payload = duel_lobby_service.start_match(
            db, lobby_id=lobby_id, user=user
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelRoomStartResponse.model_validate(payload)


@router.post(
    "/{lobby_id}/start/request",
    response_model=DuelRoomState,
    summary="Ask both players to confirm the start (host only, no download yet)",
)
def request_lobby_start(
    lobby_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelRoomState:
    try:
        state = duel_lobby_service.request_start(
            db, lobby_id=lobby_id, user=user
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelRoomState.model_validate(state)


@router.post(
    "/{lobby_id}/start/confirm",
    response_model=DuelRoomStartResponse,
    summary="Confirm the pending start (member); both confirmations create the match",
)
def confirm_lobby_start(
    lobby_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelRoomStartResponse:
    try:
        _state, payload = duel_lobby_service.confirm_start(
            db, lobby_id=lobby_id, user=user
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelRoomStartResponse.model_validate(payload)


@router.post(
    "/{lobby_id}/start/decline",
    response_model=DuelRoomState,
    summary="Decline the pending start (member); the room keeps working",
)
def decline_lobby_start(
    lobby_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelRoomState:
    try:
        state = duel_lobby_service.decline_start(
            db, lobby_id=lobby_id, user=user
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelRoomState.model_validate(state)


@router.get(
    "/{lobby_id}/chat",
    response_model=DuelChatHistoryResponse,
    summary="Lobby chat, incrementally with after_id (members only)",
)
def lobby_chat(
    lobby_id: UUID,
    after_id: int = Query(default=0, ge=0),
    limit: int = Query(default=200, ge=1, le=200),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelChatHistoryResponse:
    try:
        items, latest = duel_lobby_service.chat_history(
            db, lobby_id=lobby_id, user=user, after_id=after_id, limit=limit
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelChatHistoryResponse(items=items, latest_id=latest)


@router.post(
    "/{lobby_id}/chat",
    response_model=DuelChatMessageOut,
    status_code=status.HTTP_201_CREATED,
    summary="Send a chat message (members only, rate limited)",
)
def post_lobby_chat(
    lobby_id: UUID,
    payload: DuelRoomChatIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelChatMessageOut:
    if not duel_chat_limiter.allow((lobby_id, user.id)):
        raise _rate_limited("Too many chat messages")
    try:
        message = duel_lobby_service.post_message(
            db, lobby_id=lobby_id, user=user, message=payload.message
        )
    except duel_service.DuelError as exc:
        raise _http(exc) from exc
    return DuelChatMessageOut.model_validate(message)
