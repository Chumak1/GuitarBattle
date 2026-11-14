"""Shared duel lobbies: rooms with chat, a host-chosen song and a start button.

A lobby is deliberately *not* a ``DuelMatch``: the product flow is

1. the host creates a room (name + optional password) — no song yet,
2. a guest joins, both chat and the host picks a published song,
3. the host requests a start, both players confirm (the match and the download
   only start after both agreed; ``/start`` remains as the legacy one-shot
   path), and the linked ``duel_matches`` row is created ``waiting`` — the
   shared countdown starts with ``POST /duels/{id}/ready`` once both downloads
   are done,
4. after the match the two players either replay the same song (*rematch*) or
   go back to the same room (*return-to-lobby*) — the chat and the membership
   are preserved because the lobby row itself is never recreated.

All mutations lock the ``duel_lobbies`` row first, the same pattern
:mod:`app.services.duel_service` uses for matches. Errors are
:class:`app.services.duel_service.DuelError` so REST handlers share one error
envelope: ``{"detail": {"code": ..., "message": ...}}``.
"""
from __future__ import annotations

import logging
import re
import secrets
from datetime import datetime, timedelta, timezone
from typing import Any
from uuid import UUID

from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app.models.duel import (
    DuelLobby,
    DuelLobbyMessage,
    DuelMatch,
    DuelParticipant,
)
from app.models.song import Song
from app.models.user import User
from app.services import duel_rules, duel_service
from app.services.passwords import hash_password, verify_password
from app.services.voice_relay import voice_relay

logger = logging.getLogger(__name__)

#: Longest chat history returned by one poll. Larger histories are reachable
#: with ``?after_id=`` in increments of this size.
MAX_CHAT_HISTORY = 200
#: Lobby name limits, mirrored by Pydantic and the DB CHECK constraint.
MAX_NAME_LENGTH = 40
#: Chat message limits, mirrored by Pydantic and the DB CHECK constraint.
MAX_MESSAGE_LENGTH = 500
#: Room list page cap.
MAX_ROOMS = 50

_HTML_TAG = re.compile(r"<[^>]*>")
_CONTROL_CHARS = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")
_WHITESPACE = re.compile(r"\s+")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _now() -> datetime:
    return datetime.now(timezone.utc)


def _aware(value: datetime | None) -> datetime | None:
    if value is None:
        return None
    return value if value.tzinfo is not None else value.replace(tzinfo=timezone.utc)


def sanitize_message_text(raw: str) -> str:
    """Plain-text chat: no tags, no control characters, single spaces.

    The client renders the stored string as text, so removing markup here is
    the only place that can guarantee "no HTML" for both players.
    """
    text = _CONTROL_CHARS.sub("", str(raw))
    text = _HTML_TAG.sub("", text)
    text = text.replace("<", "").replace(">", "")
    text = _WHITESPACE.sub(" ", text).strip()
    return text


def _locked_room(db: Session, lobby_id: UUID) -> DuelLobby:
    lobby = db.scalar(
        select(DuelLobby)
        .where(DuelLobby.id == lobby_id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    if lobby is None:
        raise duel_service.DuelError(
            404, "lobby_not_found", "Duel lobby not found"
        )
    return lobby


def _room(db: Session, lobby_id: UUID) -> DuelLobby:
    lobby = db.get(DuelLobby, lobby_id)
    if lobby is None:
        raise duel_service.DuelError(
            404, "lobby_not_found", "Duel lobby not found"
        )
    return lobby


def _require_member(lobby: DuelLobby, user: User) -> None:
    if user.id not in (lobby.host_user_id, lobby.guest_user_id):
        raise duel_service.DuelError(
            403, "not_participant", "You are not a member of this lobby"
        )


def _require_host(lobby: DuelLobby, user: User) -> None:
    _require_member(lobby, user)
    if user.id != lobby.host_user_id:
        raise duel_service.DuelError(
            403, "not_host", "Only the host can do that"
        )


def _published_song(db: Session, song_id: UUID) -> Song:
    song = db.scalar(
        select(Song).where(Song.id == song_id, Song.archived_at.is_(None))
    )
    if song is None:
        raise duel_service.DuelError(404, "song_not_found", "Song not found")
    if not song.is_published:
        raise duel_service.DuelError(
            422, "song_not_published", "Song is not published"
        )
    return song


def _latest_match(db: Session, lobby_id: UUID) -> DuelMatch | None:
    return db.scalar(
        select(DuelMatch)
        .where(DuelMatch.lobby_id == lobby_id)
        .order_by(DuelMatch.created_at.desc(), DuelMatch.id.desc())
        .limit(1)
    )


def _players_payload(db: Session, lobby: DuelLobby) -> list[dict[str, Any]]:
    ids = [
        value
        for value in (lobby.host_user_id, lobby.guest_user_id)
        if value is not None
    ]
    users = {
        user.id: user
        for user in db.scalars(select(User).where(User.id.in_(ids))).all()
    }
    players: list[dict[str, Any]] = []
    for user_id, is_host in (
        (lobby.host_user_id, True),
        (lobby.guest_user_id, False),
    ):
        if user_id is None:
            continue
        user = users.get(user_id)
        if user is None:  # pragma: no cover - FK guarantees the row
            continue
        players.append(
            {
                "user_id": user.id,
                "username": user.username,
                "display_name": user.display_name,
                "is_host": is_host,
            }
        )
    return players


def _song_payload(db: Session, song_id: UUID | None) -> dict[str, Any] | None:
    if song_id is None:
        return None
    song = db.get(Song, song_id)
    return duel_service.song_info(song) if song is not None else None


def lobby_state(db: Session, lobby: DuelLobby) -> dict[str, Any]:
    """Full snapshot both members see: players, song, status and match id."""
    latest = _latest_match(db, lobby.id)
    return {
        "lobby_id": lobby.id,
        "name": lobby.name,
        "has_password": lobby.password_hash is not None,
        "status": lobby.status,
        "host_user_id": lobby.host_user_id,
        "guest_user_id": lobby.guest_user_id,
        "song": _song_payload(db, lobby.song_id),
        "match_id": latest.id if latest is not None else None,
        "players": _players_payload(db, lobby),
        "start": _start_payload(lobby),
        "created_at": lobby.created_at,
        "updated_at": lobby.updated_at,
    }


def _start_payload(lobby: DuelLobby) -> dict[str, Any] | None:
    """Pending start request, or ``None`` when nobody asked to start."""
    if lobby.start_requested_by is None:
        return None
    return {
        "requested_by": lobby.start_requested_by,
        "host_confirmed": bool(lobby.start_host_confirmed),
        "guest_confirmed": bool(lobby.start_guest_confirmed),
    }


def _clear_start_request(lobby: DuelLobby) -> None:
    """Drop the pending confirmation round (decline, cancel, room reset)."""
    lobby.start_requested_by = None
    lobby.start_host_confirmed = False
    lobby.start_guest_confirmed = False


def room_summary(db: Session, lobby: DuelLobby) -> dict[str, Any]:
    latest = _latest_match(db, lobby.id)
    return {
        "lobby_id": lobby.id,
        "name": lobby.name,
        "has_password": lobby.password_hash is not None,
        "status": lobby.status,
        "host_user_id": lobby.host_user_id,
        "song": _song_payload(db, lobby.song_id),
        "match_id": latest.id if latest is not None else None,
        "players": _players_payload(db, lobby),
        "created_at": lobby.created_at,
    }


def get_room_state(
    db: Session, *, lobby_id: UUID, user: User
) -> dict[str, Any]:
    """Read-only snapshot for a member; non-members get a 403."""
    lobby = _room(db, lobby_id)
    _require_member(lobby, user)
    return lobby_state(db, lobby)


def lobby_member(db: Session, *, lobby_id: UUID, user: User) -> DuelLobby:
    """The open lobby the caller belongs to.

    Used by the side channels of a room (the voice relay) so they never repeat
    the lobby rules: an unknown or ``closed`` room is a 404 for everybody, and
    a signed-in player who is neither host nor guest gets a 403.
    """
    lobby = _room(db, lobby_id)
    if lobby.status == "closed":
        raise duel_service.DuelError(
            404, "lobby_not_found", "Duel lobby not found"
        )
    _require_member(lobby, user)
    return lobby


# ---------------------------------------------------------------------------
# List / create / join / leave
# ---------------------------------------------------------------------------
def list_rooms(
    db: Session, *, include_full: bool = False
) -> tuple[list[dict[str, Any]], int]:
    """Open rooms, newest first.

    ``closed`` rooms are never listed. A room with a guest is only listed when
    ``include_full=true``: it is still useful to see it (e.g. to re-enter your
    own room), but it cannot be joined.
    """
    filters = [DuelLobby.status.in_(("waiting", "playing"))]
    if not include_full:
        filters.append(DuelLobby.guest_user_id.is_(None))

    rows = db.scalars(
        select(DuelLobby)
        .where(*filters)
        .order_by(DuelLobby.created_at.desc(), DuelLobby.id.desc())
        .limit(MAX_ROOMS)
    ).all()
    total = int(
        db.scalar(
            select(func.count(DuelLobby.id)).where(*filters)
        )
        or 0
    )
    return [room_summary(db, row) for row in rows], total


def create_room(
    db: Session, *, user: User, name: str, password: str | None
) -> dict[str, Any]:
    clean_name = str(name or "").strip()
    if not clean_name or len(clean_name) > MAX_NAME_LENGTH:
        raise duel_service.DuelError(
            422,
            "invalid_name",
            f"Lobby name must be 1..{MAX_NAME_LENGTH} characters",
        )
    clean_password = (password or "").strip() or None
    now = _now()
    lobby = DuelLobby(
        name=clean_name,
        password_hash=(
            hash_password(clean_password) if clean_password is not None else None
        ),
        host_user_id=user.id,
        guest_user_id=None,
        song_id=None,
        status="waiting",
        created_at=now,
        updated_at=now,
    )
    db.add(lobby)
    db.flush()
    state = lobby_state(db, lobby)
    db.commit()
    return state


def join_room(
    db: Session, *, lobby_id: UUID, user: User, password: str | None
) -> dict[str, Any]:
    lobby = _locked_room(db, lobby_id)

    if lobby.status == "closed":
        raise duel_service.DuelError(409, "lobby_closed", "Lobby is closed")

    # Duplicate join is idempotent: reconnecting clients must never fail.
    if user.id in (lobby.host_user_id, lobby.guest_user_id):
        state = lobby_state(db, lobby)
        db.commit()
        return state

    if lobby.guest_user_id is not None:
        raise duel_service.DuelError(409, "lobby_full", "Lobby already has two players")

    if lobby.status != "waiting":
        raise duel_service.DuelError(
            409, "lobby_not_waiting", "Lobby is not waiting for a player"
        )

    if lobby.password_hash is not None:
        if not password:
            raise duel_service.DuelError(
                403, "password_required", "This lobby requires a password"
            )
        if not verify_password(password, lobby.password_hash):
            raise duel_service.DuelError(
                403, "invalid_password", "Wrong lobby password"
            )

    lobby.guest_user_id = user.id
    lobby.updated_at = _now()
    db.flush()
    state = lobby_state(db, lobby)
    db.commit()
    return state


def leave_room(
    db: Session, *, lobby_id: UUID, user: User
) -> dict[str, Any]:
    lobby = _locked_room(db, lobby_id)
    _require_member(lobby, user)

    now = _now()
    # The start handshake needs both players in the room: any leave drops it.
    _clear_start_request(lobby)
    if user.id == lobby.host_user_id:
        # The host owns the room: closing it frees the guest while keeping the
        # chat history readable for both members.
        if lobby.status != "closed":
            lobby.status = "closed"
            lobby.updated_at = now
        # A closed room has no voice channel: drop its in-memory chunks now
        # instead of waiting for the TTL (app/services/voice_relay.py).
        voice_relay.clear_lobby(lobby.id)
    else:
        lobby.guest_user_id = None
        lobby.updated_at = now
        # The guest is no longer a member, so their buffered audio must not be
        # delivered to anyone after this point.
        voice_relay.clear_user(lobby.id, user.id)

    db.flush()
    state = lobby_state(db, lobby)
    db.commit()
    return state


# ---------------------------------------------------------------------------
# Song picker
# ---------------------------------------------------------------------------
def select_song(
    db: Session, *, lobby_id: UUID, user: User, song_id: UUID
) -> dict[str, Any]:
    lobby = _locked_room(db, lobby_id)
    _require_host(lobby, user)

    if lobby.status != "waiting":
        raise duel_service.DuelError(
            409, "lobby_not_waiting", "The song can only be changed while waiting"
        )

    song = _published_song(db, song_id)
    # A confirmation round belongs to the song the players saw; picking another
    # song restarts it.
    if lobby.song_id != song.id:
        _clear_start_request(lobby)
    lobby.song_id = song.id
    lobby.updated_at = _now()
    db.flush()
    state = lobby_state(db, lobby)
    db.commit()
    return state


# ---------------------------------------------------------------------------
# Match creation shared by /start and /rematch
# ---------------------------------------------------------------------------
def _create_lobby_match(
    db: Session, *, lobby: DuelLobby, song: Song, now: datetime
) -> DuelMatch:
    """Create the linked match of a room.

    The match is created ``waiting`` with no ``started_at`` and no ready flags:
    both clients download the song *after* confirming the start, and the
    shared countdown only begins when each side reports ``ready`` (which also
    guarantees the chart and stems are already local). Callers that keep the
    legacy "start immediately" behaviour (``/start`` and ``rematch``) call
    :func:`_arm_lobby_match` right after.
    """
    assert lobby.guest_user_id is not None
    note_count, note_times = duel_service.load_chart(db, song)
    match = DuelMatch(
        song_id=song.id,
        lobby_id=lobby.id,
        mode="online",
        bot_level=None,
        status="waiting",
        rules_version=duel_rules.RULES_VERSION,
        seed=secrets.randbits(32),
        note_count=note_count,
        note_times_ms=note_times,
        next_note_index=0,
        started_at=None,
        created_at=now,
        updated_at=now,
    )
    db.add(match)
    db.flush()

    for user_id, side in (
        (lobby.host_user_id, "player"),
        (lobby.guest_user_id, "opponent"),
    ):
        db.add(
            DuelParticipant(
                match_id=match.id,
                user_id=user_id,
                side=side,
                is_bot=False,
                ready_at=None,
                rating_before=duel_service.current_rating(db, user_id),
            )
        )
    db.flush()
    return match


def _arm_lobby_match(db: Session, match: DuelMatch, *, now: datetime) -> None:
    """Legacy/rematch behaviour: both sides ready, countdown starts at once."""
    match.status = "playing"
    match.started_at = now + timedelta(milliseconds=duel_service.COUNTDOWN_MS)
    match.next_note_index = 0
    for participant in db.scalars(
        select(DuelParticipant).where(DuelParticipant.match_id == match.id)
    ).all():
        if participant.ready_at is None:
            participant.ready_at = now
    db.flush()


def start_match(
    db: Session, *, lobby_id: UUID, user: User
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Legacy one-shot start (the response shape old clients rely on).

    New clients use ``request_start`` + ``confirm_start`` instead, which defer
    the download to after the confirmation.
    """
    lobby = _locked_room(db, lobby_id)
    _require_host(lobby, user)

    if lobby.status != "waiting":
        raise duel_service.DuelError(
            409, "lobby_not_waiting", "Lobby is not waiting to start"
        )
    if lobby.guest_user_id is None:
        raise duel_service.DuelError(
            409, "opponent_missing", "Waiting for a second player"
        )
    if lobby.song_id is None:
        raise duel_service.DuelError(
            409, "song_not_selected", "Choose a song before starting"
        )

    song = _published_song(db, lobby.song_id)
    now = _now()
    match = _create_lobby_match(db, lobby=lobby, song=song, now=now)
    _arm_lobby_match(db, match, now=now)
    lobby.status = "playing"
    _clear_start_request(lobby)
    lobby.updated_at = now
    db.flush()

    state = lobby_state(db, lobby)
    match_id = match.id
    db.commit()
    return state, {
        "match_id": match_id,
        "lobby": state,
    }


# ---------------------------------------------------------------------------
# Two-step start: request -> confirm (both) -> match created (still waiting)
# ---------------------------------------------------------------------------
def request_start(
    db: Session, *, lobby_id: UUID, user: User
) -> dict[str, Any]:
    """Host asks to start; both players then confirm before any download."""
    lobby = _locked_room(db, lobby_id)
    _require_host(lobby, user)

    if lobby.status != "waiting":
        raise duel_service.DuelError(
            409, "lobby_not_waiting", "Lobby is not waiting to start"
        )
    if lobby.guest_user_id is None:
        raise duel_service.DuelError(
            409, "opponent_missing", "Waiting for a second player"
        )
    if lobby.song_id is None:
        raise duel_service.DuelError(
            409, "song_not_selected", "Choose a song before starting"
        )

    now = _now()

    # A retry of the same request must not throw away an opponent's
    # confirmation; only a fresh round starts from zero.
    if lobby.start_requested_by is None:
        lobby.start_requested_by = user.id
        lobby.start_host_confirmed = False
        lobby.start_guest_confirmed = False
    lobby.updated_at = now
    db.flush()
    state = lobby_state(db, lobby)
    db.commit()
    return state


def confirm_start(
    db: Session, *, lobby_id: UUID, user: User
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Confirm the pending start request; both confirmations create the match."""
    lobby = _locked_room(db, lobby_id)
    _require_member(lobby, user)

    if lobby.status != "waiting":
        raise duel_service.DuelError(
            409, "lobby_not_waiting", "Lobby is not waiting to start"
        )
    if lobby.start_requested_by is None:
        raise duel_service.DuelError(
            409, "start_not_requested", "The host has not requested a start"
        )
    if lobby.guest_user_id is None:
        raise duel_service.DuelError(
            409, "opponent_missing", "Waiting for a second player"
        )
    if lobby.song_id is None:
        raise duel_service.DuelError(
            409, "song_not_selected", "Choose a song before starting"
        )

    now = _now()

    if user.id == lobby.host_user_id:
        lobby.start_host_confirmed = True
    else:
        lobby.start_guest_confirmed = True
    lobby.updated_at = now
    db.flush()

    if not (lobby.start_host_confirmed and lobby.start_guest_confirmed):
        state = lobby_state(db, lobby)
        db.commit()
        return state, {"match_id": None, "lobby": state}

    # Both sides agreed: create the match. It stays `waiting` (no started_at,
    # no ready flags) so the countdown only runs after both downloads finish
    # and each client posts /ready.
    song = _published_song(db, lobby.song_id)
    match = _create_lobby_match(db, lobby=lobby, song=song, now=now)
    lobby.status = "playing"
    _clear_start_request(lobby)
    lobby.updated_at = now
    db.flush()

    state = lobby_state(db, lobby)
    match_id = match.id
    db.commit()
    return state, {"match_id": match_id, "lobby": state}


def decline_start(
    db: Session, *, lobby_id: UUID, user: User
) -> dict[str, Any]:
    """Any member cancels the pending request; the room keeps working."""
    lobby = _locked_room(db, lobby_id)
    _require_member(lobby, user)

    if lobby.start_requested_by is not None:
        _clear_start_request(lobby)
        lobby.updated_at = _now()
        db.flush()

    state = lobby_state(db, lobby)
    db.commit()
    return state


# ---------------------------------------------------------------------------
# Chat
# ---------------------------------------------------------------------------
def _message_payload(row: DuelLobbyMessage, user: User | None) -> dict[str, Any]:
    return {
        "id": int(row.id),
        "user_id": row.user_id,
        "username": user.username if user is not None else "",
        "display_name": user.display_name if user is not None else None,
        "text": row.text,
        "created_at": row.created_at,
    }


def chat_history(
    db: Session,
    *,
    lobby_id: UUID,
    user: User,
    after_id: int = 0,
    limit: int = MAX_CHAT_HISTORY,
) -> tuple[list[dict[str, Any]], int]:
    lobby = _room(db, lobby_id)
    _require_member(lobby, user)

    limit = max(1, min(MAX_CHAT_HISTORY, int(limit)))
    if after_id > 0:
        rows = db.scalars(
            select(DuelLobbyMessage)
            .where(
                DuelLobbyMessage.lobby_id == lobby.id,
                DuelLobbyMessage.id > after_id,
            )
            .order_by(DuelLobbyMessage.id.asc())
            .limit(limit)
        ).all()
    else:
        # First poll: the last page, returned in ascending order.
        newest = db.scalars(
            select(DuelLobbyMessage)
            .where(DuelLobbyMessage.lobby_id == lobby.id)
            .order_by(DuelLobbyMessage.id.desc())
            .limit(limit)
        ).all()
        rows = list(reversed(newest))

    user_ids = {row.user_id for row in rows}
    users = (
        {
            row.id: row
            for row in db.scalars(select(User).where(User.id.in_(user_ids))).all()
        }
        if user_ids
        else {}
    )
    items = [_message_payload(row, users.get(row.user_id)) for row in rows]
    latest = int(rows[-1].id) if rows else int(after_id)
    return items, latest


def post_message(
    db: Session, *, lobby_id: UUID, user: User, message: str
) -> dict[str, Any]:
    lobby = _room(db, lobby_id)
    _require_member(lobby, user)

    if lobby.status == "closed":
        raise duel_service.DuelError(409, "lobby_closed", "Lobby is closed")

    text = sanitize_message_text(message)
    if not text:
        raise duel_service.DuelError(
            422, "empty_message", "Message must contain visible characters"
        )
    if len(text) > MAX_MESSAGE_LENGTH:
        raise duel_service.DuelError(
            422,
            "message_too_long",
            f"Message must be at most {MAX_MESSAGE_LENGTH} characters",
        )

    row = DuelLobbyMessage(
        lobby_id=lobby.id,
        user_id=user.id,
        text=text,
        created_at=_now(),
    )
    db.add(row)
    db.flush()
    payload = _message_payload(row, user)
    db.commit()
    return payload


# ---------------------------------------------------------------------------
# After the match: rematch / return to the same lobby
# ---------------------------------------------------------------------------
def _match_for_update(db: Session, match_id: UUID) -> DuelMatch:
    match = db.scalar(
        select(DuelMatch)
        .where(DuelMatch.id == match_id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    if match is None:
        raise duel_service.DuelError(404, "match_not_found", "Duel match not found")
    return match


def _require_match_participant(db: Session, match: DuelMatch, user: User) -> None:
    if duel_service._participant(db, match.id, user.id) is None:
        raise duel_service.DuelError(
            403, "not_participant", "You are not a participant"
        )


def rematch(
    db: Session, *, match_id: UUID, user: User
) -> dict[str, Any]:
    """Create a fresh match for the same two players and the room's song.

    The previous match row is never touched: its result, XP, rating and event
    history stay exactly as they were stored. The song comes from the room when
    it has one (the host may have picked another song after ``return-to-lobby``)
    and from the finished match otherwise.
    """
    match = _match_for_update(db, match_id)
    _require_match_participant(db, match, user)

    if match.lobby_id is None:
        raise duel_service.DuelError(
            409, "lobby_not_found", "This match was not started from a lobby"
        )

    lobby = _locked_room(db, match.lobby_id)
    _require_member(lobby, user)
    if lobby.status == "closed":
        raise duel_service.DuelError(409, "lobby_closed", "Lobby is closed")
    if lobby.guest_user_id is None:
        raise duel_service.DuelError(
            409, "opponent_missing", "The second player has left the lobby"
        )

    if match.status != "finished":
        raise duel_service.DuelError(
            409, "match_not_finished", "The match is not finished yet"
        )

    song = _published_song(db, match.song_id)
    # The room may have selected another song while this match was running (the
    # host changed it after return-to-lobby). A rematch must play what the room
    # currently shows, never the previous match's song.
    if lobby.song_id is not None:
        song = _published_song(db, lobby.song_id)
    latest = _latest_match(db, lobby.id)
    if latest is not None and latest.id != match.id:
        # A second click (or a retry) must not create a second match.
        if latest.status in ("waiting", "playing"):
            db.commit()
            return {
                "match_id": latest.id,
                "previous_match_id": match.id,
                "lobby_id": lobby.id,
                "song": duel_service.song_info(song),
                "already_created": True,
            }

    now = _now()
    new_match = _create_lobby_match(db, lobby=lobby, song=song, now=now)
    # Rematch keeps the legacy behaviour (both sides ready, countdown at once):
    # the song is already cached from the match that just ended.
    _arm_lobby_match(db, new_match, now=now)
    lobby.status = "playing"
    _clear_start_request(lobby)
    lobby.updated_at = now
    db.flush()
    payload = {
        "match_id": new_match.id,
        "previous_match_id": match.id,
        "lobby_id": lobby.id,
        "song": duel_service.song_info(song),
        "already_created": False,
    }
    db.commit()
    return payload


def return_to_lobby(
    db: Session, *, match_id: UUID, user: User
) -> dict[str, Any]:
    """Put both players back into the same room, chat and song kept."""
    match = _match_for_update(db, match_id)
    _require_match_participant(db, match, user)

    if match.lobby_id is None:
        raise duel_service.DuelError(
            409, "lobby_not_found", "This match was not started from a lobby"
        )
    if match.status not in ("finished", "cancelled"):
        raise duel_service.DuelError(
            409, "match_not_finished", "Finish the match before returning to the lobby"
        )

    lobby = _locked_room(db, match.lobby_id)
    _require_member(lobby, user)
    if lobby.status == "closed":
        raise duel_service.DuelError(409, "lobby_closed", "Lobby is closed")

    # Song is intentionally kept: the host may start again immediately. A
    # pending confirmation from the previous round is dropped.
    lobby.status = "waiting"
    _clear_start_request(lobby)
    lobby.updated_at = _now()
    db.flush()
    state = lobby_state(db, lobby)
    db.commit()
    return state
