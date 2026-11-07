"""API tests for the duel lobby (``GET /api/v1/duels/lobby``).

The lobby is the list the client shows under the rating panel. It must only
contain rooms that are actually joinable: online, waiting, not older than the
30-second queue timeout, and not already full. These tests exercise the same
HTTP contract the JUCE client uses; the disposable PostgreSQL database from
``conftest.py`` is required and the module is skipped without it.
"""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
from uuid import UUID

import pytest
from sqlalchemy import select, update

from app.models.duel import DuelMatch, DuelParticipant
from tests import helpers

pytestmark = pytest.mark.db

API = "/api/v1"


def _online_room(client, headers, song_id: str) -> dict:
    response = client.post(
        f"{API}/duels", json={"song_id": song_id, "mode": "online"}, headers=headers
    )
    assert response.status_code == 201, response.text
    body = response.json()
    assert body["mode"] == "online"
    assert body["status"] == "waiting"
    return body


def _bot_match(client, headers, song_id: str) -> dict:
    response = client.post(
        f"{API}/duels",
        json={"song_id": song_id, "mode": "bot", "bot_level": "easy"},
        headers=headers,
    )
    assert response.status_code == 201, response.text
    return response.json()


def _ages_ago(db_session, match_id: str, seconds: int) -> None:
    db_session.execute(
        update(DuelMatch)
        .where(DuelMatch.id == UUID(match_id))
        .values(created_at=datetime.now(timezone.utc) - timedelta(seconds=seconds))
    )
    db_session.commit()


def _lobby(client, headers) -> dict:
    response = client.get(f"{API}/duels/lobby", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _room_ids(body: dict) -> list[str]:
    return [item["match_id"] for item in body["items"]]


def test_lobby_requires_a_token(db_client, published_song, user_account):
    _online_room(db_client, user_account["headers"], published_song["id"])
    response = db_client.get(f"{API}/duels/lobby")
    assert response.status_code == 401, response.text


def test_lobby_filters_and_sorts_joinable_rooms(
    db_client, db_session, admin_user, user_account, second_user, published_song
):
    # A second and a third published song: online rooms for the same song are
    # paired by POST /duels, so separate rooms need separate songs.
    song_b = helpers.publish_song(
        db_client,
        admin_user["headers"],
        helpers.create_song(db_client, admin_user["headers"], title="Lobby B")["id"],
    )
    song_c = helpers.publish_song(
        db_client,
        admin_user["headers"],
        helpers.create_song(db_client, admin_user["headers"], title="Lobby C")["id"],
    )
    song_d = helpers.publish_song(
        db_client,
        admin_user["headers"],
        helpers.create_song(db_client, admin_user["headers"], title="Lobby D")["id"],
    )
    third = helpers.register_user(db_client)
    fourth = helpers.register_user(db_client)
    viewer = helpers.register_user(db_client)

    oldest = _online_room(db_client, user_account["headers"], published_song["id"])
    _ages_ago(db_session, oldest["match_id"], seconds=5)
    middle = _online_room(db_client, second_user["headers"], song_b["id"])
    newest = _online_room(db_client, third["headers"], song_c["id"])

    # Excluded: a bot match, an expired online room and a room that started.
    _bot_match(db_client, user_account["headers"], published_song["id"])
    expired = _online_room(db_client, fourth["headers"], song_d["id"])
    _ages_ago(db_session, expired["match_id"], seconds=60)

    started = _online_room(db_client, fourth["headers"], song_d["id"])
    # The second player pairs into ``started`` only if it is still fresh; it is.
    joined = db_client.post(
        f"{API}/duels/{started['match_id']}/join", headers=viewer["headers"]
    )
    assert joined.status_code == 200, joined.text
    db_client.post(f"{API}/duels/{started['match_id']}/ready", headers=fourth["headers"])
    db_client.post(f"{API}/duels/{started['match_id']}/ready", headers=viewer["headers"])

    body = _lobby(db_client, viewer["headers"])
    assert body["total"] == 3
    assert _room_ids(body) == [
        newest["match_id"],
        middle["match_id"],
        oldest["match_id"],
    ]

    item = body["items"][0]
    assert set(item) == {
        "match_id",
        "song_id",
        "song_slug",
        "song_title",
        "host_name",
        "host_rating",
        "created_at",
        "waiting_seconds",
        "note_count",
    }
    assert item["song_id"] == newest["song"]["id"]
    assert item["song_slug"] == song_c["slug"]
    assert item["song_title"] == song_c["title"]
    assert item["host_name"] == third["username"]
    assert item["host_rating"] == 1000
    assert item["note_count"] == newest["note_count"]
    assert 0 <= item["waiting_seconds"] <= 5
    assert item["created_at"].endswith("Z") or "+00:00" in item["created_at"]

    # The older room reports a larger wait and still lists its own host.
    assert body["items"][2]["host_name"] == user_account["username"]
    assert body["items"][2]["waiting_seconds"] >= 4


def test_lobby_room_is_joinable_and_disappears_after_start(
    db_client, db_session, user_account, second_user, published_song
):
    room = _online_room(db_client, user_account["headers"], published_song["id"])
    match_id = room["match_id"]

    assert match_id in _room_ids(_lobby(db_client, second_user["headers"]))

    joined = db_client.post(
        f"{API}/duels/{match_id}/join", headers=second_user["headers"]
    )
    assert joined.status_code == 200, joined.text
    state = joined.json()
    assert state["match_id"] == match_id
    assert state["status"] == "waiting"
    assert state["your_side"] == "opponent"
    assert len(state["participants"]) == 2
    sides = {p["side"]: p for p in state["participants"]}
    assert sides["player"]["username"] == user_account["username"]
    assert sides["opponent"]["username"] == second_user["username"]

    participant = db_session.scalar(
        select(DuelParticipant).where(
            DuelParticipant.match_id == UUID(match_id),
            DuelParticipant.side == "opponent",
        )
    )
    assert participant is not None
    assert participant.joined_at is not None

    # Full rooms are not joinable, so they are not advertised.
    assert match_id not in _room_ids(_lobby(db_client, user_account["headers"]))
    full = db_client.post(
        f"{API}/duels/{match_id}/join",
        headers=helpers.register_user(db_client)["headers"],
    )
    assert full.status_code == 409, full.text
    assert full.json()["detail"]["code"] == "match_full"

    first_ready = db_client.post(
        f"{API}/duels/{match_id}/ready", headers=user_account["headers"]
    )
    assert first_ready.status_code == 200, first_ready.text
    assert first_ready.json()["status"] == "waiting"
    second_ready = db_client.post(
        f"{API}/duels/{match_id}/ready", headers=second_user["headers"]
    )
    assert second_ready.status_code == 200, second_ready.text
    assert second_ready.json()["status"] == "playing"

    body = _lobby(db_client, user_account["headers"])
    assert match_id not in _room_ids(body)
    assert body["total"] == 0


def test_lobby_room_expires_after_queue_timeout(
    db_client, db_session, user_account, second_user, published_song
):
    room = _online_room(db_client, user_account["headers"], published_song["id"])
    match_id = room["match_id"]
    assert match_id in _room_ids(_lobby(db_client, second_user["headers"]))

    _ages_ago(db_session, match_id, seconds=60)

    body = _lobby(db_client, second_user["headers"])
    assert body["items"] == []
    assert body["total"] == 0

    joined = db_client.post(
        f"{API}/duels/{match_id}/join", headers=second_user["headers"]
    )
    assert joined.status_code == 409, joined.text
    assert joined.json()["detail"]["code"] == "no_opponent"
    assert _lobby(db_client, second_user["headers"])["items"] == []


def test_lobby_is_capped_at_fifty_rooms(
    db_client, db_session, user_account, published_song
):
    now = datetime.now(timezone.utc)
    for index in range(51):
        match = DuelMatch(
            song_id=UUID(published_song["id"]),
            mode="online",
            status="waiting",
            seed=index,
            note_count=10,
            created_at=now,
            updated_at=now,
        )
        db_session.add(match)
        db_session.flush()
        db_session.add(
            DuelParticipant(
                match_id=match.id,
                user_id=UUID(user_account["id"]),
                side="player",
                is_bot=False,
            )
        )
    db_session.commit()

    body = _lobby(db_client, user_account["headers"])
    assert body["total"] == 51
    assert len(body["items"]) == 50
