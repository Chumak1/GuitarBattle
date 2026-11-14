"""Lobby voice chat: an in-memory REST relay and a UDP endpoint exchange.

The JUCE 9.0.2 client cannot open a WebSocket/TLS socket, so voice is relayed
over ordinary JSON requests:

* ``POST   /voice/lobbies/{lobby_id}/chunks``  — upload one ~100 ms PCM chunk,
* ``GET    /voice/lobbies/{lobby_id}/chunks``  — poll the opponent's chunks,
* ``DELETE /voice/lobbies/{lobby_id}/chunks``  — drop the caller's own chunks,
* ``POST   /voice/lobbies/{lobby_id}/endpoint`` — publish the caller's UDP port
  and read the peer's UDP endpoint for direct (client-to-client) audio.

Only a member (host or guest) of an open duel lobby may call them. Membership
and the room lifecycle come from :mod:`app.services.duel_lobby_service`; this
module adds no lobby rules of its own.

Nothing is persisted and no payload, token or raw byte is logged:
:mod:`app.services.voice_relay` keeps the chunks in memory for three seconds
and the UDP endpoints for thirty.
"""
from __future__ import annotations

import base64
import ipaddress
from uuid import UUID

from fastapi import (
    APIRouter,
    Depends,
    HTTPException,
    Query,
    Request,
    Response,
    status,
)
from sqlalchemy.orm import Session

from app.api.v1.auth import get_current_user
from app.database import get_db
from app.models.duel import DuelLobby
from app.models.user import User
from app.schemas.voice import (
    VoiceChunkAck,
    VoiceChunkIn,
    VoiceChunkListResponse,
    VoiceChunkOut,
    VoiceEndpointIn,
    VoiceEndpointOut,
    VoicePeerEndpoint,
)
from app.services import duel_lobby_service, duel_service
from app.services.client_ip import FORWARDED_FOR_HEADER, client_ip
from app.services.voice_relay import VoiceRateLimited, voice_relay

router = APIRouter(prefix="/voice/lobbies", tags=["voice"])


def _member(db: Session, lobby_id: UUID, user: User) -> DuelLobby:
    """The open lobby the caller belongs to, or the matching HTTP error."""
    try:
        return duel_lobby_service.lobby_member(db, lobby_id=lobby_id, user=user)
    except duel_service.DuelError as exc:
        raise HTTPException(
            status_code=exc.status_code, detail=exc.detail()
        ) from exc


def _peer_user_id(lobby: DuelLobby, user: User) -> UUID | None:
    """The *other* member of the room, or ``None`` in a one-player lobby."""
    if user.id == lobby.host_user_id:
        return lobby.guest_user_id
    return lobby.host_user_id


def _caller_endpoint_ip(request: Request) -> str | None:
    """The public IP of the caller, derived from the connection only.

    ``client_ip`` already prefers ``X-Forwarded-For`` (Caddy appends the
    immediate peer, so its first entry is the real client) and drops private
    or malformed addresses. A direct development connection has no such
    header, and there the socket peer is the client itself: it is used as is,
    loopback included, so a local two-window test can still talk UDP.

    When the header *is* present but unusable the request is rejected instead
    of falling back, because behind the proxy the socket peer is Caddy and
    publishing ``127.0.0.1`` would point the opponent at their own machine.
    """
    public_ip = client_ip(request)
    if public_ip is not None:
        return public_ip
    if request.headers.get(FORWARDED_FOR_HEADER):
        return None
    peer = request.client.host if request.client is not None else None
    if not peer:
        return None
    try:
        return str(ipaddress.ip_address(peer))
    except ValueError:
        return None


@router.post(
    "/{lobby_id}/chunks",
    response_model=VoiceChunkAck,
    status_code=status.HTTP_200_OK,
    summary="Upload one voice chunk (lobby members only)",
)
def post_voice_chunk(
    lobby_id: UUID,
    payload: VoiceChunkIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> VoiceChunkAck:
    """Buffer a PCM chunk and return its lobby-wide sequence number."""
    lobby = _member(db, lobby_id, user)
    try:
        seq = voice_relay.push(
            lobby_id=lobby.id,
            user_id=user.id,
            username=user.username,
            data=payload.decoded(),
        )
    except VoiceRateLimited:
        # Soft flood guard: the client should keep ~10 chunks per second.
        raise HTTPException(
            status_code=status.HTTP_429_TOO_MANY_REQUESTS,
            detail="voice rate limit",
        ) from None
    return VoiceChunkAck(seq=seq)


@router.get(
    "/{lobby_id}/chunks",
    response_model=VoiceChunkListResponse,
    summary="Poll the voice chunks of the other lobby members",
)
def poll_voice_chunks(
    lobby_id: UUID,
    since: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> VoiceChunkListResponse:
    """Chunks of the opponent with ``seq > since``, oldest first, max 20."""
    lobby = _member(db, lobby_id, user)
    chunks, next_since = voice_relay.poll(
        lobby_id=lobby.id, user_id=user.id, since=since
    )
    return VoiceChunkListResponse(
        chunks=[
            VoiceChunkOut(
                seq=chunk.seq,
                user_id=chunk.user_id,
                username=chunk.username,
                data=base64.b64encode(chunk.data).decode("ascii"),
            )
            for chunk in chunks
        ],
        next_since=next_since,
    )


@router.delete(
    "/{lobby_id}/chunks",
    status_code=status.HTTP_204_NO_CONTENT,
    response_class=Response,
    summary="Drop the caller's buffered chunks (idempotent)",
)
def clear_voice_chunks(
    lobby_id: UUID,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> Response:
    """Clear the caller's own chunks; call it on mic off and on leave."""
    lobby = _member(db, lobby_id, user)
    voice_relay.clear_user(lobby.id, user.id)
    return Response(status_code=status.HTTP_204_NO_CONTENT)


@router.post(
    "/{lobby_id}/endpoint",
    response_model=VoiceEndpointOut,
    status_code=status.HTTP_200_OK,
    summary="Publish the caller's UDP voice endpoint and read the peer's",
)
def exchange_voice_endpoint(
    lobby_id: UUID,
    payload: VoiceEndpointIn,
    request: Request,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> VoiceEndpointOut:
    """Direct-UDP rendezvous: store my port, return the opponent's.

    The body carries only the local port of the client's UDP socket. The
    address is always the caller's own, as seen by the server — a
    client-supplied IP is ignored outright. Both members must call this
    before audio flows, so ``peer`` is ``null`` until the other one does.
    """
    lobby = _member(db, lobby_id, user)
    ip = _caller_endpoint_ip(request)
    if ip is None:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail={
                "code": "client_ip_unavailable",
                "message": "The public IP of this connection cannot be determined",
            },
        )
    voice_relay.publish_endpoint(
        lobby_id=lobby.id, user_id=user.id, ip=ip, port=payload.port
    )
    _, peer = voice_relay.endpoints(
        lobby_id=lobby.id,
        caller_id=user.id,
        peer_id=_peer_user_id(lobby, user),
    )
    return VoiceEndpointOut(
        ip=ip,
        port=payload.port,
        peer=(
            VoicePeerEndpoint(ip=peer.ip, port=peer.port)
            if peer is not None
            else None
        ),
    )
