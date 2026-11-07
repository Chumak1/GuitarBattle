"""Guitar Duels REST and WebSocket API (``/api/v1/duels``).

REST is the contract documented in ``docs/DUELS_API.md``. The WebSocket is a
thin transport over the same service: every message is answered with an
authoritative snapshot rebuilt from the database, never with client-supplied
HP or damage.
"""
from __future__ import annotations

import asyncio
import json
import logging
from typing import Any, Literal
from uuid import UUID

from fastapi import (
    APIRouter,
    Depends,
    HTTPException,
    Query,
    WebSocket,
    WebSocketDisconnect,
    status,
)
from fastapi.encoders import jsonable_encoder
from sqlalchemy.orm import Session

from app.api.v1.auth import _user_from_access_token, get_current_user
from app.database import SessionLocal, get_db
from app.models.user import User
from app.schemas.duels import (
    DuelCreate,
    DuelEventsRejected,
    DuelEventsRequest,
    DuelEventsResponse,
    DuelHistoryResponse,
    DuelLeaderboardResponse,
    DuelLobbyResponse,
    DuelMatchState,
    DuelMeResponse,
    DuelProfileResponse,
    DuelResultResponse,
)
from app.services import duel_service
from app.services.duel_rooms import (
    duel_create_limiter,
    duel_event_limiter,
    duel_ws_limiter,
    room_manager,
)

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/duels", tags=["duels"])

#: A disconnected side gets this long to reconnect before the match is
#: finalised as a leave. Long enough for a network hiccup, short enough that
#: the opponent does not wait forever.
RECONNECT_GRACE_SECONDS = 30
#: WebSocket receive timeout while the match is playing; drives the ticker that
#: resolves notes whose timing window has expired even without new input.
PLAYING_TICK_SECONDS = 2.0
WAITING_TICK_SECONDS = 10.0


# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------
def _duel_http(exc: duel_service.DuelError) -> HTTPException:
    return HTTPException(status_code=exc.status_code, detail=exc.detail())


def _rate_limited(code: str, message: str) -> HTTPException:
    return HTTPException(
        status_code=status.HTTP_429_TOO_MANY_REQUESTS,
        detail={"code": code, "message": message},
    )


def _annotate_connections(state: dict[str, Any]) -> dict[str, Any]:
    """Mark which participants currently have a live WebSocket in this process."""
    sides = room_manager.connected_sides(state["match_id"])
    for participant in state.get("participants", []):
        participant["connected"] = participant.get("side") in sides
    return state


def _public_state(state: dict[str, Any]) -> dict[str, Any]:
    """A snapshot safe to broadcast to both sides (no per-viewer ``your_side``)."""
    _annotate_connections(state)
    return {**state, "your_side": None}


def _broadcast(
    match_id: UUID,
    messages: list[dict[str, Any]],
    state: dict[str, Any] | None = None,
) -> None:
    payload: list[dict[str, Any]] = list(messages)
    if state is not None:
        payload.append({"type": "state", "match": _public_state(state)})
    room_manager.broadcast_threadsafe(match_id, jsonable_encoder(payload))


# ---------------------------------------------------------------------------
# REST: specific routes first
# ---------------------------------------------------------------------------
@router.post(
    "",
    response_model=DuelMatchState,
    status_code=status.HTTP_201_CREATED,
    summary="Create a bot duel or enqueue an online duel",
)
def create_duel(
    payload: DuelCreate,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelMatchState:
    if not duel_create_limiter.allow(user.id):
        raise _rate_limited(
            "rate_limited", "Too many duel matches created, try again later"
        )
    try:
        state, messages = duel_service.create_match(
            db,
            user=user,
            song_id=payload.song_id,
            mode=payload.mode,
            bot_level=payload.bot_level,
        )
    except duel_service.DuelError as exc:
        raise _duel_http(exc) from exc

    match_id = state["match_id"]
    _broadcast(match_id, messages, state)
    return DuelMatchState.model_validate(state)


@router.get(
    "/history",
    response_model=DuelHistoryResponse,
    summary="Duel history of the signed-in player",
)
def duel_history(
    limit: int = Query(default=20, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    mode: Literal["bot", "online"] | None = Query(default=None),
    result: Literal["win", "loss", "draw"] | None = Query(default=None),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelHistoryResponse:
    items, total = duel_service.history(
        db,
        user_id=user.id,
        limit=limit,
        offset=offset,
        mode=mode,
        result=result,
    )
    return DuelHistoryResponse(
        items=items, total=total, limit=limit, offset=offset
    )


@router.get(
    "/leaderboard",
    response_model=DuelLeaderboardResponse,
    summary="Competitive duel leaderboard (week/month/all)",
)
def duel_leaderboard(
    period: Literal["week", "month", "all"] = Query(default="all"),
    limit: int = Query(default=20, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
) -> DuelLeaderboardResponse:
    items, total = duel_service.leaderboard(
        db, period=period, limit=limit, offset=offset
    )
    return DuelLeaderboardResponse(
        period=period, items=items, total=total, limit=limit, offset=offset
    )


@router.get(
    "/lobby",
    response_model=DuelLobbyResponse,
    summary="Waiting online rooms available to join",
)
def duel_lobby(
    db: Session = Depends(get_db),
    _: User = Depends(get_current_user),
) -> DuelLobbyResponse:
    items, total = duel_service.lobby(db)
    return DuelLobbyResponse(items=items, total=total)


@router.get(
    "/me",
    response_model=DuelMeResponse,
    summary="Duel profile of the signed-in player plus the active match",
)
def duel_me(
    db: Session = Depends(get_db), user: User = Depends(get_current_user)
) -> DuelMeResponse:
    try:
        data = duel_service.profile(db, user_id=user.id)
    except duel_service.DuelError as exc:
        raise _duel_http(exc) from exc
    active = duel_service.active_match(db, user_id=user.id)
    return DuelMeResponse(**data, active_match=active)


@router.get(
    "/players/{user_id}",
    response_model=DuelProfileResponse,
    summary="Public duel profile of a player",
)
def duel_player(
    user_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(get_current_user),
) -> DuelProfileResponse:
    try:
        return DuelProfileResponse.model_validate(
            duel_service.profile(db, user_id=user_id)
        )
    except duel_service.DuelError as exc:
        raise _duel_http(exc) from exc


# ---------------------------------------------------------------------------
# REST: one match
# ---------------------------------------------------------------------------
@router.get(
    "/{match_id}",
    response_model=DuelMatchState,
    summary="Authoritative state of one duel match",
)
def get_duel(
    match_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelMatchState:
    try:
        return DuelMatchState.model_validate(
            _annotate_connections(
                duel_service.get_match(db, match_id=match_id, user=user)
            )
        )
    except duel_service.DuelError as exc:
        raise _duel_http(exc) from exc


@router.post(
    "/{match_id}/join",
    response_model=DuelMatchState,
    summary="Join a waiting online match",
)
def join_duel(
    match_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelMatchState:
    try:
        state, messages = duel_service.join_match(
            db, match_id=match_id, user=user
        )
    except duel_service.DuelError as exc:
        raise _duel_http(exc) from exc
    _broadcast(match_id, messages, state)
    return DuelMatchState.model_validate(state)


@router.post(
    "/{match_id}/ready",
    response_model=DuelMatchState,
    summary="Mark this side ready; the server starts the countdown",
)
def ready_duel(
    match_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelMatchState:
    try:
        state, messages = duel_service.ready_match(
            db, match_id=match_id, user=user
        )
    except duel_service.DuelError as exc:
        raise _duel_http(exc) from exc
    _broadcast(match_id, messages, state)
    return DuelMatchState.model_validate(state)


@router.post(
    "/{match_id}/events",
    response_model=DuelEventsResponse,
    summary="Submit a batch of chart notes",
)
def submit_duel_events(
    match_id: UUID,
    payload: DuelEventsRequest,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelEventsResponse:
    cost = max(1.0, len(payload.events) / 10.0)
    if not duel_event_limiter.allow((match_id, user.id), cost=cost):
        raise _rate_limited("rate_limited", "Too many note events")

    events = [event.model_dump() for event in payload.events]
    try:
        state, accepted, duplicates, rejected, exchanges, finished = (
            duel_service.submit_events(
                db, match_id=match_id, user=user, events=events
            )
        )
    except duel_service.DuelError as exc:
        raise _duel_http(exc) from exc

    messages = duel_service.exchange_messages_for_state(match_id, exchanges)
    if finished:
        messages.append(
            {
                "type": "finish",
                "match_id": match_id,
                "result": _public_state(state),
            }
        )
    _broadcast(match_id, messages, state)

    return DuelEventsResponse(
        match=DuelMatchState.model_validate(state),
        accepted=accepted,
        duplicates=duplicates,
        rejected=[DuelEventsRejected(**item) for item in rejected],
        exchanges=exchanges,
        finished=finished,
    )


@router.post(
    "/{match_id}/finish",
    response_model=DuelResultResponse,
    summary="Finish the chart (online waits for both sides)",
)
def finish_duel(
    match_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelResultResponse:
    try:
        state, already_finished, finished_now, exchanges = (
            duel_service.finish_match(db, match_id=match_id, user=user)
        )
    except duel_service.DuelError as exc:
        raise _duel_http(exc) from exc

    messages = duel_service.exchange_messages_for_state(match_id, exchanges)
    if finished_now:
        messages.append(
            {
                "type": "finish",
                "match_id": match_id,
                "result": _public_state(state),
            }
        )
    _broadcast(match_id, messages, state)

    return DuelResultResponse(
        match=DuelMatchState.model_validate(state),
        already_finished=already_finished,
        waiting_for_opponent=(
            not already_finished
            and not finished_now
            and state["status"] == "playing"
        ),
    )


@router.post(
    "/{match_id}/leave",
    response_model=DuelResultResponse,
    summary="Leave a match; the server finalises it by the duel rules",
)
def leave_duel(
    match_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelResultResponse:
    try:
        state, already_finished, finished_now, messages = (
            duel_service.leave_match(db, match_id=match_id, user=user)
        )
    except duel_service.DuelError as exc:
        raise _duel_http(exc) from exc

    _broadcast(match_id, messages, state)
    return DuelResultResponse(
        match=DuelMatchState.model_validate(state),
        already_finished=already_finished,
        waiting_for_opponent=False,
    )


@router.get(
    "/{match_id}/result",
    response_model=DuelMatchState,
    summary="Final result of a finished match",
)
def duel_result(
    match_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DuelMatchState:
    try:
        return DuelMatchState.model_validate(
            _annotate_connections(
                duel_service.match_result(db, match_id=match_id, user=user)
            )
        )
    except duel_service.DuelError as exc:
        raise _duel_http(exc) from exc


# ---------------------------------------------------------------------------
# WebSocket
# ---------------------------------------------------------------------------
def _run_with_user(
    function: Any, *, user_id: UUID, **kwargs: Any
) -> Any:
    """Run a service call in a fresh session (called in the thread pool)."""
    with SessionLocal() as db:
        user = db.get(User, user_id)
        if user is None:
            raise duel_service.DuelError(401, "unauthorized", "Unknown user")
        return function(db, user=user, **kwargs)


def _tick(match_id: UUID) -> Any:
    with SessionLocal() as db:
        return duel_service.tick_match(db, match_id=match_id)


def _load_state(match_id: UUID, user_id: UUID) -> dict[str, Any]:
    with SessionLocal() as db:
        user = db.get(User, user_id)
        if user is None:
            raise duel_service.DuelError(401, "unauthorized", "Unknown user")
        return duel_service.get_match(db, match_id=match_id, user=user)


async def _forfeit_after(
    match_id: UUID, side: str, user_id: UUID, delay: float
) -> None:
    """Finalise a match after a side has stayed disconnected."""
    try:
        await asyncio.sleep(delay)
        if room_manager.has_connection(match_id, side):
            return

        def work() -> Any:
            with SessionLocal() as db:
                user = db.get(User, user_id)
                if user is None:
                    return None
                try:
                    return duel_service.leave_match(
                        db, match_id=match_id, user=user
                    )
                except duel_service.DuelError:
                    return None

        result = await asyncio.to_thread(work)
        if not result:
            return
        state, _already, finished_now, messages = result
        payload = jsonable_encoder(
            list(messages)
            + [{"type": "state", "match": _public_state(state)}]
        )
        await room_manager.broadcast(match_id, payload)
        if finished_now:
            await room_manager.close_room(match_id)
    except asyncio.CancelledError:
        raise
    except Exception:  # pragma: no cover - defensive
        logger.exception("Duel forfeit task failed")
    finally:
        room_manager.drop_forfeit_task(match_id, side)


async def _send(websocket: WebSocket, payload: dict[str, Any]) -> bool:
    try:
        await websocket.send_json(jsonable_encoder(payload))
        return True
    except Exception:
        return False


async def _broadcast_ws(
    match_id: UUID,
    messages: list[dict[str, Any]],
    state: dict[str, Any] | None = None,
) -> None:
    payload = list(messages)
    if state is not None:
        payload.append({"type": "state", "match": _public_state(state)})
    await room_manager.broadcast(match_id, jsonable_encoder(payload))


@router.websocket("/{match_id}/ws")
async def duel_websocket(
    websocket: WebSocket,
    match_id: UUID,
    token: str | None = Query(default=None),
) -> None:
    """Realtime duel channel.

    The access token is passed as ``?token=`` because browsers cannot set an
    ``Authorization`` header on a WebSocket handshake. It is never logged.
    """
    room_manager.bind_loop()
    await websocket.accept()

    raw_token = (token or "").strip()
    with SessionLocal() as db:
        user = _user_from_access_token(raw_token, db) if raw_token else None
        if user is None:
            await _send(
                websocket,
                {
                    "type": "error",
                    "code": "unauthorized",
                    "message": "Invalid or missing access token",
                },
            )
            await websocket.close(code=4401)
            return
        user_id = user.id
        try:
            state = duel_service.get_match(db, match_id=match_id, user=user)
        except duel_service.DuelError as exc:
            await _send(
                websocket,
                {
                    "type": "error",
                    "code": exc.code,
                    "message": exc.message,
                },
            )
            await websocket.close(code=4404)
            return

    side = state["your_side"]
    if side is None:  # pragma: no cover - get_match already checks membership
        await websocket.close(code=4403)
        return

    first = room_manager.connect(match_id, side, websocket)
    if first:
        room_manager.cancel_forfeit_task(match_id, side)

    match_closed = False
    await _send(
        websocket,
        {
            "type": "connected",
            "match": _annotate_connections(state),
            "your_side": side,
            "reconnected": state["status"] == "playing",
        },
    )
    if first:
        await _broadcast_ws(
            match_id,
            [
                {
                    "type": "join",
                    "match_id": match_id,
                    "user_id": user_id,
                    "side": side,
                }
            ],
        )

    # A finished match has no live timeline; send the final state and close.
    if state["status"] in ("finished", "cancelled"):
        await _send(
            websocket,
            {
                "type": (
                    "finish" if state["status"] == "finished" else "cancelled"
                ),
                "match_id": match_id,
                "result": _annotate_connections(state),
            },
        )
        room_manager.disconnect(match_id, side, websocket)
        await websocket.close(code=1000)
        return

    last_ping = asyncio.get_running_loop().time()
    try:
        while True:
            timeout = (
                PLAYING_TICK_SECONDS
                if state.get("status") == "playing"
                else WAITING_TICK_SECONDS
            )
            try:
                raw = await asyncio.wait_for(
                    websocket.receive_text(), timeout=timeout
                )
            except asyncio.TimeoutError:
                now = asyncio.get_running_loop().time()
                if now - last_ping >= 20.0:
                    last_ping = now
                    if not await _send(websocket, {"type": "ping"}):
                        break
                if state.get("status") == "playing":
                    ticked = await asyncio.to_thread(_tick, match_id)
                    tick_state, exchanges, finished = ticked
                    if tick_state is not None:
                        state = tick_state
                        messages = duel_service.exchange_messages_for_state(
                            match_id, exchanges
                        )
                        if finished:
                            messages.append(
                                {
                                    "type": "finish",
                                    "match_id": match_id,
                                    "result": _public_state(state),
                                }
                            )
                        await _broadcast_ws(match_id, messages, state)
                        if finished:
                            match_closed = True
                            await room_manager.close_room(match_id)
                            break
                continue
            except WebSocketDisconnect:
                break

            if len(raw) > 64 * 1024:
                await _send(
                    websocket,
                    {
                        "type": "error",
                        "code": "message_too_large",
                        "message": "Message exceeds 64 KiB",
                    },
                )
                continue

            try:
                message = json.loads(raw)
            except (TypeError, ValueError):
                await _send(
                    websocket,
                    {
                        "type": "error",
                        "code": "invalid_json",
                        "message": "Expected a JSON object",
                    },
                )
                continue
            if not isinstance(message, dict):
                await _send(
                    websocket,
                    {
                        "type": "error",
                        "code": "invalid_message",
                        "message": "Expected a JSON object",
                    },
                )
                continue

            message_type = str(message.get("type") or "")
            if message_type == "ping":
                last_ping = asyncio.get_running_loop().time()
                await _send(websocket, {"type": "pong"})
                continue

            if message_type == "join":
                try:
                    fresh = await asyncio.to_thread(
                        _load_state, match_id, user_id
                    )
                except duel_service.DuelError as exc:
                    await _send(
                        websocket,
                        {
                            "type": "error",
                            "code": exc.code,
                            "message": exc.message,
                        },
                    )
                    continue
                state = fresh
                await _send(
                    websocket,
                    {
                        "type": "state",
                        "match": _annotate_connections(fresh),
                        "reconnected": fresh["status"] == "playing",
                    },
                )
                continue

            if not duel_ws_limiter.allow((match_id, user_id)):
                await _send(
                    websocket,
                    {
                        "type": "error",
                        "code": "rate_limited",
                        "message": "Too many messages",
                    },
                )
                continue

            if message_type == "ready":
                try:
                    state, messages = await asyncio.to_thread(
                        _run_with_user,
                        duel_service.ready_match,
                        user_id=user_id,
                        match_id=match_id,
                    )
                except duel_service.DuelError as exc:
                    await _send(
                        websocket,
                        {
                            "type": "error",
                            "code": exc.code,
                            "message": exc.message,
                        },
                    )
                    continue
                await _broadcast_ws(match_id, messages, state)
                continue

            if message_type in ("note", "events"):
                if message_type == "note":
                    events = [
                        {
                            "note_index": message.get("note_index"),
                            "quality": message.get("quality"),
                            "client_time_ms": message.get("client_time_ms"),
                        }
                    ]
                else:
                    events = message.get("events") or []
                if not isinstance(events, list) or not events:
                    await _send(
                        websocket,
                        {
                            "type": "error",
                            "code": "invalid_events",
                            "message": "events must be a non-empty list",
                        },
                    )
                    continue
                if len(events) > duel_service.MAX_EVENTS_PER_BATCH:
                    await _send(
                        websocket,
                        {
                            "type": "error",
                            "code": "batch_too_large",
                            "message": "At most 100 events per message",
                        },
                    )
                    continue
                try:
                    state, accepted, duplicates, rejected, exchanges, finished = (
                        await asyncio.to_thread(
                            _run_with_user,
                            duel_service.submit_events,
                            user_id=user_id,
                            match_id=match_id,
                            events=events,
                        )
                    )
                except duel_service.DuelError as exc:
                    await _send(
                        websocket,
                        {
                            "type": "error",
                            "code": exc.code,
                            "message": exc.message,
                        },
                    )
                    continue
                await _send(
                    websocket,
                    {
                        "type": "ack",
                        "accepted": accepted,
                        "duplicates": duplicates,
                        "rejected": rejected,
                    },
                )
                messages = duel_service.exchange_messages_for_state(
                    match_id, exchanges
                )
                if finished:
                    messages.append(
                        {
                            "type": "finish",
                            "match_id": match_id,
                            "result": _public_state(state),
                        }
                    )
                await _broadcast_ws(match_id, messages, state)
                if finished:
                    match_closed = True
                    await room_manager.close_room(match_id)
                    break
                continue

            if message_type == "finish":
                try:
                    state, already, finished_now, exchanges = await asyncio.to_thread(
                        _run_with_user,
                        duel_service.finish_match,
                        user_id=user_id,
                        match_id=match_id,
                    )
                except duel_service.DuelError as exc:
                    await _send(
                        websocket,
                        {
                            "type": "error",
                            "code": exc.code,
                            "message": exc.message,
                        },
                    )
                    continue
                messages = duel_service.exchange_messages_for_state(
                    match_id, exchanges
                )
                if finished_now:
                    messages.append(
                        {
                            "type": "finish",
                            "match_id": match_id,
                            "result": _public_state(state),
                        }
                    )
                await _broadcast_ws(match_id, messages, state)
                if finished_now:
                    match_closed = True
                    await room_manager.close_room(match_id)
                    break
                continue

            if message_type == "leave":
                try:
                    state, already, finished_now, messages = await asyncio.to_thread(
                        _run_with_user,
                        duel_service.leave_match,
                        user_id=user_id,
                        match_id=match_id,
                    )
                except duel_service.DuelError as exc:
                    await _send(
                        websocket,
                        {
                            "type": "error",
                            "code": exc.code,
                            "message": exc.message,
                        },
                    )
                    continue
                await _broadcast_ws(match_id, messages, state)
                if finished_now:
                    match_closed = True
                    await room_manager.close_room(match_id)
                    break
                continue

            await _send(
                websocket,
                {
                    "type": "error",
                    "code": "unknown_message",
                    "message": f"Unknown message type: {message_type!r}",
                },
            )
    except WebSocketDisconnect:
        pass
    except Exception:  # pragma: no cover - defensive
        logger.exception("Duel WebSocket failed")
    finally:
        last = room_manager.disconnect(match_id, side, websocket)
        if last and not match_closed:
            task = asyncio.create_task(
                _forfeit_after(
                    match_id, side, user_id, RECONNECT_GRACE_SECONDS
                )
            )
            room_manager.set_forfeit_task(match_id, side, task)
