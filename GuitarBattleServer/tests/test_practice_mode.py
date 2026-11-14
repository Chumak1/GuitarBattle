"""Practice section: a non-competitive attempt, enforced by the server.

Contract under test (additive, backwards compatible):

* ``POST /games/sessions`` accepts ``practice: bool`` (default ``False``) and
  derives ``mode``: unpublished draft -> ``preview``; published + ``practice``
  -> ``practice``; ``daily`` keeps ``daily``; otherwise ``normal``.
* ``daily: true`` together with ``practice: true`` is ambiguous -> ``422``.
* ``finish`` on a ``practice`` session stores the ``scores`` row for audit but
  awards no XP, no achievements, no personal best, no daily result and no
  rating contribution. The response reports ``xp_earned=0`` and the unchanged
  level.
* ``practice`` attempts stay out of every user-facing competitive aggregate:
  ``/games/history``, ``/games/best-scores``, the global rating, the per-song
  leaderboard and the profile statistics.
* ``normal`` and ``daily`` behaviour is untouched.
"""
from __future__ import annotations

import uuid

import pytest

from app.models.game_session import GameSession, Score
from sqlalchemy import func, select
from tests import helpers

pytestmark = pytest.mark.db

SCORE = {
    "score_points": 2500,
    "accuracy_percent": 99.0,
    "max_combo": 60,
    "perfect_count": 58,
    "good_count": 2,
    "miss_count": 0,
}


def _start(client, headers, song_id, *, daily=False, practice=False):
    return client.post(
        f"{helpers.API}/games/sessions",
        json={
            "song_id": song_id,
            "speed_percent": 100,
            "daily": daily,
            "practice": practice,
        },
        headers=headers,
    )


def _finish(client, headers, session_id, **overrides):
    return client.post(
        f"{helpers.API}/games/sessions/{session_id}/finish",
        json={**SCORE, **overrides},
        headers=headers,
    )


def _practice_attempt(client, headers, song_id, **overrides):
    session = _start(client, headers, song_id, practice=True)
    assert session.status_code == 201, session.text
    response = _finish(client, headers, session.json()["id"], **overrides)
    assert response.status_code == 200, response.text
    return session.json(), response.json()


def _normal_attempt(client, headers, song_id):
    session = _start(client, headers, song_id)
    assert session.status_code == 201, session.text
    response = _finish(client, headers, session.json()["id"])
    assert response.status_code == 200, response.text
    return session.json(), response.json()


def _profile(client, headers):
    response = client.get(f"{helpers.API}/profile/progression", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _best_scores(client, headers):
    response = client.get(f"{helpers.API}/games/best-scores", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _history(client, headers):
    response = client.get(f"{helpers.API}/games/history", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _global_entry(client, username):
    response = client.get(f"{helpers.API}/leaderboard/global?limit=100")
    assert response.status_code == 200, response.text
    items = {item["username"]: item for item in response.json()["items"]}
    return items.get(username)


# ---------------------------------------------------------------------------
# Session mode precedence
# ---------------------------------------------------------------------------
def test_practice_session_mode_is_practice(db_client, user_account, published_song):
    session = _start(
        db_client, user_account["headers"], published_song["id"], practice=True
    )
    assert session.status_code == 201, session.text
    assert session.json()["mode"] == "practice"


def test_practice_and_daily_are_ambiguous(
    db_client, user_account, published_song, today_daily_slot
):
    response = _start(
        db_client,
        user_account["headers"],
        published_song["id"],
        daily=True,
        practice=True,
    )
    assert response.status_code == 422, response.text


def test_practice_on_unpublished_draft_stays_preview(
    db_client, admin_user, draft_song
):
    session = _start(
        db_client, admin_user["headers"], draft_song["id"], practice=True
    )
    assert session.status_code == 201, session.text
    assert session.json()["mode"] == "preview"


def test_practice_flag_is_optional_for_old_clients(db_client, user_account, published_song):
    response = db_client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": published_song["id"], "speed_percent": 100},
        headers=user_account["headers"],
    )
    assert response.status_code == 201, response.text
    assert response.json()["mode"] == "normal"


# ---------------------------------------------------------------------------
# Finishing a practice attempt awards nothing
# ---------------------------------------------------------------------------
def test_practice_finish_awards_no_rewards(db_client, user_account, published_song):
    headers = user_account["headers"]
    before = _profile(db_client, headers)

    session, body = _practice_attempt(db_client, headers, published_song["id"])

    # The score itself is returned so the client can show the attempt stats.
    assert body["session_id"] == session["id"]
    assert body["score_points"] == SCORE["score_points"]
    assert body["accuracy_percent"] == pytest.approx(SCORE["accuracy_percent"])

    # No progression at all.
    assert body["xp_earned"] == 0
    assert body["xp_total"] == before["xp_total"]
    assert body["level"] == before["level"]
    assert body["level_up"] is False
    assert body["personal_best_updated"] is False
    assert body["mastery"] is None
    assert body["achievements_awarded"] == []

    after = _profile(db_client, headers)
    assert after["xp_total"] == before["xp_total"]
    assert after["level"] == before["level"]
    assert after["songs_mastered"] == 0
    assert after["personal_bests"] == []
    assert after["mastery_by_song"] == []
    assert after["stats"]["samples"] == 0

    # The rating and the per-song bests stay empty.
    best = _best_scores(db_client, headers)
    assert best["rating_points"] == 0
    assert best["items"] == []
    assert _global_entry(db_client, user_account["username"]) is None

    # Competitive history hides the practice run.
    assert _history(db_client, headers) == []

    # The per-song leaderboard hides it too.
    board = db_client.get(
        f"{helpers.API}/leaderboard", params={"song_slug": published_song["slug"]}
    )
    assert board.status_code == 200, board.text
    assert board.json()["items"] == []


def test_practice_score_row_is_kept_for_audit(
    db_client, user_account, published_song, db_session
):
    session, _ = _practice_attempt(
        db_client, user_account["headers"], published_song["id"]
    )
    row = db_session.get(GameSession, uuid.UUID(session["id"]))
    assert row is not None
    assert row.mode == "practice"
    assert row.status == "finished"
    assert db_session.scalar(
        select(func.count()).select_from(Score).where(Score.session_id == row.id)
    ) == 1


def test_direct_api_calls_cannot_farm_rewards_with_practice(
    db_client, user_account, published_song
):
    headers = user_account["headers"]
    for _ in range(3):
        _, body = _practice_attempt(db_client, headers, published_song["id"])
        assert body["xp_earned"] == 0

    assert _profile(db_client, headers)["xp_total"] == 0
    assert _best_scores(db_client, headers)["rating_points"] == 0
    assert _history(db_client, headers) == []


# ---------------------------------------------------------------------------
# Other modes keep awarding
# ---------------------------------------------------------------------------
def test_normal_session_still_awards_rewards(db_client, user_account, published_song):
    headers = user_account["headers"]
    _, body = _normal_attempt(db_client, headers, published_song["id"])

    assert body["xp_earned"] > 0
    assert body["personal_best_updated"] is True
    assert _profile(db_client, headers)["songs_mastered"] == 1
    assert len(_history(db_client, headers)) == 1


def test_daily_session_still_awards_rewards(
    db_client, user_account, published_song, today_daily_slot
):
    headers = user_account["headers"]
    session = _start(
        db_client, headers, published_song["id"], daily=True
    )
    assert session.status_code == 201, session.text
    assert session.json()["mode"] == "daily"

    response = _finish(db_client, headers, session.json()["id"])
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["xp_earned"] > 0
    assert body["personal_best_updated"] is True

    history = db_client.get(f"{helpers.API}/daily/history", headers=headers)
    assert history.status_code == 200, history.text
    assert history.json()["total"] == 1


def test_practice_does_not_consume_the_daily_attempt(
    db_client, user_account, published_song, today_daily_slot
):
    headers = user_account["headers"]

    # A practice run on the daily song is not the official attempt.
    _, body = _practice_attempt(db_client, headers, published_song["id"])
    assert body["xp_earned"] == 0
    assert _history(db_client, headers) == []

    official = _start(db_client, headers, published_song["id"], daily=True)
    assert official.status_code == 201, official.text
    assert official.json()["mode"] == "daily"
