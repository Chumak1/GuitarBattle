"""API tests for shared duel lobbies (``/api/v1/duels/lobbies``).

The flow under test is the product flow:

1. the host creates a room with a name and an optional password, no song yet,
2. a guest joins (password checked, second guest rejected) and both chat,
3. only the host picks a published song and starts the match,
4. after the match the same players either replay the song (``rematch``) or
   return to the same room (``return-to-lobby``) with chat and membership kept.

Everything goes through HTTP, exactly like the JUCE client. The disposable
PostgreSQL database from ``conftest.py`` is required.
"""
from __future__ import annotations

from uuid import UUID, uuid4

import pytest
from sqlalchemy import select

from app.models.duel import DuelLobby, DuelLobbyMessage
from tests import helpers

pytestmark = pytest.mark.db

API = "/api/v1"
ROOMS = f"{API}/duels/lobbies"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _create_room(client, headers, name: str = "Friday night", password=None) -> dict:
    payload: dict = {"name": name}
    if password is not None:
        payload["password"] = password
    response = client.post(ROOMS, json=payload, headers=headers)
    assert response.status_code == 201, response.text
    return response.json()


def _join(client, lobby_id: str, headers, password=None):
    body = None if password is None else {"password": password}
    return client.post(f"{ROOMS}/{lobby_id}/join", json=body, headers=headers)


def _state(client, lobby_id: str, headers) -> dict:
    response = client.get(f"{ROOMS}/{lobby_id}", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _select_song(client, lobby_id: str, headers, song_id: str):
    return client.post(
        f"{ROOMS}/{lobby_id}/song", json={"song_id": song_id}, headers=headers
    )


def _start(client, lobby_id: str, headers):
    return client.post(f"{ROOMS}/{lobby_id}/start", headers=headers)


def _request_start(client, lobby_id: str, headers):
    return client.post(f"{ROOMS}/{lobby_id}/start/request", headers=headers)


def _confirm_start(client, lobby_id: str, headers):
    return client.post(f"{ROOMS}/{lobby_id}/start/confirm", headers=headers)


def _decline_start(client, lobby_id: str, headers):
    return client.post(f"{ROOMS}/{lobby_id}/start/decline", headers=headers)


def _chat(client, lobby_id: str, headers, message: str):
    return client.post(
        f"{ROOMS}/{lobby_id}/chat", json={"message": message}, headers=headers
    )


def _history(client, lobby_id: str, headers, after_id: int | None = None) -> dict:
    url = f"{ROOMS}/{lobby_id}/chat"
    if after_id is not None:
        url += f"?after_id={after_id}"
    response = client.get(url, headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _ready_room(client, host, guest, song_id: str, **kwargs) -> dict:
    """Create + join + pick a song; returns the started lobby payload."""
    room = _create_room(client, host["headers"], **kwargs)
    joined = _join(client, room["lobby_id"], guest["headers"], kwargs.get("password"))
    assert joined.status_code == 200, joined.text
    assert _select_song(client, room["lobby_id"], host["headers"], song_id).status_code == 200
    started = _start(client, room["lobby_id"], host["headers"])
    assert started.status_code == 200, started.text
    return {**room, **started.json()}


# ---------------------------------------------------------------------------
# Create / list
# ---------------------------------------------------------------------------
def test_create_lobby_has_no_song_and_never_returns_the_password(
    db_client, db_session, user_account
):
    room = _create_room(db_client, user_account["headers"], password="secret-pass")

    assert room["name"] == "Friday night"
    assert room["status"] == "waiting"
    assert room["has_password"] is True
    assert room["song"] is None
    assert room["match_id"] is None
    assert room["guest_user_id"] is None
    assert room["host_user_id"] == user_account["id"]
    assert "password" not in room
    assert "password_hash" not in room
    assert [p["is_host"] for p in room["players"]] == [True]
    assert room["players"][0]["username"] == user_account["username"]

    stored = db_session.scalar(
        select(DuelLobby).where(DuelLobby.id == UUID(room["lobby_id"]))
    )
    assert stored is not None
    assert stored.password_hash is not None
    assert stored.password_hash != "secret-pass"
    assert stored.password_hash.startswith("$argon2")

    # A room without a password reports has_password=false.
    plain = _create_room(db_client, user_account["headers"], name="No password")
    assert plain["has_password"] is False

    # Validation: blank/too long names are rejected, auth is required.
    assert (
        db_client.post(ROOMS, json={"name": "   "}, headers=user_account["headers"]).status_code
        == 422
    )
    assert (
        db_client.post(
            ROOMS, json={"name": "x" * 41}, headers=user_account["headers"]
        ).status_code
        == 422
    )
    assert db_client.post(ROOMS, json={"name": "Room"}).status_code == 401


def test_lobby_list_reports_players_song_and_password_flag(
    db_client, user_account, second_user, third_user, published_song
):
    open_room = _create_room(
        db_client, user_account["headers"], name="Open room"
    )
    locked_room = _create_room(
        db_client, second_user["headers"], name="Locked room", password="pw"
    )

    body = db_client.get(ROOMS, headers=third_user["headers"]).json()
    assert body["total"] == 2
    assert body["include_full"] is False
    by_id = {item["lobby_id"]: item for item in body["items"]}
    assert set(by_id) == {open_room["lobby_id"], locked_room["lobby_id"]}
    assert by_id[locked_room["lobby_id"]]["has_password"] is True
    assert by_id[open_room["lobby_id"]]["has_password"] is False
    assert by_id[open_room["lobby_id"]]["status"] == "waiting"
    assert by_id[open_room["lobby_id"]]["song"] is None
    assert [p["username"] for p in by_id[open_room["lobby_id"]]["players"]] == [
        user_account["username"]
    ]

    joined = _join(db_client, open_room["lobby_id"], third_user["headers"])
    assert joined.status_code == 200, joined.text

    # A full room is hidden by default and visible with include_full=true.
    default_ids = {
        item["lobby_id"]
        for item in db_client.get(ROOMS, headers=second_user["headers"]).json()["items"]
    }
    assert open_room["lobby_id"] not in default_ids

    full = db_client.get(
        ROOMS, params={"include_full": "true"}, headers=second_user["headers"]
    ).json()
    assert full["include_full"] is True
    full_item = next(
        item for item in full["items"] if item["lobby_id"] == open_room["lobby_id"]
    )
    assert len(full_item["players"]) == 2

    # The host can pick a song and the list shows it before the start.
    assert (
        _select_song(
            db_client, open_room["lobby_id"], user_account["headers"], published_song["id"]
        ).status_code
        == 200
    )
    body = db_client.get(
        ROOMS, params={"include_full": "true"}, headers=second_user["headers"]
    ).json()
    item = next(
        row for row in body["items"] if row["lobby_id"] == open_room["lobby_id"]
    )
    assert item["song"]["id"] == published_song["id"]
    assert item["song"]["slug"] == published_song["slug"]


# ---------------------------------------------------------------------------
# Join / leave
# ---------------------------------------------------------------------------
def test_join_checks_password_and_rejects_a_third_player(
    db_client, user_account, second_user, third_user
):
    room = _create_room(db_client, user_account["headers"], password="open-sesame")
    lobby_id = room["lobby_id"]

    missing = _join(db_client, lobby_id, second_user["headers"])
    assert missing.status_code == 403, missing.text
    assert missing.json()["detail"]["code"] == "password_required"

    wrong = _join(db_client, lobby_id, second_user["headers"], password="nope")
    assert wrong.status_code == 403, wrong.text
    assert wrong.json()["detail"]["code"] == "invalid_password"

    # Joining without a token is rejected before any password logic.
    assert db_client.post(f"{ROOMS}/{lobby_id}/join", json={"password": "open-sesame"}).status_code == 401

    ok = _join(db_client, lobby_id, second_user["headers"], password="open-sesame")
    assert ok.status_code == 200, ok.text
    state = ok.json()
    assert state["guest_user_id"] == second_user["id"]
    assert [p["username"] for p in state["players"]] == [
        user_account["username"],
        second_user["username"],
    ]
    assert [p["is_host"] for p in state["players"]] == [True, False]

    # Duplicate join is idempotent, for the guest and for the host.
    again = _join(db_client, lobby_id, second_user["headers"], password="open-sesame")
    assert again.status_code == 200, again.text
    assert again.json()["guest_user_id"] == second_user["id"]
    assert _join(db_client, lobby_id, user_account["headers"]).status_code == 200

    full = _join(db_client, lobby_id, third_user["headers"], password="open-sesame")
    assert full.status_code == 409, full.text
    assert full.json()["detail"]["code"] == "lobby_full"

    # Only the two members may read the room; the third player gets 403.
    assert _state(db_client, lobby_id, second_user["headers"])["guest_user_id"] == second_user["id"]
    outsider = db_client.get(f"{ROOMS}/{lobby_id}", headers=third_user["headers"])
    assert outsider.status_code == 403, outsider.text
    assert outsider.json()["detail"]["code"] == "not_participant"

    # The guest leaves, the room keeps waiting and can be joined again.
    left = db_client.post(
        f"{ROOMS}/{lobby_id}/leave", headers=second_user["headers"]
    )
    assert left.status_code == 200, left.text
    assert left.json()["guest_user_id"] is None
    assert left.json()["status"] == "waiting"
    assert (
        _join(db_client, lobby_id, third_user["headers"], password="open-sesame").status_code
        == 200
    )


def test_host_leave_closes_the_room_and_blocks_chat(
    db_client, user_account, second_user
):
    room = _create_room(db_client, user_account["headers"], name="Closing")
    lobby_id = room["lobby_id"]
    assert _join(db_client, lobby_id, second_user["headers"]).status_code == 200
    assert _chat(db_client, lobby_id, second_user["headers"], "hi").status_code == 201

    left = db_client.post(f"{ROOMS}/{lobby_id}/leave", headers=user_account["headers"])
    assert left.status_code == 200, left.text
    assert left.json()["status"] == "closed"

    # Closed rooms disappear from the list and reject new players.
    listed = db_client.get(ROOMS, headers=second_user["headers"]).json()
    assert lobby_id not in {item["lobby_id"] for item in listed["items"]}
    assert _join(
        db_client, lobby_id, helpers.register_user(db_client)["headers"]
    ).status_code == 409

    # The chat history stays readable for the former members, but is frozen.
    history = _history(db_client, lobby_id, second_user["headers"])
    assert [item["text"] for item in history["items"]] == ["hi"]
    blocked = _chat(db_client, lobby_id, second_user["headers"], "still there?")
    assert blocked.status_code == 409, blocked.text
    assert blocked.json()["detail"]["code"] == "lobby_closed"


# ---------------------------------------------------------------------------
# Chat
# ---------------------------------------------------------------------------
def test_chat_is_sanitized_incremental_and_rate_limited(
    db_client, db_session, user_account, second_user, third_user
):
    room = _create_room(db_client, user_account["headers"])
    lobby_id = room["lobby_id"]
    assert _join(db_client, lobby_id, second_user["headers"]).status_code == 200

    first = _chat(
        db_client, lobby_id, user_account["headers"], "  <b>hello</b>   world  "
    )
    assert first.status_code == 201, first.text
    assert first.json()["text"] == "hello world"
    assert first.json()["username"] == user_account["username"]
    assert first.json()["user_id"] == user_account["id"]

    second = _chat(db_client, lobby_id, second_user["headers"], "hi <script>x</script>")
    assert second.status_code == 201, second.text
    assert second.json()["text"] == "hi x"

    history = _history(db_client, lobby_id, second_user["headers"])
    assert [item["text"] for item in history["items"]] == ["hello world", "hi x"]
    ids = [item["id"] for item in history["items"]]
    assert ids == sorted(ids)
    assert history["latest_id"] == ids[-1]

    # after_id returns only newer messages.
    empty = _history(db_client, lobby_id, user_account["headers"], after_id=ids[-1])
    assert empty["items"] == []
    assert empty["latest_id"] == ids[-1]

    third = _chat(db_client, lobby_id, user_account["headers"], "third")
    assert third.status_code == 201
    increment = _history(
        db_client, lobby_id, second_user["headers"], after_id=ids[-1]
    )
    assert [item["text"] for item in increment["items"]] == ["third"]

    # Sanitisation: markup-only messages and blank text are rejected.
    empty_after_sanitize = _chat(
        db_client, lobby_id, user_account["headers"], "<script></script>"
    )
    assert empty_after_sanitize.status_code == 422, empty_after_sanitize.text
    assert empty_after_sanitize.json()["detail"]["code"] == "empty_message"
    blank = _chat(db_client, lobby_id, user_account["headers"], "     ")
    assert blank.status_code == 422, blank.text

    # Too long is a validation error, not a 500.
    assert (
        _chat(db_client, lobby_id, user_account["headers"], "x" * 501).status_code
        == 422
    )

    # Only members read and write chat.
    assert (
        db_client.get(f"{ROOMS}/{lobby_id}/chat", headers=third_user["headers"]).status_code
        == 403
    )
    assert (
        _chat(db_client, lobby_id, third_user["headers"], "intruder").status_code
        == 403
    )

    # Rate limit: a burst is allowed, then the endpoint answers 429. A fresh
    # room is used so the bucket is not affected by the messages above.
    spam_room = _create_room(db_client, user_account["headers"], name="Spam")
    assert (
        _join(db_client, spam_room["lobby_id"], second_user["headers"]).status_code
        == 200
    )
    statuses = [
        _chat(
            db_client,
            spam_room["lobby_id"],
            second_user["headers"],
            f"spam {index}",
        ).status_code
        for index in range(20)
    ]
    assert statuses[0] == 201, statuses
    assert 429 in statuses, statuses

    stored = db_session.scalars(
        select(DuelLobbyMessage).where(DuelLobbyMessage.lobby_id == UUID(lobby_id))
    ).all()
    assert all("<" not in row.text and ">" not in row.text for row in stored)


# ---------------------------------------------------------------------------
# Song picker / start
# ---------------------------------------------------------------------------
def test_only_the_host_selects_a_published_song(
    db_client, user_account, second_user, third_user, published_song, draft_song
):
    room = _create_room(db_client, user_account["headers"])
    lobby_id = room["lobby_id"]
    assert _join(db_client, lobby_id, second_user["headers"]).status_code == 200

    guest_try = _select_song(
        db_client, lobby_id, second_user["headers"], published_song["id"]
    )
    assert guest_try.status_code == 403, guest_try.text
    assert guest_try.json()["detail"]["code"] == "not_host"

    outsider = _select_song(
        db_client, lobby_id, third_user["headers"], published_song["id"]
    )
    assert outsider.status_code == 403, outsider.text
    assert outsider.json()["detail"]["code"] == "not_participant"

    assert (
        _select_song(db_client, lobby_id, user_account["headers"], str(uuid4())).status_code
        == 404
    )
    unpublished = _select_song(
        db_client, lobby_id, user_account["headers"], draft_song["id"]
    )
    assert unpublished.status_code == 422, unpublished.text
    assert unpublished.json()["detail"]["code"] == "song_not_published"

    selected = _select_song(
        db_client, lobby_id, user_account["headers"], published_song["id"]
    )
    assert selected.status_code == 200, selected.text
    assert selected.json()["song"]["id"] == published_song["id"]
    assert selected.json()["song"]["slug"] == published_song["slug"]

    # Both players see the same song in their own state.
    guest_state = _state(db_client, lobby_id, second_user["headers"])
    assert guest_state["song"]["id"] == published_song["id"]
    assert guest_state["status"] == "waiting"


def test_only_the_host_starts_and_both_players_are_required(
    db_client, user_account, second_user, third_user, published_song
):
    room = _create_room(db_client, user_account["headers"])
    lobby_id = room["lobby_id"]

    no_guest = _start(db_client, lobby_id, user_account["headers"])
    assert no_guest.status_code == 409, no_guest.text
    assert no_guest.json()["detail"]["code"] == "opponent_missing"

    assert _join(db_client, lobby_id, second_user["headers"]).status_code == 200

    no_song = _start(db_client, lobby_id, user_account["headers"])
    assert no_song.status_code == 409, no_song.text
    assert no_song.json()["detail"]["code"] == "song_not_selected"

    guest_try = _start(db_client, lobby_id, second_user["headers"])
    assert guest_try.status_code == 403, guest_try.text
    assert guest_try.json()["detail"]["code"] == "not_host"

    outsider = _start(db_client, lobby_id, third_user["headers"])
    assert outsider.status_code == 403, outsider.text

    assert (
        _select_song(
            db_client, lobby_id, user_account["headers"], published_song["id"]
        ).status_code
        == 200
    )

    started = _start(db_client, lobby_id, user_account["headers"])
    assert started.status_code == 200, started.text
    body = started.json()
    assert body["lobby"]["lobby_id"] == lobby_id
    assert body["lobby"]["status"] == "playing"
    # Legacy start: no confirmation round, the match is live immediately.
    assert body["lobby"]["start"] is None
    match_id = body["match_id"]
    assert match_id

    match = db_client.get(f"{API}/duels/{match_id}", headers=user_account["headers"])
    assert match.status_code == 200, match.text
    snapshot = match.json()
    assert snapshot["status"] == "playing"
    assert snapshot["mode"] == "online"
    assert snapshot["lobby_id"] == lobby_id
    assert snapshot["song"]["id"] == published_song["id"]
    assert snapshot["note_count"] > 0
    assert len(snapshot["participants"]) == 2
    assert snapshot["your_side"] == "player"
    assert all(p["ready"] for p in snapshot["participants"])
    assert {p["username"] for p in snapshot["participants"]} == {
        user_account["username"],
        second_user["username"],
    }

    # The guest is the opponent and sees the same match.
    guest_match = db_client.get(f"{API}/duels/{match_id}", headers=second_user["headers"])
    assert guest_match.status_code == 200
    assert guest_match.json()["your_side"] == "opponent"
    assert guest_match.json()["match_id"] == match_id

    # A second start is refused and the song is frozen while playing.
    assert _start(db_client, lobby_id, user_account["headers"]).status_code == 409
    assert (
        _select_song(
            db_client, lobby_id, user_account["headers"], published_song["id"]
        ).status_code
        == 409
    )


# ---------------------------------------------------------------------------
# Two-step start: request -> confirm / decline
# ---------------------------------------------------------------------------
def test_start_request_is_host_only_and_keeps_the_room_waiting(
    db_client, user_account, second_user, published_song
):
    room = _create_room(db_client, user_account["headers"], name="Confirm me")
    lobby_id = room["lobby_id"]
    assert _join(db_client, lobby_id, second_user["headers"]).status_code == 200

    # No song yet: nothing to confirm.
    no_song = _request_start(db_client, lobby_id, user_account["headers"])
    assert no_song.status_code == 409, no_song.text
    assert no_song.json()["detail"]["code"] == "song_not_selected"

    assert (
        _select_song(
            db_client, lobby_id, user_account["headers"], published_song["id"]
        ).status_code
        == 200
    )

    # Only the host may open the confirmation round.
    guest_try = _request_start(db_client, lobby_id, second_user["headers"])
    assert guest_try.status_code == 403, guest_try.text
    assert guest_try.json()["detail"]["code"] == "not_host"

    requested = _request_start(db_client, lobby_id, user_account["headers"])
    assert requested.status_code == 200, requested.text
    body = requested.json()
    # Nothing is created and nothing is downloaded: the room still waits.
    assert body["status"] == "waiting"
    assert body["match_id"] is None
    assert body["start"] == {
        "requested_by": user_account["id"],
        "host_confirmed": False,
        "guest_confirmed": False,
    }

    # Both members see the same pending request (this is what drives the
    # "Start the duel?" dialog on the guest side).
    guest_view = _state(db_client, lobby_id, second_user["headers"])
    assert guest_view["start"] == body["start"]
    assert guest_view["status"] == "waiting"

    # A repeated request from the host is idempotent and does not reset a
    # confirmation the guest already made.
    assert _confirm_start(db_client, lobby_id, second_user["headers"]).status_code == 200
    again = _request_start(db_client, lobby_id, user_account["headers"])
    assert again.status_code == 200, again.text
    assert again.json()["start"]["guest_confirmed"] is True
    assert again.json()["status"] == "waiting"


def test_one_confirmation_keeps_waiting_and_both_create_a_waiting_match(
    db_client, user_account, second_user, published_song
):
    room = _create_room(db_client, user_account["headers"], name="Both confirm")
    lobby_id = room["lobby_id"]
    assert _join(db_client, lobby_id, second_user["headers"]).status_code == 200
    assert (
        _select_song(
            db_client, lobby_id, user_account["headers"], published_song["id"]
        ).status_code
        == 200
    )

    # No pending request yet: a confirmation is a normal 409, not a 500.
    early = _confirm_start(db_client, lobby_id, second_user["headers"])
    assert early.status_code == 409, early.text
    assert early.json()["detail"]["code"] == "start_not_requested"

    assert _request_start(db_client, lobby_id, user_account["headers"]).status_code == 200

    host_only = _confirm_start(db_client, lobby_id, user_account["headers"])
    assert host_only.status_code == 200, host_only.text
    first = host_only.json()
    assert first["match_id"] is None
    assert first["lobby"]["status"] == "waiting"
    assert first["lobby"]["start"]["host_confirmed"] is True
    assert first["lobby"]["start"]["guest_confirmed"] is False
    assert first["lobby"]["match_id"] is None

    # Outsiders cannot confirm either.
    outsider = helpers.register_user(db_client)
    stranger = _confirm_start(db_client, lobby_id, outsider["headers"])
    assert stranger.status_code == 403, stranger.text
    assert stranger.json()["detail"]["code"] == "not_participant"

    both = _confirm_start(db_client, lobby_id, second_user["headers"])
    assert both.status_code == 200, both.text
    body = both.json()
    assert body["match_id"], body
    assert body["lobby"]["status"] == "playing"
    assert body["lobby"]["start"] is None
    match_id = body["match_id"]

    # The match exists, but its countdown must NOT run during the download.
    match = db_client.get(f"{API}/duels/{match_id}", headers=user_account["headers"])
    assert match.status_code == 200, match.text
    snapshot = match.json()
    assert snapshot["status"] == "waiting"
    assert snapshot["started_at"] is None
    assert snapshot["lobby_id"] == lobby_id
    assert snapshot["mode"] == "online"
    assert snapshot["starts_in_ms"] == 0
    assert all(not p["ready"] for p in snapshot["participants"])

    # The room is playing now: a second request and a second song are refused.
    assert _request_start(db_client, lobby_id, user_account["headers"]).status_code == 409
    assert (
        _select_song(
            db_client, lobby_id, user_account["headers"], published_song["id"]
        ).status_code
        == 409
    )

    # Once both sides report the download is ready, the shared countdown starts.
    assert (
        db_client.post(f"{API}/duels/{match_id}/ready", headers=user_account["headers"]).status_code
        == 200
    )
    started = db_client.post(
        f"{API}/duels/{match_id}/ready", headers=second_user["headers"]
    )
    assert started.status_code == 200, started.text
    assert started.json()["status"] == "playing"
    assert started.json()["started_at"] is not None
    assert all(p["ready"] for p in started.json()["participants"])


def test_decline_clears_the_request_and_keeps_the_room_open(
    db_client, user_account, second_user, published_song
):
    room = _create_room(db_client, user_account["headers"], name="Decline me")
    lobby_id = room["lobby_id"]
    assert _join(db_client, lobby_id, second_user["headers"]).status_code == 200
    assert (
        _select_song(
            db_client, lobby_id, user_account["headers"], published_song["id"]
        ).status_code
        == 200
    )
    assert _request_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    assert _confirm_start(db_client, lobby_id, user_account["headers"]).status_code == 200

    # Either player can decline; the whole round is dropped.
    declined = _decline_start(db_client, lobby_id, second_user["headers"])
    assert declined.status_code == 200, declined.text
    body = declined.json()
    assert body["status"] == "waiting"
    assert body["start"] is None
    assert body["match_id"] is None
    assert body["song"]["id"] == published_song["id"]

    # The host sees the same cleared state and may press START again.
    host_view = _state(db_client, lobby_id, user_account["headers"])
    assert host_view["start"] is None
    assert host_view["status"] == "waiting"

    # Declining with nothing pending is idempotent.
    assert _decline_start(db_client, lobby_id, user_account["headers"]).status_code == 200

    restarted = _request_start(db_client, lobby_id, user_account["headers"])
    assert restarted.status_code == 200, restarted.text
    assert restarted.json()["start"]["host_confirmed"] is False

    # Both confirm again: a fresh match is created for the same room.
    assert _confirm_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    final = _confirm_start(db_client, lobby_id, second_user["headers"])
    assert final.status_code == 200, final.text
    assert final.json()["match_id"]
    assert final.json()["lobby"]["status"] == "playing"


def test_cancelled_lobby_match_returns_the_room_to_waiting(
    db_client, user_account, second_user, third_user, published_song
):
    room = _create_room(db_client, user_account["headers"], name="Abort download")
    lobby_id = room["lobby_id"]
    assert _join(db_client, lobby_id, second_user["headers"]).status_code == 200
    assert (
        _select_song(
            db_client, lobby_id, user_account["headers"], published_song["id"]
        ).status_code
        == 200
    )
    assert _chat(db_client, lobby_id, second_user["headers"], "hold on").status_code == 201
    assert _request_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    assert _confirm_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    created = _confirm_start(db_client, lobby_id, second_user["headers"])
    assert created.status_code == 200, created.text
    match_id = created.json()["match_id"]
    assert created.json()["lobby"]["status"] == "playing"

    # Nobody may join between the confirmation and the download.
    assert (
        db_client.post(f"{API}/duels/{match_id}/join", headers=third_user["headers"]).status_code
        in (403, 409)
    )

    # One client aborts the download and leaves the fresh match.
    left = db_client.post(f"{API}/duels/{match_id}/leave", headers=user_account["headers"])
    assert left.status_code == 200, left.text
    assert left.json()["match"]["status"] == "cancelled"

    # Both players are back in the same room: waiting, same song, same chat.
    for headers in (user_account["headers"], second_user["headers"]):
        state = _state(db_client, lobby_id, headers)
        assert state["status"] == "waiting"
        assert state["start"] is None
        assert state["song"]["id"] == published_song["id"]
        assert state["guest_user_id"] == second_user["id"]

    history = _history(db_client, lobby_id, second_user["headers"])
    assert [item["text"] for item in history["items"]] == ["hold on"]

    # The host can request a new confirmation round immediately.
    again = _request_start(db_client, lobby_id, user_account["headers"])
    assert again.status_code == 200, again.text
    assert again.json()["status"] == "waiting"
    assert again.json()["start"]["host_confirmed"] is False


# ---------------------------------------------------------------------------
# After the match
# ---------------------------------------------------------------------------
def test_rematch_keeps_the_previous_result_and_creates_a_new_match(
    db_client, user_account, second_user, third_user, published_song
):
    started = _ready_room(
        db_client, user_account, second_user, published_song["id"], name="Rematch"
    )
    lobby_id = started["lobby_id"]
    first_match = started["match_id"]

    # Finish the first match (no notes submitted => the other side wins).
    finished = db_client.post(
        f"{API}/duels/{first_match}/finish", headers=user_account["headers"]
    )
    assert finished.status_code == 200, finished.text
    assert finished.json()["match"]["status"] == "finished"
    old_result = db_client.get(
        f"{API}/duels/{first_match}/result", headers=second_user["headers"]
    )
    assert old_result.status_code == 200, old_result.text
    old = old_result.json()
    assert old["status"] == "finished"
    assert old["winner_user_id"] == second_user["id"]
    old_finished_at = old["finished_at"]

    # Participants only.
    assert (
        db_client.post(
            f"{API}/duels/{first_match}/rematch", headers=third_user["headers"]
        ).status_code
        == 403
    )

    new_match = db_client.post(
        f"{API}/duels/{first_match}/rematch", headers=second_user["headers"]
    )
    assert new_match.status_code == 200, new_match.text
    body = new_match.json()
    assert body["match_id"] != first_match
    assert body["previous_match_id"] == first_match
    assert body["lobby_id"] == lobby_id
    assert body["song"]["id"] == published_song["id"]
    assert body["already_created"] is False
    second_match = body["match_id"]

    # The old result is untouched and still readable.
    preserved = db_client.get(
        f"{API}/duels/{first_match}/result", headers=user_account["headers"]
    )
    assert preserved.status_code == 200, preserved.text
    assert preserved.json()["status"] == "finished"
    assert preserved.json()["winner_user_id"] == second_user["id"]
    assert preserved.json()["finished_at"] == old_finished_at

    # The new match is a real playing match with the same two players.
    fresh = db_client.get(f"{API}/duels/{second_match}", headers=user_account["headers"])
    assert fresh.status_code == 200, fresh.text
    assert fresh.json()["status"] == "playing"
    assert fresh.json()["song"]["id"] == published_song["id"]
    assert fresh.json()["lobby_id"] == lobby_id
    assert fresh.json()["your_side"] == "player"

    # Clicking REPLAY twice does not create a second match.
    again = db_client.post(
        f"{API}/duels/{first_match}/rematch", headers=user_account["headers"]
    )
    assert again.status_code == 200, again.text
    assert again.json()["match_id"] == second_match
    assert again.json()["already_created"] is True

    # A quick-queue/bot match has no lobby to rematch into.
    bot = db_client.post(
        f"{API}/duels",
        json={"song_id": published_song["id"], "mode": "bot", "bot_level": "easy"},
        headers=user_account["headers"],
    )
    assert bot.status_code == 201, bot.text
    bot_rematch = db_client.post(
        f"{API}/duels/{bot.json()['match_id']}/rematch",
        headers=user_account["headers"],
    )
    assert bot_rematch.status_code == 409, bot_rematch.text
    assert bot_rematch.json()["detail"]["code"] == "lobby_not_found"


def test_return_to_lobby_keeps_chat_members_and_song(
    db_client, user_account, second_user, third_user, published_song
):
    started = _ready_room(
        db_client, user_account, second_user, published_song["id"], name="Again"
    )
    lobby_id = started["lobby_id"]
    match_id = started["match_id"]

    assert _chat(db_client, lobby_id, user_account["headers"], "good luck").status_code == 201
    assert _chat(db_client, lobby_id, second_user["headers"], "you too").status_code == 201

    while_playing = db_client.post(
        f"{API}/duels/{match_id}/return-to-lobby", headers=user_account["headers"]
    )
    assert while_playing.status_code == 409, while_playing.text
    assert while_playing.json()["detail"]["code"] == "match_not_finished"

    finished = db_client.post(
        f"{API}/duels/{match_id}/finish", headers=user_account["headers"]
    )
    assert finished.status_code == 200, finished.text
    assert finished.json()["match"]["status"] == "finished"

    assert (
        db_client.post(
            f"{API}/duels/{match_id}/return-to-lobby", headers=third_user["headers"]
        ).status_code
        == 403
    )

    returned = db_client.post(
        f"{API}/duels/{match_id}/return-to-lobby", headers=second_user["headers"]
    )
    assert returned.status_code == 200, returned.text
    lobby = returned.json()
    assert lobby["lobby_id"] == lobby_id
    assert lobby["status"] == "waiting"
    assert lobby["song"]["id"] == published_song["id"]
    assert lobby["host_user_id"] == user_account["id"]
    assert lobby["guest_user_id"] == second_user["id"]
    assert [p["username"] for p in lobby["players"]] == [
        user_account["username"],
        second_user["username"],
    ]

    # Chat survived the round trip for both members.
    history = _history(db_client, lobby_id, user_account["headers"])
    assert [item["text"] for item in history["items"]] == ["good luck", "you too"]

    # The host can start again immediately with the preserved song.
    restarted = _start(db_client, lobby_id, user_account["headers"])
    assert restarted.status_code == 200, restarted.text
    assert restarted.json()["match_id"] != match_id
    assert restarted.json()["lobby"]["status"] == "playing"

    # The previous result is still stored.
    previous = db_client.get(
        f"{API}/duels/{match_id}/result", headers=user_account["headers"]
    )
    assert previous.status_code == 200
    assert previous.json()["status"] == "finished"


def _ready(client, match_id: str, headers):
    return client.post(f"{API}/duels/{match_id}/ready", headers=headers)


def _finish_lobby_match(client, host, guest, match_id: str) -> None:
    """Start the shared countdown (both ready) and finish the match."""
    assert _ready(client, match_id, host["headers"]).status_code == 200
    last = _ready(client, match_id, guest["headers"])
    assert last.status_code == 200, last.text
    finished = client.post(f"{API}/duels/{match_id}/finish", headers=host["headers"])
    assert finished.status_code == 200, finished.text
    assert finished.json()["match"]["status"] == "finished"


def test_second_round_after_return_to_lobby_uses_the_new_song(
    db_client, user_account, second_user, published_song, second_published_song
):
    """Song B chosen in the room after a finished match must be played.

    Regression: the next match used to keep the previous song, so the room's
    current selection has to reach ``_create_lobby_match`` at confirm time.
    """
    room = _create_room(db_client, user_account["headers"], name="Song change")
    lobby_id = room["lobby_id"]
    assert _join(db_client, lobby_id, second_user["headers"]).status_code == 200

    assert (
        _select_song(
            db_client, lobby_id, user_account["headers"], published_song["id"]
        ).status_code
        == 200
    )
    assert _request_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    assert _confirm_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    confirmed = _confirm_start(db_client, lobby_id, second_user["headers"])
    assert confirmed.status_code == 200, confirmed.text
    first_match = confirmed.json()["match_id"]
    assert first_match

    # The match is frozen with song A.
    first = db_client.get(f"{API}/duels/{first_match}", headers=user_account["headers"])
    assert first.status_code == 200, first.text
    assert first.json()["song"]["id"] == published_song["id"]
    assert first.json()["song"]["slug"] == published_song["slug"]

    _finish_lobby_match(db_client, user_account, second_user, first_match)

    returned = db_client.post(
        f"{API}/duels/{first_match}/return-to-lobby", headers=user_account["headers"]
    )
    assert returned.status_code == 200, returned.text
    assert returned.json()["status"] == "waiting"
    assert returned.json()["song"]["id"] == published_song["id"]

    # The host picks another song while the room is waiting again.
    changed = _select_song(
        db_client, lobby_id, user_account["headers"], second_published_song["id"]
    )
    assert changed.status_code == 200, changed.text
    assert changed.json()["song"]["id"] == second_published_song["id"]
    assert changed.json()["song"]["slug"] == second_published_song["slug"]

    # Both confirm the new round: the fresh match must carry song B.
    assert _request_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    assert _confirm_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    second = _confirm_start(db_client, lobby_id, second_user["headers"])
    assert second.status_code == 200, second.text
    second_match = second.json()["match_id"]
    assert second_match and second_match != first_match
    assert second.json()["lobby"]["match_id"] == second_match

    fresh = db_client.get(f"{API}/duels/{second_match}", headers=second_user["headers"])
    assert fresh.status_code == 200, fresh.text
    body = fresh.json()
    assert body["song"]["id"] == second_published_song["id"]
    assert body["song"]["slug"] == second_published_song["slug"]
    assert body["lobby_id"] == lobby_id
    assert body["status"] == "waiting"
    assert body["note_count"] > 0


def test_song_change_cancels_the_pending_confirmation_round(
    db_client, user_account, second_user, published_song, second_published_song
):
    """A confirmation belongs to the song both players saw."""
    room = _create_room(db_client, user_account["headers"], name="Round identity")
    lobby_id = room["lobby_id"]
    assert _join(db_client, lobby_id, second_user["headers"]).status_code == 200
    assert (
        _select_song(
            db_client, lobby_id, user_account["headers"], published_song["id"]
        ).status_code
        == 200
    )

    assert _request_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    assert _confirm_start(db_client, lobby_id, second_user["headers"]).status_code == 200

    # Picking another song restarts the round.
    changed = _select_song(
        db_client, lobby_id, user_account["headers"], second_published_song["id"]
    )
    assert changed.status_code == 200, changed.text
    assert changed.json()["start"] is None

    stale = _confirm_start(db_client, lobby_id, second_user["headers"])
    assert stale.status_code == 409, stale.text
    assert stale.json()["detail"]["code"] == "start_not_requested"

    # The new round creates the match with the newly selected song.
    assert _request_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    assert _confirm_start(db_client, lobby_id, user_account["headers"]).status_code == 200
    confirmed = _confirm_start(db_client, lobby_id, second_user["headers"])
    assert confirmed.status_code == 200, confirmed.text
    match_id = confirmed.json()["match_id"]
    snapshot = db_client.get(f"{API}/duels/{match_id}", headers=user_account["headers"])
    assert snapshot.status_code == 200, snapshot.text
    assert snapshot.json()["song"]["id"] == second_published_song["id"]


def test_rematch_uses_the_song_selected_after_the_match(
    db_client, user_account, second_user, published_song, second_published_song
):
    """A rematch never replays the previous song behind the room's back."""
    started = _ready_room(
        db_client, user_account, second_user, published_song["id"], name="Rematch song"
    )
    lobby_id = started["lobby_id"]
    first_match = started["match_id"]

    finished = db_client.post(
        f"{API}/duels/{first_match}/finish", headers=user_account["headers"]
    )
    assert finished.status_code == 200, finished.text

    returned = db_client.post(
        f"{API}/duels/{first_match}/return-to-lobby", headers=user_account["headers"]
    )
    assert returned.status_code == 200, returned.text

    changed = _select_song(
        db_client, lobby_id, user_account["headers"], second_published_song["id"]
    )
    assert changed.status_code == 200, changed.text

    rematch = db_client.post(
        f"{API}/duels/{first_match}/rematch", headers=user_account["headers"]
    )
    assert rematch.status_code == 200, rematch.text
    body = rematch.json()
    assert body["match_id"] != first_match
    assert body["song"]["id"] == second_published_song["id"]
    assert body["song"]["slug"] == second_published_song["slug"]

    fresh = db_client.get(f"{API}/duels/{body['match_id']}", headers=user_account["headers"])
    assert fresh.status_code == 200, fresh.text
    assert fresh.json()["song"]["id"] == second_published_song["id"]

