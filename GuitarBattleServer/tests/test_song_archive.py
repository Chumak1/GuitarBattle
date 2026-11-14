"""Regression tests: hiding or removing a song never loses player results.

Contract under test (migration ``0012_song_archive``):

* ``DELETE /admin/songs/{id}`` archives by default: the row and every result
  attached to its stable ``id`` stay in the database;
* an archived song disappears from the public catalogue/detail/download and
  cannot start new sessions, but the player's best score, mastery, XP,
  achievements, attempt history and Daily Challenge results are untouched;
* the global rating keeps counting results of hidden songs exactly once;
* a physical delete is possible only for a song that has no results at all,
  and the database ``RESTRICT`` foreign keys enforce that even off the API.
"""
from __future__ import annotations

import uuid

import pytest
from sqlalchemy import delete, func, select
from sqlalchemy.exc import IntegrityError

from app.models.game_session import GameSession
from app.models.progression import (
    DailyChallenge,
    DailyResult,
    SongBestScore,
    UserAchievement,
)
from app.models.song import Song
from app.models.user import User
from app.services.progression import utc_today
from tests import helpers

pytestmark = pytest.mark.db

SCORE = {
    "score_points": 1200,
    "accuracy_percent": 96.0,
    "max_combo": 40,
    "perfect_count": 38,
    "good_count": 2,
    "miss_count": 0,
}
DAILY_SCORE = {**SCORE, "score_points": 1800, "accuracy_percent": 98.0}


# ---------------------------------------------------------------------------
# Local transport helpers
# ---------------------------------------------------------------------------
def _start(client, headers, song_id, *, daily=False):
    return client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": song_id, "speed_percent": 100, "daily": daily},
        headers=headers,
    )


def _finish(client, headers, session_id, payload):
    return client.post(
        f"{helpers.API}/games/sessions/{session_id}/finish",
        json=payload,
        headers=headers,
    )


def _play(client, headers, song_id, payload=SCORE, *, daily=False):
    session = _start(client, headers, song_id, daily=daily)
    assert session.status_code == 201, session.text
    response = _finish(client, headers, session.json()["id"], payload)
    assert response.status_code == 200, response.text
    return response.json()


def _profile(client, headers):
    response = client.get(f"{helpers.API}/profile/progression", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _archive(client, admin_headers, song_id):
    response = client.delete(
        f"{helpers.API}/admin/songs/{song_id}", headers=admin_headers
    )
    assert response.status_code == 204, response.text


def _catalogue_ids(client):
    response = client.get(f"{helpers.API}/songs?limit=100")
    assert response.status_code == 200, response.text
    return {item["id"] for item in response.json()["items"]}


def _global_rating(client, username):
    response = client.get(f"{helpers.API}/leaderboard/global?limit=100")
    assert response.status_code == 200, response.text
    items = {item["username"]: item for item in response.json()["items"]}
    return items.get(username)


def _best_scores(client, headers):
    response = client.get(f"{helpers.API}/games/best-scores", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _publish(client, admin_headers, **overrides):
    song = helpers.create_song(client, admin_headers, **overrides)
    updated = helpers.publish_song(client, admin_headers, song["id"])
    return {**song, **updated}


def _user_row(db_session, account):
    return db_session.get(User, uuid.UUID(account["id"]))


# ---------------------------------------------------------------------------
# (a) Archiving keeps every result, aggregate and achievement
# ---------------------------------------------------------------------------
def test_archive_keeps_best_score_mastery_xp_and_daily_results(
    db_client, db_session, admin_user, user_account
):
    song = _publish(db_client, admin_user["headers"], title="Archive Me")
    # Today's Daily Challenge slot: the official attempt is played before the
    # song is archived, so the archive must not touch that history.
    helpers.schedule_daily(
        db_client, admin_user["headers"], utc_today().weekday(), song["id"]
    )
    daily_body = _play(
        db_client, user_account["headers"], song["id"], DAILY_SCORE, daily=True
    )
    assert daily_body["xp_earned"] > 0

    headers = user_account["headers"]
    before_profile = _profile(db_client, headers)
    before_rating = _global_rating(db_client, user_account["username"])
    before_xp = int(_user_row(db_session, user_account).xp_total)
    before_achievements = int(
        db_session.scalar(select(func.count()).select_from(UserAchievement)) or 0
    )
    before_history = db_client.get(f"{helpers.API}/daily/history", headers=headers)
    assert before_history.status_code == 200, before_history.text
    assert before_history.json()["total"] == 1

    _archive(db_client, admin_user["headers"], song["id"])

    # The song row survives with the same id, only hidden.
    row = db_session.get(Song, uuid.UUID(song["id"]))
    db_session.refresh(row)
    assert row is not None
    assert row.archived_at is not None
    assert row.is_published is False

    # User results are still in the database…
    assert (
        db_session.scalar(
            select(func.count())
            .select_from(SongBestScore)
            .where(SongBestScore.song_id == row.id)
        )
        == 1
    )
    assert (
        db_session.scalar(
            select(func.count())
            .select_from(GameSession)
            .where(
                GameSession.song_id == row.id,
                GameSession.status == "finished",
            )
        )
        == 1
    )
    assert (
        db_session.scalar(
            select(func.count())
            .select_from(DailyResult)
            .where(DailyResult.user_id == uuid.UUID(user_account["id"]))
        )
        == 1
    )
    # …and so is the frozen challenge (its song_id is RESTRICT).
    challenge = db_session.get(DailyChallenge, utc_today())
    assert challenge is not None
    assert challenge.song_id == row.id

    # XP, achievements and the derived profile are unchanged.
    db_session.expire_all()
    assert int(_user_row(db_session, user_account).xp_total) == before_xp
    assert (
        int(db_session.scalar(select(func.count()).select_from(UserAchievement)) or 0)
        == before_achievements
    )

    after_profile = _profile(db_client, headers)
    assert after_profile["xp_total"] == before_xp
    assert after_profile["songs_mastered"] == before_profile["songs_mastered"]
    assert after_profile["genre_mastery"] == before_profile["genre_mastery"]
    assert after_profile["mastery_by_song"][0]["song_id"] == song["id"]
    assert after_profile["personal_bests"][0]["best_score"] == DAILY_SCORE["score_points"]

    # The rating is identical — archiving a song never rewrites the ranked
    # history that was already earned, and nothing was duplicated. The official
    # daily attempt is a ranked solo event, so the player keeps a Guitar Skills
    # entry.
    after_rating = _global_rating(db_client, user_account["username"])
    assert after_rating == before_rating
    assert after_rating is not None
    assert after_rating["events_counted"] >= 1

    # Daily history stays readable after the song left the catalogue.
    after_history = db_client.get(f"{helpers.API}/daily/history", headers=headers)
    assert after_history.status_code == 200, after_history.text
    assert after_history.json()["total"] == 1
    assert after_history.json()["items"][0]["song_slug"] == song["slug"]

    # Archiving also removes the song from the daily rotation, so no new
    # official attempt can be created for a song whose assets are gone.
    schedule = {
        item["weekday"]: item["song"]
        for item in helpers.daily_schedule(db_client, admin_user["headers"])
    }
    assert schedule[utc_today().weekday()] is None


# ---------------------------------------------------------------------------
# (b) Hidden in the catalogue, still counted for the player
# ---------------------------------------------------------------------------
def test_archived_song_is_hidden_but_result_and_rating_remain(
    db_client, admin_user, user_account
):
    song = _publish(db_client, admin_user["headers"], title="Hidden Soon")
    _play(db_client, user_account["headers"], song["id"], SCORE)
    assert song["id"] in _catalogue_ids(db_client)
    assert _global_rating(db_client, user_account["username"])["rating_points"] == 1200

    _archive(db_client, admin_user["headers"], song["id"])

    assert song["id"] not in _catalogue_ids(db_client)
    detail = db_client.get(f"{helpers.API}/songs/{song['slug']}")
    assert detail.status_code == 404

    # A new session for the hidden song is refused…
    blocked = _start(db_client, user_account["headers"], song["id"])
    assert blocked.status_code == 404

    # …but the stored result still feeds the player's rating and best scores.
    rating = _global_rating(db_client, user_account["username"])
    assert rating["rating_points"] == 1200
    assert rating["games_played"] == 1

    best = _best_scores(db_client, user_account["headers"])
    assert best["rating_points"] == 1200
    assert [item["song_id"] for item in best["items"]] == [song["id"]]

    page = db_client.get(
        f"{helpers.API}/profile/personal-bests", headers=user_account["headers"]
    )
    assert page.status_code == 200, page.text
    assert page.json()["total"] == 1
    assert page.json()["items"][0]["best_score"] == 1200


def test_unpublish_keeps_rating_and_mastery(db_client, admin_user, user_account):
    song = _publish(db_client, admin_user["headers"], title="Unpublished")
    _play(db_client, user_account["headers"], song["id"], SCORE)

    response = db_client.post(
        f"{helpers.API}/admin/songs/{song['id']}/unpublish",
        headers=admin_user["headers"],
    )
    assert response.status_code == 200

    assert song["id"] not in _catalogue_ids(db_client)
    assert db_client.get(f"{helpers.API}/songs/{song['slug']}").status_code == 404

    rating = _global_rating(db_client, user_account["username"])
    assert rating["rating_points"] == 1200
    assert rating["games_played"] == 1
    assert _profile(db_client, user_account["headers"])["songs_mastered"] == 1


# ---------------------------------------------------------------------------
# (c) Physical deletion is guarded by user results
# ---------------------------------------------------------------------------
def test_permanent_delete_is_refused_while_results_exist(
    db_client, db_session, admin_user, user_account
):
    song = _publish(db_client, admin_user["headers"], title="Never Delete")
    _play(db_client, user_account["headers"], song["id"], SCORE)

    response = db_client.delete(
        f"{helpers.API}/admin/songs/{song['id']}?permanent=true",
        headers=admin_user["headers"],
    )
    assert response.status_code == 409, response.text

    # Nothing was removed and the song was not even archived by the failure.
    row = db_session.get(Song, uuid.UUID(song["id"]))
    db_session.refresh(row)
    assert row is not None
    assert row.archived_at is None
    assert (
        db_session.scalar(
            select(func.count())
            .select_from(SongBestScore)
            .where(SongBestScore.song_id == row.id)
        )
        == 1
    )
    assert (
        db_session.scalar(
            select(func.count())
            .select_from(GameSession)
            .where(GameSession.song_id == row.id)
        )
        == 1
    )


def test_database_refuses_to_cascade_user_results(
    db_client, db_session, admin_user, user_account
):
    """Even a raw ``DELETE FROM songs`` cannot wipe a personal best."""
    song = _publish(db_client, admin_user["headers"], title="FK Guard")
    _play(db_client, user_account["headers"], song["id"], SCORE)
    db_session.commit()

    with pytest.raises(IntegrityError):
        db_session.execute(
            delete(Song.__table__).where(Song.id == uuid.UUID(song["id"]))
        )
        db_session.commit()
    db_session.rollback()

    assert db_session.get(Song, uuid.UUID(song["id"])) is not None
    assert (
        db_session.scalar(
            select(func.count())
            .select_from(SongBestScore)
            .where(SongBestScore.song_id == uuid.UUID(song["id"]))
        )
        == 1
    )


def test_permanent_delete_of_a_never_played_song_still_works(
    db_client, db_session, admin_user
):
    song = helpers.create_song(db_client, admin_user["headers"])

    response = db_client.delete(
        f"{helpers.API}/admin/songs/{song['id']}?permanent=true",
        headers=admin_user["headers"],
    )
    assert response.status_code == 204, response.text
    assert db_session.get(Song, uuid.UUID(song["id"])) is None
    assert (
        db_client.get(
            f"{helpers.API}/admin/songs/{song['id']}", headers=admin_user["headers"]
        ).status_code
        == 404
    )


# ---------------------------------------------------------------------------
# (d) Hiding/restoring never duplicates rating points
# ---------------------------------------------------------------------------
def test_rating_is_stable_across_archive_and_restore(
    db_client, admin_user, user_account
):
    first = _publish(db_client, admin_user["headers"], title="Rating One")
    second = _publish(db_client, admin_user["headers"], title="Rating Two")
    _play(db_client, user_account["headers"], first["id"], {**SCORE, "score_points": 1000})
    _play(db_client, user_account["headers"], second["id"], {**SCORE, "score_points": 500})

    assert _global_rating(db_client, user_account["username"])["rating_points"] == 1500

    _archive(db_client, admin_user["headers"], first["id"])
    hidden = _global_rating(db_client, user_account["username"])
    assert hidden["rating_points"] == 1500
    assert hidden["games_played"] == 2

    # A second read (and a second archive call) must not change the number.
    _archive(db_client, admin_user["headers"], first["id"])
    assert _global_rating(db_client, user_account["username"])["rating_points"] == 1500
    assert _best_scores(db_client, user_account["headers"])["rating_points"] == 1500

    restored = db_client.post(
        f"{helpers.API}/admin/songs/{first['id']}/restore",
        headers=admin_user["headers"],
    )
    assert restored.status_code == 200, restored.text
    assert restored.json()["archived_at"] is None
    assert restored.json()["is_published"] is False

    after_restore = _global_rating(db_client, user_account["username"])
    assert after_restore["rating_points"] == 1500
    assert after_restore["games_played"] == 2


def test_admin_list_shows_status_and_archive_filter(
    db_client, admin_user, user_account
):
    song = _publish(db_client, admin_user["headers"], title="Listed")
    _play(db_client, user_account["headers"], song["id"], SCORE)
    _archive(db_client, admin_user["headers"], song["id"])

    detail = db_client.get(
        f"{helpers.API}/admin/songs/{song['id']}", headers=admin_user["headers"]
    )
    assert detail.status_code == 200, detail.text
    assert detail.json()["archived_at"] is not None

    archived = db_client.get(
        f"{helpers.API}/admin/songs",
        params={"archived": "true", "limit": 100},
        headers=admin_user["headers"],
    )
    assert archived.status_code == 200, archived.text
    assert [item["id"] for item in archived.json()["items"]] == [song["id"]]

    active = db_client.get(
        f"{helpers.API}/admin/songs",
        params={"archived": "false", "limit": 100},
        headers=admin_user["headers"],
    )
    assert active.status_code == 200, active.text
    assert song["id"] not in {item["id"] for item in active.json()["items"]}
