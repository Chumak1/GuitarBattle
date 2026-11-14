"""API-level tests for Guitar Duels.

They exercise the same HTTP contract the JUCE client uses. The disposable
PostgreSQL database from ``conftest.py`` is required; without it the whole
module is skipped with the standard message.
"""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
from uuid import UUID

import pytest
from sqlalchemy import update

from app.models.duel import DuelMatch
from app.services import duel_rules

pytestmark = pytest.mark.db

API = "/api/v1"


def _create(
    client,
    headers,
    song_id: str,
    *,
    mode: str = "bot",
    bot_level: str | None = "easy",
) -> dict:
    payload = {"song_id": song_id, "mode": mode}
    if bot_level is not None:
        payload["bot_level"] = bot_level
    response = client.post(f"{API}/duels", json=payload, headers=headers)
    assert response.status_code == 201, response.text
    return response.json()


def _rewind_start(db_session, match_id: str, seconds: int = 10) -> None:
    db_session.execute(
        update(DuelMatch)
        .where(DuelMatch.id == UUID(match_id))
        .values(
            started_at=datetime.now(timezone.utc) - timedelta(seconds=seconds)
        )
    )
    db_session.commit()


def _ages_ago(db_session, match_id: str, seconds: int) -> None:
    db_session.execute(
        update(DuelMatch)
        .where(DuelMatch.id == UUID(match_id))
        .values(
            created_at=datetime.now(timezone.utc) - timedelta(seconds=seconds)
        )
    )
    db_session.commit()


def _human(participants: list[dict]) -> dict:
    return next(item for item in participants if not item["is_bot"])


def _bot(participants: list[dict]) -> dict:
    return next(item for item in participants if item["is_bot"])


def test_bot_match_is_server_authoritative_and_idempotent(
    db_client, user_account, published_song, db_session
):
    match = _create(db_client, user_account["headers"], published_song["id"])
    match_id = match["match_id"]
    assert match["mode"] == "bot"
    assert match["rules_version"] == 3
    assert match["note_count"] > 0
    assert 0 <= match["seed"] <= 0xFFFFFFFF
    assert len(match["participants"]) == 2

    ready = db_client.post(
        f"{API}/duels/{match_id}/ready", headers=user_account["headers"]
    )
    assert ready.status_code == 200, ready.text
    assert ready.json()["status"] == "playing"

    _rewind_start(db_session, match_id, seconds=10)
    count = min(8, match["note_count"])
    events = [
        {"note_index": index, "quality": "perfect"}
        for index in range(count)
    ]
    first = db_client.post(
        f"{API}/duels/{match_id}/events",
        json={"events": events},
        headers=user_account["headers"],
    )
    assert first.status_code == 200, first.text
    body = first.json()
    assert body["accepted"] == list(range(count))
    assert body["duplicates"] == []
    assert body["rejected"] == []
    assert body["match"]["participants"][0]["notes"] >= count

    duplicate = db_client.post(
        f"{API}/duels/{match_id}/events",
        json={"events": events},
        headers=user_account["headers"],
    )
    assert duplicate.status_code == 200, duplicate.text
    assert sorted(duplicate.json()["duplicates"]) == list(range(count))
    assert duplicate.json()["accepted"] == []

    finished = db_client.post(
        f"{API}/duels/{match_id}/finish", headers=user_account["headers"]
    )
    assert finished.status_code == 200, finished.text
    result = finished.json()
    assert result["already_finished"] is False
    assert result["match"]["status"] == "finished"
    participants = result["match"]["participants"]
    human = _human(participants)
    bot = _bot(participants)
    assert human["result"] in ("win", "loss", "draw")
    assert bot["result"] in ("win", "loss", "draw")
    assert count <= human["notes"] <= match["note_count"]
    assert human["xp_awarded"] > 0
    assert human["rating_before"] is None
    assert human["rating_after"] is None
    assert bot["rating_after"] is None
    # A bot win has no user id; a human win points at the human.
    assert result["match"]["winner_user_id"] in (None, user_account["id"])

    again = db_client.post(
        f"{API}/duels/{match_id}/finish", headers=user_account["headers"]
    )
    assert again.status_code == 200, again.text
    assert again.json()["already_finished"] is True
    assert _human(again.json()["match"]["participants"])["xp_awarded"] == human[
        "xp_awarded"
    ]

    history = db_client.get(
        f"{API}/duels/history", headers=user_account["headers"]
    )
    assert history.status_code == 200, history.text
    items = history.json()["items"]
    assert any(item["match_id"] == match_id for item in items)


def test_event_out_of_range_is_rejected_not_crashing(
    db_client, user_account, published_song, db_session
):
    match = _create(db_client, user_account["headers"], published_song["id"])
    match_id = match["match_id"]
    db_client.post(f"{API}/duels/{match_id}/ready", headers=user_account["headers"])
    _rewind_start(db_session, match_id)

    response = db_client.post(
        f"{API}/duels/{match_id}/events",
        json={"events": [{"note_index": match["note_count"], "quality": "perfect"}]},
        headers=user_account["headers"],
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["accepted"] == []
    assert body["rejected"][0]["code"] == "event_out_of_range"


def test_online_queue_pairing_elo_and_leaderboard(
    db_client, user_account, second_user, published_song, db_session
):
    first = _create(
        db_client, user_account["headers"], published_song["id"], mode="online"
    )
    match_id = first["match_id"]
    assert first["status"] == "waiting"
    assert first["your_side"] == "player"

    joined = _create(
        db_client, second_user["headers"], published_song["id"], mode="online"
    )
    assert joined["match_id"] == match_id
    assert joined["your_side"] == "opponent"
    assert len(joined["participants"]) == 2

    ready_first = db_client.post(
        f"{API}/duels/{match_id}/ready", headers=user_account["headers"]
    )
    assert ready_first.status_code == 200
    assert ready_first.json()["status"] == "waiting"

    ready_second = db_client.post(
        f"{API}/duels/{match_id}/ready", headers=second_user["headers"]
    )
    assert ready_second.status_code == 200
    assert ready_second.json()["status"] == "playing"

    _rewind_start(db_session, match_id, seconds=10)
    perfect = [
        {"note_index": index, "quality": "perfect"} for index in range(4)
    ]
    miss = [{"note_index": index, "quality": "miss"} for index in range(4)]

    first_events = db_client.post(
        f"{API}/duels/{match_id}/events",
        json={"events": perfect},
        headers=user_account["headers"],
    )
    assert first_events.status_code == 200, first_events.text
    assert first_events.json()["exchanges"] == []

    second_events = db_client.post(
        f"{API}/duels/{match_id}/events",
        json={"events": miss},
        headers=second_user["headers"],
    )
    assert second_events.status_code == 200, second_events.text
    assert len(second_events.json()["exchanges"]) == 4
    state = second_events.json()["match"]
    player = next(p for p in state["participants"] if p["side"] == "player")
    opponent = next(p for p in state["participants"] if p["side"] == "opponent")
    assert player["hp"] == pytest.approx(100.0)
    # rules_version 3 scales damage by the frozen chart length: the synthetic
    # chart of the 90 s test song has 180 notes, so the factor is 48/180 and
    # four perfect-vs-miss exchanges deal 4 * 10.0 * 48/180 HP.
    assert opponent["hp"] == pytest.approx(
        100.0 - 4 * 10.0 * duel_rules.DAMAGE_REFERENCE_NOTES / 180, abs=0.01
    )

    waiting = db_client.post(
        f"{API}/duels/{match_id}/finish", headers=second_user["headers"]
    )
    assert waiting.status_code == 200, waiting.text
    assert waiting.json()["waiting_for_opponent"] is True
    assert waiting.json()["match"]["status"] == "playing"

    final = db_client.post(
        f"{API}/duels/{match_id}/finish", headers=user_account["headers"]
    )
    assert final.status_code == 200, final.text
    final_body = final.json()
    assert final_body["match"]["status"] == "finished"
    assert final_body["already_finished"] is False
    final_player = next(
        p for p in final_body["match"]["participants"] if p["side"] == "player"
    )
    final_opponent = next(
        p for p in final_body["match"]["participants"] if p["side"] == "opponent"
    )
    assert final_player["result"] == "win"
    assert final_opponent["result"] == "loss"
    assert final_player["rating_before"] == 1000
    assert final_player["rating_after"] == 1016
    assert final_opponent["rating_before"] == 1000
    assert final_opponent["rating_after"] == 984
    assert final_player["rating_after"] + final_opponent["rating_after"] == 2000
    assert final_player["xp_awarded"] > 0
    assert final_opponent["xp_awarded"] > 0

    leaderboard = db_client.get(f"{API}/duels/leaderboard?period=all")
    assert leaderboard.status_code == 200, leaderboard.text
    items = leaderboard.json()["items"]
    assert items[0]["username"] == user_account["username"]
    assert items[0]["wins"] == 1
    assert items[0]["rating"] == 1016
    assert any(item["username"] == second_user["username"] for item in items)

    profile = db_client.get(
        f"{API}/duels/players/{user_account['id']}",
        headers=second_user["headers"],
    )
    assert profile.status_code == 200, profile.text
    assert profile.json()["rating"] == 1016
    assert profile.json()["online"]["wins"] == 1
    assert profile.json()["online"]["duels"] == 1

    me = db_client.get(f"{API}/duels/me", headers=user_account["headers"])
    assert me.status_code == 200, me.text
    assert me.json()["active_match"] is None


def test_rules_version_2_match_keeps_unscaled_damage(
    db_client, user_account, second_user, published_song, db_session
):
    """Matches created before the v3 rollout keep the old damage.

    The chart length is the same 180-note synthetic chart as above, but a
    frozen ``rules_version = 2`` must ignore the scaling factor entirely.
    """
    match = _create(
        db_client, user_account["headers"], published_song["id"], mode="online"
    )
    match_id = match["match_id"]
    _create(db_client, second_user["headers"], published_song["id"], mode="online")

    db_session.execute(
        update(DuelMatch)
        .where(DuelMatch.id == UUID(match_id))
        .values(rules_version=2)
    )
    db_session.commit()

    db_client.post(f"{API}/duels/{match_id}/ready", headers=user_account["headers"])
    db_client.post(f"{API}/duels/{match_id}/ready", headers=second_user["headers"])
    _rewind_start(db_session, match_id, seconds=10)

    db_client.post(
        f"{API}/duels/{match_id}/events",
        json={"events": [{"note_index": i, "quality": "perfect"} for i in range(4)]},
        headers=user_account["headers"],
    )
    body = db_client.post(
        f"{API}/duels/{match_id}/events",
        json={"events": [{"note_index": i, "quality": "miss"} for i in range(4)]},
        headers=second_user["headers"],
    )
    assert body.status_code == 200, body.text
    state = body.json()["match"]
    assert state["rules_version"] == 2

    opponent = next(p for p in state["participants"] if p["side"] == "opponent")
    # Unscaled: four perfect-vs-miss exchanges at the full 10.0 each.
    assert opponent["hp"] == pytest.approx(60.0)


def test_no_opponent_after_timeout_cancels_the_queue(
    db_client, user_account, published_song, db_session
):
    match = _create(
        db_client, user_account["headers"], published_song["id"], mode="online"
    )
    match_id = match["match_id"]
    _ages_ago(db_session, match_id, seconds=60)

    response = db_client.post(
        f"{API}/duels/{match_id}/ready", headers=user_account["headers"]
    )
    assert response.status_code == 409, response.text
    assert response.json()["detail"]["code"] == "no_opponent"

    state = db_client.get(
        f"{API}/duels/{match_id}", headers=user_account["headers"]
    )
    assert state.status_code == 200, state.text
    assert state.json()["status"] == "cancelled"


def test_online_finish_without_moves_is_a_forfeit(
    db_client, user_account, second_user, published_song, db_session
):
    first = _create(
        db_client, user_account["headers"], published_song["id"], mode="online"
    )
    match_id = first["match_id"]
    _create(db_client, second_user["headers"], published_song["id"], mode="online")
    db_client.post(f"{API}/duels/{match_id}/ready", headers=user_account["headers"])
    db_client.post(f"{API}/duels/{match_id}/ready", headers=second_user["headers"])
    _rewind_start(db_session, match_id)

    finished = db_client.post(
        f"{API}/duels/{match_id}/finish", headers=user_account["headers"]
    )
    assert finished.status_code == 200, finished.text
    body = finished.json()
    assert body["match"]["status"] == "finished"
    player = next(
        p for p in body["match"]["participants"] if p["side"] == "player"
    )
    opponent = next(
        p for p in body["match"]["participants"] if p["side"] == "opponent"
    )
    assert player["result"] == "loss"
    assert opponent["result"] == "win"
    assert player["rating_after"] == 984
    assert opponent["rating_after"] == 1016


def test_leave_without_a_move_is_a_technical_loss(
    db_client, user_account, published_song, db_session
):
    match = _create(db_client, user_account["headers"], published_song["id"])
    match_id = match["match_id"]
    db_client.post(f"{API}/duels/{match_id}/ready", headers=user_account["headers"])
    _rewind_start(db_session, match_id)

    left = db_client.post(
        f"{API}/duels/{match_id}/leave", headers=user_account["headers"]
    )
    assert left.status_code == 200, left.text
    body = left.json()
    assert body["match"]["status"] == "finished"
    human = _human(body["match"]["participants"])
    assert human["result"] == "loss"
    assert human["xp_awarded"] == 0
    assert body["match"]["winner_side"] == "opponent"


def test_leave_after_moves_is_decided_by_health(
    db_client, user_account, second_user, published_song, db_session
):
    first = _create(
        db_client, user_account["headers"], published_song["id"], mode="online"
    )
    match_id = first["match_id"]
    _create(db_client, second_user["headers"], published_song["id"], mode="online")
    db_client.post(f"{API}/duels/{match_id}/ready", headers=user_account["headers"])
    db_client.post(f"{API}/duels/{match_id}/ready", headers=second_user["headers"])
    _rewind_start(db_session, match_id, seconds=10)

    # The first player lands four perfect notes, the second misses all four.
    db_client.post(
        f"{API}/duels/{match_id}/events",
        json={"events": [{"note_index": i, "quality": "perfect"} for i in range(4)]},
        headers=user_account["headers"],
    )
    db_client.post(
        f"{API}/duels/{match_id}/events",
        json={"events": [{"note_index": i, "quality": "miss"} for i in range(4)]},
        headers=second_user["headers"],
    )

    left = db_client.post(
        f"{API}/duels/{match_id}/leave", headers=second_user["headers"]
    )
    assert left.status_code == 200, left.text
    body = left.json()
    assert body["match"]["status"] == "finished"
    opponent = next(
        p for p in body["match"]["participants"] if p["side"] == "opponent"
    )
    assert opponent["result"] == "loss"
    assert opponent["hp"] < 100.0


def test_websocket_connect_and_ping(
    db_client, user_account, published_song
):
    match = _create(db_client, user_account["headers"], published_song["id"])
    token = user_account["token"]
    with db_client.websocket_connect(
        f"{API}/duels/{match['match_id']}/ws?token={token}"
    ) as websocket:
        first = websocket.receive_json()
        assert first["type"] == "connected"
        assert first["your_side"] == "player"

        websocket.send_json({"type": "ping"})
        for _ in range(5):
            message = websocket.receive_json()
            if message["type"] == "pong":
                break
        else:  # pragma: no cover - defensive
            raise AssertionError("No pong received")
