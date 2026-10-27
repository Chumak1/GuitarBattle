"""Daily Challenge by weekday: schedule admin, catalogue and rating rules.

Covers the contract added by migration ``0009_daily_slots``:

* ``GET/PUT /admin/daily`` — admin-only weekly schedule;
* ``songs.daily_weekday`` — the same schedule from the song side;
* ``/daily/today`` serves the scheduled song of the UTC weekday and returns
  ``song: null`` for a free weekday instead of an error;
* daily songs leave the ordinary catalogue, the global rating and the
  personal-best list, while their assets stay downloadable by slug;
* ``/daily/history`` — the player's official attempts with the day rank.
"""
from __future__ import annotations

import pytest

from app.services.progression import utc_today
from tests import helpers

SCORE = {
    "score_points": 1000,
    "accuracy_percent": 90.0,
    "max_combo": 10,
    "perfect_count": 8,
    "good_count": 2,
    "miss_count": 0,
}


def _create_published(db_client, admin_user, **overrides):
    song = helpers.create_song(db_client, admin_user["headers"], **overrides)
    updated = helpers.publish_song(db_client, admin_user["headers"], song["id"])
    return {**song, **updated}


def _start(client, headers, song_id, *, daily=False):
    return client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": song_id, "speed_percent": 100, "daily": daily},
        headers=headers,
    )


def _finish(client, headers, session_id, **overrides):
    return client.post(
        f"{helpers.API}/games/sessions/{session_id}/finish",
        json={**SCORE, **overrides},
        headers=headers,
    )


def _play(client, headers, song_id, **overrides):
    session = _start(client, headers, song_id)
    assert session.status_code == 201, session.text
    response = _finish(client, headers, session.json()["id"], **overrides)
    assert response.status_code == 200, response.text
    return response.json()


def _today(client, headers):
    response = client.get(f"{helpers.API}/daily/today", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


# ---------------------------------------------------------------------------
# Admin schedule
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_admin_daily_requires_admin(db_client, user_account, published_song):
    assert db_client.get(f"{helpers.API}/admin/daily").status_code == 401
    assert (
        db_client.get(
            f"{helpers.API}/admin/daily", headers=user_account["headers"]
        ).status_code
        == 403
    )
    assert (
        db_client.put(
            f"{helpers.API}/admin/daily/0",
            json={"song_id": published_song["id"]},
            headers=user_account["headers"],
        ).status_code
        == 403
    )


@pytest.mark.db
def test_admin_daily_schedule_has_seven_weekdays(db_client, admin_user):
    items = helpers.daily_schedule(db_client, admin_user["headers"])
    assert [item["weekday"] for item in items] == list(range(7))
    assert items[0]["weekday_name"] == "Понедельник"
    assert items[6]["weekday_name"] == "Воскресенье"
    assert all(item["song"] is None for item in items)


@pytest.mark.db
def test_admin_daily_assign_and_clear(db_client, admin_user, published_song):
    slot = helpers.schedule_daily(
        db_client, admin_user["headers"], 2, published_song["id"]
    )
    assert slot["weekday"] == 2
    assert slot["song"]["id"] == published_song["id"]
    assert slot["song"]["slug"] == published_song["slug"]
    assert slot["song"]["is_published"] is True

    items = helpers.daily_schedule(db_client, admin_user["headers"])
    assert items[2]["song"]["id"] == published_song["id"]
    assert [item["song"] for item in items if item["weekday"] != 2] == [None] * 6

    cleared = helpers.schedule_daily(db_client, admin_user["headers"], 2, None)
    assert cleared["song"] is None
    assert all(
        item["song"] is None
        for item in helpers.daily_schedule(db_client, admin_user["headers"])
    )

    # The song marker is cleared together with the slot.
    detail = db_client.get(
        f"{helpers.API}/admin/songs/{published_song['id']}",
        headers=admin_user["headers"],
    )
    assert detail.status_code == 200
    assert detail.json()["daily_weekday"] is None


@pytest.mark.db
def test_admin_daily_moving_a_song_clears_the_old_slot(
    db_client, admin_user, published_song
):
    helpers.schedule_daily(db_client, admin_user["headers"], 1, published_song["id"])
    helpers.schedule_daily(db_client, admin_user["headers"], 3, published_song["id"])

    items = helpers.daily_schedule(db_client, admin_user["headers"])
    assert items[1]["song"] is None
    assert items[3]["song"]["id"] == published_song["id"]
    assert (
        db_client.get(
            f"{helpers.API}/admin/songs/{published_song['id']}",
            headers=admin_user["headers"],
        ).json()["daily_weekday"]
        == 3
    )


@pytest.mark.db
def test_admin_daily_replacing_a_slot_returns_the_previous_song_to_catalog(
    db_client, admin_user, published_song, second_published_song
):
    helpers.schedule_daily(db_client, admin_user["headers"], 4, published_song["id"])
    slot = helpers.schedule_daily(
        db_client, admin_user["headers"], 4, second_published_song["id"]
    )
    assert slot["song"]["id"] == second_published_song["id"]

    previous = db_client.get(
        f"{helpers.API}/admin/songs/{published_song['id']}",
        headers=admin_user["headers"],
    ).json()
    assert previous["daily_weekday"] is None
    catalogue = db_client.get(f"{helpers.API}/songs?limit=100").json()
    assert published_song["id"] in {item["id"] for item in catalogue["items"]}
    assert second_published_song["id"] not in {
        item["id"] for item in catalogue["items"]
    }


@pytest.mark.db
def test_admin_daily_validates_song_and_weekday(db_client, admin_user):
    missing = db_client.put(
        f"{helpers.API}/admin/daily/0",
        json={"song_id": "00000000-0000-0000-0000-000000000000"},
        headers=admin_user["headers"],
    )
    assert missing.status_code == 404

    bad_weekday = db_client.put(
        f"{helpers.API}/admin/daily/7",
        json={"song_id": None},
        headers=admin_user["headers"],
    )
    assert bad_weekday.status_code == 422


@pytest.mark.db
def test_song_daily_weekday_field_drives_the_schedule(
    db_client, admin_user, published_song
):
    headers = admin_user["headers"]
    updated = db_client.patch(
        f"{helpers.API}/admin/songs/{published_song['id']}",
        json={"daily_weekday": 5},
        headers=headers,
    )
    assert updated.status_code == 200, updated.text
    assert updated.json()["daily_weekday"] == 5

    items = helpers.daily_schedule(db_client, headers)
    assert items[5]["song"]["id"] == published_song["id"]

    cleared = db_client.patch(
        f"{helpers.API}/admin/songs/{published_song['id']}",
        json={"daily_weekday": None},
        headers=headers,
    )
    assert cleared.status_code == 200, cleared.text
    assert cleared.json()["daily_weekday"] is None
    assert all(item["song"] is None for item in helpers.daily_schedule(db_client, headers))


@pytest.mark.db
def test_song_daily_weekday_out_of_range_is_rejected(
    db_client, admin_user, published_song
):
    response = db_client.patch(
        f"{helpers.API}/admin/songs/{published_song['id']}",
        json={"daily_weekday": 9},
        headers=admin_user["headers"],
    )
    assert response.status_code == 422


# ---------------------------------------------------------------------------
# /daily/today
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_daily_today_without_slot_is_a_valid_empty_answer(db_client, user_account):
    body = _today(db_client, user_account["headers"])
    assert body["song"] is None
    assert body["weekday"] == utc_today().weekday()
    assert body["date"] == utc_today().isoformat()
    assert body["rules_version"] == 1
    assert body["official_attempt_used"] is False
    assert body["official_result"] is None
    assert body["leaderboard_top"] == []


@pytest.mark.db
def test_daily_today_serves_the_scheduled_weekday_song(
    db_client, user_account, today_daily_slot, published_song
):
    body = _today(db_client, user_account["headers"])
    assert body["weekday"] == utc_today().weekday()
    assert body["song"]["id"] == published_song["id"]
    assert body["song"]["slug"] == published_song["slug"]


@pytest.mark.db
def test_daily_today_skips_an_unpublished_slot_song(db_client, admin_user, user_account):
    song = helpers.create_song(db_client, admin_user["headers"])
    today = utc_today()
    helpers.schedule_daily(
        db_client, admin_user["headers"], today.weekday(), song["id"]
    )

    body = _today(db_client, user_account["headers"])
    assert body["song"] is None

    # A draft cannot be played at all…
    assert (
        _start(db_client, user_account["headers"], song["id"], daily=True).status_code
        == 404
    )
    # …and no other song pretends to be today's challenge while the slot is
    # occupied by an unpublished song.
    other = _create_published(db_client, admin_user)
    response = _start(db_client, user_account["headers"], other["id"], daily=True)
    assert response.status_code == 409
    assert "daily challenge" in response.json()["detail"].lower()


@pytest.mark.db
def test_daily_challenge_is_frozen_for_the_day(
    db_client, admin_user, user_account, published_song, second_published_song
):
    """Changing the slot must not rewrite the challenge of a started day."""
    headers = admin_user["headers"]
    weekday = utc_today().weekday()
    helpers.schedule_daily(db_client, headers, weekday, published_song["id"])
    first = _today(db_client, user_account["headers"])
    assert first["song"]["id"] == published_song["id"]

    helpers.schedule_daily(db_client, headers, weekday, second_published_song["id"])
    frozen = _today(db_client, user_account["headers"])
    assert frozen["song"]["id"] == published_song["id"]

    assert (
        _start(
            db_client, user_account["headers"], second_published_song["id"], daily=True
        ).status_code
        == 409
    )


@pytest.mark.db
def test_daily_session_without_slot_is_409(db_client, user_account, published_song):
    response = _start(db_client, user_account["headers"], published_song["id"], daily=True)
    assert response.status_code == 409
    assert "daily challenge" in response.json()["detail"].lower()


# ---------------------------------------------------------------------------
# Catalogue, rating and personal bests
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_catalog_excludes_daily_song_but_assets_stay_reachable(
    db_client, admin_user, second_published_song
):
    headers = admin_user["headers"]
    helpers.schedule_daily(db_client, headers, 2, second_published_song["id"])

    catalogue = db_client.get(f"{helpers.API}/songs?limit=100").json()
    assert second_published_song["id"] not in {
        item["id"] for item in catalogue["items"]
    }

    detail = db_client.get(f"{helpers.API}/songs/{second_published_song['slug']}")
    assert detail.status_code == 200
    assert detail.json()["id"] == second_published_song["id"]


@pytest.mark.db
def test_global_rating_and_best_scores_ignore_daily_songs(
    db_client, admin_user, user_account, second_user, published_song
):
    headers = admin_user["headers"]
    # The score is played while the song is still an ordinary catalogue entry…
    _play(db_client, user_account["headers"], published_song["id"], score_points=1500)
    # …and only then scheduled as a daily song.
    helpers.schedule_daily(db_client, headers, 1, published_song["id"])

    best = db_client.get(
        f"{helpers.API}/games/best-scores", headers=user_account["headers"]
    )
    assert best.status_code == 200
    assert best.json()["items"] == []
    assert best.json()["rating_points"] == 0

    rating = db_client.get(f"{helpers.API}/leaderboard/global")
    assert rating.status_code == 200
    assert rating.json()["items"] == []


@pytest.mark.db
def test_daily_song_still_allows_practice_sessions(
    db_client, admin_user, user_account, published_song
):
    helpers.schedule_daily(db_client, admin_user["headers"], 0, published_song["id"])
    session = _start(db_client, user_account["headers"], published_song["id"])
    assert session.status_code == 201
    assert session.json()["mode"] == "normal"
    finish = _finish(db_client, user_account["headers"], session.json()["id"])
    assert finish.status_code == 200

    board = db_client.get(f"{helpers.API}/daily/leaderboard", headers=user_account["headers"])
    assert board.json()["total"] == 0


# ---------------------------------------------------------------------------
# /daily/history
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_daily_history_lists_official_results(
    db_client, user_account, second_user, published_song, today_daily_slot
):
    headers = user_account["headers"]
    today = _today(db_client, headers)
    session = _start(db_client, headers, today["song"]["id"], daily=True)
    assert session.status_code == 201
    assert _finish(db_client, headers, session.json()["id"], score_points=1500).status_code == 200

    other = second_user["headers"]
    other_session = _start(db_client, other, today["song"]["id"], daily=True)
    assert _finish(db_client, other, other_session.json()["id"], score_points=100).status_code == 200

    history = db_client.get(f"{helpers.API}/daily/history", headers=headers)
    assert history.status_code == 200
    body = history.json()
    assert body["total"] == 1
    item = body["items"][0]
    assert item["date"] == today["date"]
    assert item["song_id"] == published_song["id"]
    assert item["song_slug"] == published_song["slug"]
    assert item["score_points"] == 1500
    assert item["rank"] == 1
    assert item["participants"] == 2

    other_history = db_client.get(f"{helpers.API}/daily/history", headers=other).json()
    assert other_history["items"][0]["rank"] == 2

    assert db_client.get(f"{helpers.API}/daily/history").status_code == 401
    assert (
        db_client.get(f"{helpers.API}/daily/history?limit=0", headers=headers).status_code
        == 422
    )


@pytest.mark.db
def test_daily_history_is_empty_without_attempts(db_client, user_account):
    body = db_client.get(
        f"{helpers.API}/daily/history", headers=user_account["headers"]
    ).json()
    assert body == {"items": [], "total": 0, "limit": 20, "offset": 0}
