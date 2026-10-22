"""Game session, score submission and leaderboard tests."""
import uuid

import pytest

from tests import helpers

pytestmark = pytest.mark.db

VALID_SCORE = {
    "score_points": 1200,
    "accuracy_percent": 97.5,
    "max_combo": 42,
    "perfect_count": 40,
    "good_count": 2,
    "miss_count": 1,
}


def _create_session(client, headers, song_id, speed_percent=100):
    response = client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": song_id, "speed_percent": speed_percent},
        headers=headers,
    )
    assert response.status_code == 201, response.text
    return response.json()


def _finish_session(client, headers, session_id, **overrides):
    payload = {**VALID_SCORE, **overrides}
    return client.post(
        f"{helpers.API}/games/sessions/{session_id}/finish",
        json=payload,
        headers=headers,
    )


def test_create_session_requires_auth(db_client, published_song):
    response = db_client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": published_song["id"], "speed_percent": 100},
    )
    assert response.status_code == 401


def test_create_session_requires_published_song(db_client, user_account, draft_song):
    draft = db_client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": draft_song["id"], "speed_percent": 100},
        headers=user_account["headers"],
    )
    assert draft.status_code == 404

    missing = db_client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": str(uuid.uuid4()), "speed_percent": 100},
        headers=user_account["headers"],
    )
    assert missing.status_code == 404


def test_create_session_returns_started_session(
    db_client, user_account, published_song
):
    body = _create_session(
        db_client, user_account["headers"], published_song["id"], speed_percent=90
    )
    assert body["song_id"] == published_song["id"]
    assert body["song_version"] == published_song["version"]
    assert body["speed_percent"] == 90
    assert body["status"] == "started"
    assert body["finished_at"] is None
    assert uuid.UUID(body["id"])


@pytest.mark.parametrize("speed", [0, 49, 121])
def test_create_session_rejects_invalid_speed(
    db_client, user_account, published_song, speed
):
    response = db_client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": published_song["id"], "speed_percent": speed},
        headers=user_account["headers"],
    )
    assert response.status_code == 422


@pytest.mark.parametrize(
    "overrides",
    [
        {"score_points": -1},
        {"accuracy_percent": 101},
        {"accuracy_percent": -1},
        {"max_combo": -1},
        {"perfect_count": -1},
        {"good_count": 1_000_001},
        {"miss_count": -100},
    ],
)
def test_finish_session_rejects_invalid_score(
    db_client, user_account, published_song, overrides
):
    session = _create_session(
        db_client, user_account["headers"], published_song["id"]
    )
    response = _finish_session(
        db_client, user_account["headers"], session["id"], **overrides
    )
    assert response.status_code == 422, response.text


def test_finish_session_saves_score_and_history(
    db_client, user_account, published_song
):
    session = _create_session(
        db_client, user_account["headers"], published_song["id"]
    )
    response = _finish_session(db_client, user_account["headers"], session["id"])
    assert response.status_code == 200, response.text
    score = response.json()
    assert score["session_id"] == session["id"]
    assert score["score_points"] == VALID_SCORE["score_points"]
    assert score["accuracy_percent"] == pytest.approx(VALID_SCORE["accuracy_percent"])
    assert score["max_combo"] == VALID_SCORE["max_combo"]
    assert uuid.UUID(score["id"])

    history = db_client.get(
        f"{helpers.API}/games/history", headers=user_account["headers"]
    )
    assert history.status_code == 200
    items = history.json()
    assert len(items) == 1
    item = items[0]
    assert item["session_id"] == session["id"]
    assert item["song_slug"] == published_song["slug"]
    assert item["status"] == "finished"
    assert item["score_points"] == VALID_SCORE["score_points"]
    assert item["max_combo"] == VALID_SCORE["max_combo"]
    assert item["finished_at"] is not None


def test_session_cannot_be_finished_twice(
    db_client, user_account, published_song
):
    session = _create_session(
        db_client, user_account["headers"], published_song["id"]
    )
    first = _finish_session(db_client, user_account["headers"], session["id"])
    assert first.status_code == 200

    second = _finish_session(db_client, user_account["headers"], session["id"])
    assert second.status_code == 409


def test_other_user_cannot_finish_foreign_session(
    db_client, user_account, second_user, published_song
):
    session = _create_session(
        db_client, user_account["headers"], published_song["id"]
    )
    response = _finish_session(
        db_client, second_user["headers"], session["id"]
    )
    assert response.status_code == 404


def test_history_requires_auth(db_client):
    response = db_client.get(f"{helpers.API}/games/history")
    assert response.status_code == 401


def test_global_and_song_leaderboards(
    db_client, user_account, second_user, published_song
):
    first_session = _create_session(
        db_client, user_account["headers"], published_song["id"]
    )
    assert (
        _finish_session(
            db_client, user_account["headers"], first_session["id"], score_points=2000
        ).status_code
        == 200
    )
    second_session = _create_session(
        db_client, second_user["headers"], published_song["id"]
    )
    assert (
        _finish_session(
            db_client, second_user["headers"], second_session["id"], score_points=500
        ).status_code
        == 200
    )

    global_board = db_client.get(f"{helpers.API}/leaderboard/global")
    assert global_board.status_code == 200
    body = global_board.json()
    assert body["limit"] == 10
    assert [item["username"] for item in body["items"]] == [
        user_account["username"],
        second_user["username"],
    ]
    assert body["items"][0]["rating_points"] == 2000
    assert body["items"][0]["games_played"] == 1
    assert body["items"][0]["rank"] == 1

    song_board = db_client.get(
        f"{helpers.API}/leaderboard", params={"song_slug": published_song["slug"]}
    )
    assert song_board.status_code == 200
    items = song_board.json()["items"]
    assert [item["username"] for item in items] == [
        user_account["username"],
        second_user["username"],
    ]
    assert items[0]["score_points"] == 2000
    assert items[0]["song_slug"] == published_song["slug"]


def _play_song(client, headers, song_id, score_points):
    """Finish one full session for ``song_id`` with the given points."""
    session = _create_session(client, headers, song_id)
    response = _finish_session(client, headers, session["id"], score_points=score_points)
    assert response.status_code == 200, response.text
    return response.json()


def _global_items(client):
    response = client.get(f"{helpers.API}/leaderboard/global")
    assert response.status_code == 200, response.text
    return {item["username"]: item for item in response.json()["items"]}


def test_global_rating_counts_only_best_attempt_per_song(
    db_client, user_account, published_song
):
    headers = user_account["headers"]
    for points in (1200, 900, 1200, 700):
        _play_song(db_client, headers, published_song["id"], points)

    player = _global_items(db_client)[user_account["username"]]
    assert player["rating_points"] == 1200
    assert player["games_played"] == 1


def test_global_rating_updates_when_song_record_is_beaten(
    db_client, user_account, published_song
):
    headers = user_account["headers"]
    _play_song(db_client, headers, published_song["id"], 800)
    _play_song(db_client, headers, published_song["id"], 300)
    assert _global_items(db_client)[user_account["username"]]["rating_points"] == 800

    _play_song(db_client, headers, published_song["id"], 1500)
    player = _global_items(db_client)[user_account["username"]]
    assert player["rating_points"] == 1500
    assert player["games_played"] == 1


def test_global_rating_sums_best_score_of_each_song(
    db_client, user_account, published_song, second_published_song
):
    headers = user_account["headers"]
    _play_song(db_client, headers, published_song["id"], 1000)
    _play_song(db_client, headers, published_song["id"], 600)
    _play_song(db_client, headers, second_published_song["id"], 400)
    _play_song(db_client, headers, second_published_song["id"], 1500)

    player = _global_items(db_client)[user_account["username"]]
    assert player["rating_points"] == 2500
    assert player["games_played"] == 2


def test_global_rating_keeps_users_and_songs_independent(
    db_client, user_account, second_user, published_song, second_published_song
):
    first_headers = user_account["headers"]
    _play_song(db_client, first_headers, published_song["id"], 900)
    _play_song(db_client, first_headers, second_published_song["id"], 700)
    _play_song(db_client, first_headers, published_song["id"], 500)

    second_headers = second_user["headers"]
    _play_song(db_client, second_headers, published_song["id"], 1200)
    _play_song(db_client, second_headers, second_published_song["id"], 100)
    _play_song(db_client, second_headers, second_published_song["id"], 1400)

    players = _global_items(db_client)
    assert players[user_account["username"]]["rating_points"] == 1600
    assert players[user_account["username"]]["games_played"] == 2
    assert players[second_user["username"]]["rating_points"] == 2600
    assert players[second_user["username"]]["games_played"] == 2

    board = db_client.get(f"{helpers.API}/leaderboard/global").json()["items"]
    assert [item["username"] for item in board] == [
        second_user["username"],
        user_account["username"],
    ]
    assert [item["rank"] for item in board] == [1, 2]


def test_best_scores_returns_per_song_best_and_rating(
    db_client, user_account, published_song, second_published_song
):
    headers = user_account["headers"]
    _play_song(db_client, headers, published_song["id"], 1200)
    _play_song(db_client, headers, published_song["id"], 900)
    _play_song(db_client, headers, second_published_song["id"], 500)

    response = db_client.get(f"{helpers.API}/games/best-scores", headers=headers)
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["rating_points"] == 1700
    assert body["songs_played"] == 2
    assert body["games_played"] == 3

    by_slug = {item["song_slug"]: item for item in body["items"]}
    first = by_slug[published_song["slug"]]
    assert first["best_score"] == 1200
    assert first["attempts"] == 2
    assert first["best_accuracy"] == pytest.approx(VALID_SCORE["accuracy_percent"])
    assert first["best_max_combo"] == VALID_SCORE["max_combo"]

    second = by_slug[second_published_song["slug"]]
    assert second["best_score"] == 500
    assert second["attempts"] == 1

    # The personal rating matches the public global leaderboard entry.
    global_board = db_client.get(f"{helpers.API}/leaderboard/global").json()
    entry = next(
        item
        for item in global_board["items"]
        if item["username"] == user_account["username"]
    )
    assert entry["rating_points"] == body["rating_points"]


def test_best_scores_requires_auth(db_client):
    response = db_client.get(f"{helpers.API}/games/best-scores")
    assert response.status_code == 401


def test_best_scores_are_isolated_per_user(
    db_client, user_account, second_user, published_song
):
    _play_song(db_client, user_account["headers"], published_song["id"], 1200)
    _play_song(db_client, second_user["headers"], published_song["id"], 400)

    first = db_client.get(
        f"{helpers.API}/games/best-scores", headers=user_account["headers"]
    ).json()
    second = db_client.get(
        f"{helpers.API}/games/best-scores", headers=second_user["headers"]
    ).json()

    assert first["rating_points"] == 1200
    assert second["rating_points"] == 400
    assert first["items"][0]["best_score"] == 1200
    assert second["items"][0]["best_score"] == 400


@pytest.mark.parametrize("params", [{"limit": 0}, {"limit": 101}, {"offset": -1}])
def test_leaderboard_validates_pagination(db_client, params):
    response = db_client.get(f"{helpers.API}/leaderboard/global", params=params)
    assert response.status_code == 422
