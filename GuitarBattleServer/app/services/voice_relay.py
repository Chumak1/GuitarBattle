"""In-memory voice relay for duel lobbies.

The JUCE 9.0.2 client has no WebSocket/TLS socket, so lobby voice chat is a
REST polling relay: every client posts ~100 ms of 16 kHz mono signed 16-bit
little-endian PCM (base64 in a JSON body) and polls the chunks of the *other*
members of the same room.

Nothing here is persisted: a chunk lives in this process for at most
``CHUNK_TTL_SECONDS`` seconds and is dropped by the next POST/GET that touches
the room. No payload, token or raw byte is ever logged — only errors and the
fact that a limit was hit.

The same registry holds the *direct UDP* endpoints of the lobby members: each
client publishes ``(public_ip, udp_port)`` and reads the entry of the other
member. Those live ``ENDPOINT_TTL_SECONDS`` seconds and are refreshed by every
POST, so a stale entry can never send audio to a closed socket.

The production compose stack runs a single uvicorn worker
(``docker/entrypoint.sh``, no ``--workers``), so a process-local registry is
enough. A multi-worker deployment would need Redis.

Sequence numbers come from one process-wide counter instead of a per-lobby
one. They are still strictly increasing for every lobby, and dropping an idle
room buffer can never make a later chunk reuse an old ``seq``: a client that
polls with ``since=<old max>`` can never miss a chunk because the counter
restarted.
"""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Callable
from uuid import UUID

#: A chunk is readable by the opponent for three seconds. Two polls a second
#: leave plenty of margin for a 100 ms chunk.
CHUNK_TTL_SECONDS = 3.0
#: A published UDP endpoint is usable for thirty seconds. The client refreshes
#: it with every endpoint POST, so an entry only disappears together with the
#: client that owns it.
ENDPOINT_TTL_SECONDS = 30.0
#: Hard caps that bound the memory of one room (60 * 6.4 KiB = 384 KiB).
MAX_CHUNKS_PER_LOBBY = 60
MAX_CHUNKS_PER_USER = 20
#: 200 ms of 16 kHz mono signed 16-bit PCM.
MAX_CHUNK_BYTES = 6400
#: Soft flood guard: ``MAX_POSTS_PER_SECOND`` chunks inside ``RATE_WINDOW``
#: seconds. A real microphone sends about ten per second.
MAX_POSTS_PER_SECOND = 10
RATE_WINDOW_SECONDS = 1.0
#: One poll returns at most this many chunks (newest last).
MAX_CHUNKS_PER_POLL = 20


class VoiceRateLimited(Exception):
    """The caller posted more chunks than the per-user window allows."""


@dataclass
class VoiceChunk:
    """One buffered PCM chunk. ``data`` is raw bytes, never logged."""

    seq: int
    user_id: UUID
    username: str
    data: bytes
    created_at: float


@dataclass(frozen=True)
class VoiceEndpoint:
    """A member's public UDP endpoint. Frozen: it is handed to other threads."""

    ip: str
    port: int
    updated_at: float


@dataclass
class _LobbyBuffer:
    chunks: list[VoiceChunk] = field(default_factory=list)
    #: ``user_id -> monotonic timestamps`` of the recent posts (flood guard).
    posts: dict[UUID, list[float]] = field(default_factory=dict)
    #: ``user_id -> published UDP endpoint`` (direct voice, 30 s TTL).
    endpoints: dict[UUID, VoiceEndpoint] = field(default_factory=dict)


class VoiceRelay:
    """Thread-safe voice state (chunk buffers + UDP endpoints) keyed by lobby.

    FastAPI runs the sync endpoints in a thread pool, so every method takes the
    registry lock. The lock is only ever held for in-memory work: no I/O, no
    database, no payload formatting.
    """

    def __init__(self, *, clock: Callable[[], float] | None = None) -> None:
        self._lock = threading.Lock()
        self._buffers: dict[UUID, _LobbyBuffer] = {}
        self._next_seq = 1
        #: Monotonic source; tests replace it with a fake clock.
        self.clock: Callable[[], float] = clock or time.monotonic

    # -- writes ---------------------------------------------------------
    def push(
        self,
        *,
        lobby_id: UUID,
        user_id: UUID,
        username: str,
        data: bytes,
    ) -> int:
        """Buffer one chunk and return its lobby-wide ``seq``.

        Raises :class:`VoiceRateLimited` when the caller exceeds
        ``MAX_POSTS_PER_SECOND`` inside ``RATE_WINDOW_SECONDS``.
        """
        if len(data) > MAX_CHUNK_BYTES:
            # The schema rejects this first; the check keeps the registry safe
            # even when it is called directly.
            raise ValueError("voice chunk is too large")
        with self._lock:
            now = self.clock()
            buffer = self._buffers.setdefault(lobby_id, _LobbyBuffer())
            self._prune(buffer, now)
            if self._rate_limited(buffer, user_id, now):
                raise VoiceRateLimited()
            seq = self._next_seq
            self._next_seq += 1
            buffer.chunks.append(
                VoiceChunk(
                    seq=seq,
                    user_id=user_id,
                    username=username,
                    data=data,
                    created_at=now,
                )
            )
            self._trim(buffer, user_id)
            return seq

    def clear_user(self, lobby_id: UUID, user_id: UUID) -> None:
        """Drop the caller's chunks and UDP endpoint (mic off / leave).

        Idempotent.
        """
        with self._lock:
            buffer = self._buffers.get(lobby_id)
            if buffer is None:
                return
            buffer.chunks = [c for c in buffer.chunks if c.user_id != user_id]
            buffer.posts.pop(user_id, None)
            buffer.endpoints.pop(user_id, None)
            if not buffer.chunks and not buffer.posts and not buffer.endpoints:
                self._buffers.pop(lobby_id, None)

    def clear_lobby(self, lobby_id: UUID) -> None:
        """Forget a room entirely (the lobby was closed). Idempotent."""
        with self._lock:
            self._buffers.pop(lobby_id, None)

    def reset(self) -> None:
        """Drop every buffer; used by the tests."""
        with self._lock:
            self._buffers.clear()

    def publish_endpoint(
        self, *, lobby_id: UUID, user_id: UUID, ip: str, port: int
    ) -> VoiceEndpoint:
        """Store (or refresh) one member's public UDP endpoint.

        The IP is whatever the HTTP layer derived from the connection; this
        method never sees a client-supplied address. ``ip``/``port`` are not
        validated here — the request schema does that — but an obviously
        unusable port is still rejected to keep the registry safe.
        """
        if not 0 < port <= 65535:
            raise ValueError("voice endpoint port is out of range")
        with self._lock:
            now = self.clock()
            buffer = self._buffers.setdefault(lobby_id, _LobbyBuffer())
            self._prune(buffer, now)
            endpoint = VoiceEndpoint(ip=ip, port=port, updated_at=now)
            buffer.endpoints[user_id] = endpoint
            return endpoint

    # -- reads ----------------------------------------------------------
    def poll(
        self, *, lobby_id: UUID, user_id: UUID, since: int
    ) -> tuple[list[VoiceChunk], int]:
        """Chunks of the other members with ``seq > since``, newest last.

        Returns the chunks and ``next_since``: the highest ``seq`` currently
        stored for the room (the caller's own chunks included), or ``since``
        when nothing is stored. ``next_since`` never goes backwards.
        """
        with self._lock:
            buffer = self._buffers.get(lobby_id)
            if buffer is None:
                return [], max(0, since)
            self._prune(buffer, self.clock())
            if not buffer.chunks:
                # Nothing left to deliver: forget the room so an idle lobby
                # cannot leak an empty buffer forever.
                if not buffer.posts and not buffer.endpoints:
                    self._buffers.pop(lobby_id, None)
                return [], max(0, since)
            chunks = [
                chunk
                for chunk in buffer.chunks
                if chunk.seq > since and chunk.user_id != user_id
            ][:MAX_CHUNKS_PER_POLL]
            next_since = max(since, buffer.chunks[-1].seq)
            return chunks, next_since

    def endpoints(
        self,
        *,
        lobby_id: UUID,
        caller_id: UUID,
        peer_id: UUID | None = None,
    ) -> tuple[VoiceEndpoint | None, VoiceEndpoint | None]:
        """``(caller, peer)`` endpoints of a room, expired entries pruned.

        ``peer_id`` is the other member of the lobby; it is ``None`` while a
        room has no guest, in which case the peer endpoint is ``None`` too.
        Nothing is created by a read: an unknown lobby answers ``(None, None)``.
        """
        with self._lock:
            buffer = self._buffers.get(lobby_id)
            if buffer is None:
                return None, None
            self._prune(buffer, self.clock())
            caller = buffer.endpoints.get(caller_id)
            peer = (
                buffer.endpoints.get(peer_id) if peer_id is not None else None
            )
            if (
                not buffer.chunks
                and not buffer.posts
                and not buffer.endpoints
            ):
                # An idle room must not leak an empty buffer forever.
                self._buffers.pop(lobby_id, None)
            return caller, peer

    # -- internals (caller holds the lock) ------------------------------
    def _prune(self, buffer: _LobbyBuffer, now: float) -> None:
        """Drop expired chunks, stale rate-limit stamps and old endpoints."""
        if buffer.chunks:
            cutoff = now - CHUNK_TTL_SECONDS
            if buffer.chunks[0].created_at <= cutoff:
                buffer.chunks = [
                    chunk for chunk in buffer.chunks if chunk.created_at > cutoff
                ]
        for user_id, endpoint in list(buffer.endpoints.items()):
            if now - endpoint.updated_at >= ENDPOINT_TTL_SECONDS:
                del buffer.endpoints[user_id]
        for user_id, stamps in list(buffer.posts.items()):
            alive = [stamp for stamp in stamps if now - stamp < RATE_WINDOW_SECONDS]
            if alive:
                buffer.posts[user_id] = alive
            else:
                buffer.posts.pop(user_id, None)

    def _rate_limited(
        self, buffer: _LobbyBuffer, user_id: UUID, now: float
    ) -> bool:
        stamps = [
            stamp
            for stamp in buffer.posts.get(user_id, ())
            if now - stamp < RATE_WINDOW_SECONDS
        ]
        if len(stamps) >= MAX_POSTS_PER_SECOND:
            buffer.posts[user_id] = stamps
            return True
        stamps.append(now)
        buffer.posts[user_id] = stamps
        return False

    @staticmethod
    def _trim(buffer: _LobbyBuffer, user_id: UUID) -> None:
        """Keep the newest chunks of the room and of the posting user."""
        overflow = len(buffer.chunks) - MAX_CHUNKS_PER_LOBBY
        if overflow > 0:
            del buffer.chunks[:overflow]
        own = [
            index
            for index, chunk in enumerate(buffer.chunks)
            if chunk.user_id == user_id
        ]
        excess = len(own) - MAX_CHUNKS_PER_USER
        if excess > 0:
            drop = set(own[:excess])
            buffer.chunks = [
                chunk
                for index, chunk in enumerate(buffer.chunks)
                if index not in drop
            ]


#: One registry per process; see the module docstring for the worker caveat.
voice_relay = VoiceRelay()
