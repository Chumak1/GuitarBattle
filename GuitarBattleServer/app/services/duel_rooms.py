"""In-memory WebSocket rooms and rate limiting for Guitar Duels.

The API server runs as a single uvicorn worker (``docker/entrypoint.sh``), so
an in-process room registry is enough. The database remains the source of
truth: every message is built from a fresh authoritative snapshot, and a
reconnect re-reads the match.

Rooms only transport messages; they never hold HP, events or results.
"""
from __future__ import annotations

import asyncio
import logging
import threading
import time
from dataclasses import dataclass, field
from typing import Any
from uuid import UUID

logger = logging.getLogger(__name__)


@dataclass
class DuelRoom:
    match_id: UUID
    connections: dict[str, set[Any]] = field(default_factory=dict)

    def add(self, side: str, websocket: Any) -> bool:
        sockets = self.connections.setdefault(side, set())
        first = not sockets
        sockets.add(websocket)
        return first

    def remove(self, side: str, websocket: Any) -> bool:
        sockets = self.connections.get(side)
        if sockets is None:
            return False
        sockets.discard(websocket)
        if not sockets:
            self.connections.pop(side, None)
            return True
        return False

    def sides(self) -> set[str]:
        return set(self.connections.keys())


class DuelRoomManager:
    def __init__(self) -> None:
        self._rooms: dict[UUID, DuelRoom] = {}
        self._lock = threading.Lock()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._forfeit_tasks: dict[tuple[UUID, str], asyncio.Task[Any]] = {}

    def bind_loop(self) -> None:
        """Remember the event loop so sync REST handlers can broadcast."""
        try:
            self._loop = asyncio.get_running_loop()
        except RuntimeError:
            self._loop = None

    @property
    def loop(self) -> asyncio.AbstractEventLoop | None:
        return self._loop

    def connect(self, match_id: UUID, side: str, websocket: Any) -> bool:
        """Register a socket; returns ``True`` for the first side connection."""
        with self._lock:
            room = self._rooms.get(match_id)
            if room is None:
                room = DuelRoom(match_id=match_id)
                self._rooms[match_id] = room
            return room.add(side, websocket)

    def disconnect(self, match_id: UUID, side: str, websocket: Any) -> bool:
        """Unregister a socket; returns ``True`` when the side has no sockets."""
        with self._lock:
            room = self._rooms.get(match_id)
            if room is None:
                return True
            last = room.remove(side, websocket)
            if not room.connections:
                self._rooms.pop(match_id, None)
            return last

    def connected_sides(self, match_id: UUID) -> set[str]:
        with self._lock:
            room = self._rooms.get(match_id)
            return room.sides() if room is not None else set()

    def has_connection(self, match_id: UUID, side: str) -> bool:
        with self._lock:
            room = self._rooms.get(match_id)
            return bool(room and side in room.connections)

    async def broadcast(
        self, match_id: UUID, messages: list[dict[str, Any]]
    ) -> None:
        if not messages:
            return
        with self._lock:
            room = self._rooms.get(match_id)
            sockets = (
                [ws for group in room.connections.values() for ws in group]
                if room is not None
                else []
            )
        for websocket in list(sockets):
            for message in messages:
                try:
                    await websocket.send_json(message)
                except Exception:
                    # A dead socket is removed by its handler; never let one
                    # client break the broadcast for the other side.
                    logger.debug("Duel socket send failed", exc_info=True)
                    break

    def broadcast_threadsafe(
        self, match_id: UUID, messages: list[dict[str, Any]]
    ) -> None:
        """Broadcast from a sync endpoint running in the thread pool."""
        if not messages:
            return
        loop = self._loop
        if loop is None or loop.is_closed() or not loop.is_running():
            return
        asyncio.run_coroutine_threadsafe(self.broadcast(match_id, messages), loop)

    async def close_room(
        self, match_id: UUID, messages: list[dict[str, Any]] | None = None, code: int = 1000
    ) -> None:
        with self._lock:
            room = self._rooms.pop(match_id, None)
        if room is None:
            return
        for group in room.connections.values():
            for websocket in list(group):
                try:
                    if messages:
                        for message in messages:
                            await websocket.send_json(message)
                    await websocket.close(code=code)
                except Exception:
                    logger.debug("Duel socket close failed", exc_info=True)

    def set_forfeit_task(
        self, match_id: UUID, side: str, task: asyncio.Task[Any]
    ) -> None:
        key = (match_id, side)
        previous = self._forfeit_tasks.get(key)
        if previous is not None and not previous.done():
            previous.cancel()
        self._forfeit_tasks[key] = task

    def cancel_forfeit_task(self, match_id: UUID, side: str) -> None:
        task = self._forfeit_tasks.pop((match_id, side), None)
        if task is not None and not task.done():
            task.cancel()

    def drop_forfeit_task(self, match_id: UUID, side: str) -> None:
        self._forfeit_tasks.pop((match_id, side), None)


room_manager = DuelRoomManager()


# ---------------------------------------------------------------------------
# Token bucket
# ---------------------------------------------------------------------------
class RateLimiter:
    """Small process-local token bucket.

    Keys are ``(match_id, user_id)`` for notes and ``user_id`` for match
    creation. A multi-worker deployment would need Redis; the shipped compose
    stack runs one worker.
    """

    def __init__(self, *, rate_per_second: float, burst: int) -> None:
        self.rate = max(0.001, float(rate_per_second))
        self.burst = max(1, int(burst))
        self._tokens: dict[Any, tuple[float, float]] = {}
        self._lock = threading.Lock()
        self._last_cleanup = time.monotonic()

    def allow(self, key: Any, cost: float = 1.0) -> bool:
        now = time.monotonic()
        with self._lock:
            tokens, updated = self._tokens.get(key, (float(self.burst), now))
            tokens = min(
                float(self.burst), tokens + (now - updated) * self.rate
            )
            allowed = tokens >= cost
            if allowed:
                tokens -= cost
            self._tokens[key] = (tokens, now)
            self._cleanup(now)
            return allowed

    def reset(self, key: Any) -> None:
        with self._lock:
            self._tokens.pop(key, None)

    def _cleanup(self, now: float) -> None:
        # Drop idle buckets at most once a minute: an empty bucket refills in
        # ``burst / rate`` seconds, so older keys are indistinguishable from a
        # fresh one.
        if now - self._last_cleanup < 60.0:
            return
        self._last_cleanup = now
        idle_after = self.burst / self.rate + 60.0
        stale = [
            key for key, (_, updated) in self._tokens.items()
            if now - updated > idle_after
        ]
        for key in stale:
            self._tokens.pop(key, None)


#: 40 notes/second with a 120-note burst is far above a real chart while still
#: stopping a script that submits a whole song at once.
duel_event_limiter = RateLimiter(rate_per_second=40.0, burst=120)
#: Five match creations immediately, then roughly one every five seconds.
duel_create_limiter = RateLimiter(rate_per_second=0.2, burst=5)
#: WebSocket message budget per connection.
duel_ws_limiter = RateLimiter(rate_per_second=20.0, burst=60)
#: Lobby creation: a burst of six rooms, then one every two seconds.
duel_lobby_create_limiter = RateLimiter(rate_per_second=0.5, burst=6)
#: Lobby chat: two messages per second with a short burst, keyed by
#: ``(lobby_id, user_id)``. Comfortable for humans, hostile to spam scripts.
duel_chat_limiter = RateLimiter(rate_per_second=2.0, burst=12)
