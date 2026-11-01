"""Admin draft preview: unpublished songs stay private, admins may play them.

Contract under test (additive, backwards compatible):

* ``UserResponse.is_admin`` — the desktop client learns from ``/auth/me`` that
  the signed-in account may use the admin-only tools;
* ``SongSummaryResponse.is_published`` — the catalogue row can show a DRAFT
  badge; older clients ignore the field;
* ``GET /songs?include_drafts=1`` returns drafts **only** for a valid admin
  token; anonymous and non-admin requests keep the published catalogue;
* ``GET /songs/{slug}`` and the file download serve an unpublished song to an
  administrator only; archived songs stay unreachable even for an admin;
* ``POST /games/sessions`` for a draft creates ``mode="preview"``: the score is
  stored as history but awards no XP/achievements, writes no personal best and
  never reaches the rating or the best-scores block.
"""
from __future__ import annotations

import uuid

import pytest

from tests import helpers

pytestmark = pytest.mark.db

SCORE = {
    "score_points": 1200,
    "accuracy_percent": 97.5,
    "max_combo": 42,
    "perfect_count": 40,
    "good_count": 2,
    "miss_count": 1,
}


# ---------------------------------------------------------------------------
# Local transport helpers
# ---------------------------------------------------------------------------
def _catalogue(client, *, params=None, headers=None):
    response = client.get(f"{helpers.API}/songs", params=params, headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _start(client, headers, song_id, *, daily=False):
    return client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": song_id, "speed_percent": 100, "daily": daily},
        headers=headers,
    )


def _finish(client, headers, session_id, payload=SCORE):
    return client.post(
        f"{helpers.API}/games/sessions/{session_id}/finish",
        json=payload,
        headers=headers,
    )


def _upload_midi(client, admin_headers, song_id):
    response = client.post(
        f"{helpers.API}/admin/songs/{song_id}/files/midi",
        files={"file": ("draft.mid", helpers.build_midi_bytes(), "audio/midi")},
        headers=admin_headers,
    )
    assert response.status_code == 201, response.text
    return response.json()


def _best_scores(client, headers):
    response = client.get(f"{helpers.API}/games/best-scores", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _global_rating(client, username):
    response = client.get(f"{helpers.API}/leaderboard/global?limit=100")
    assert response.status_code == 200, response.text
    items = {item["username"]: item for item in response.json()["items"]}
    return items.get(username)


# ---------------------------------------------------------------------------
# is_admin in the profile
# ---------------------------------------------------------------------------
def test_register_and_me_report_is_admin(db_client, user_account):
    assert user_account["register_response"]["is_admin"] is False

    me = db_client.get(f"{helpers.API}/auth/me", headers=user_account["headers"])
    assert me.status_code == 200, me.text
    assert me.json()["is_admin"] is False


def test_admin_profile_reports_is_admin(db_client, admin_user):
    me = db_client.get(f"{helpers.API}/auth/me", headers=admin_user["headers"])
    assert me.status_code == 200, me.text
    assert me.json()["is_admin"] is True


# ---------------------------------------------------------------------------
# Catalogue
# ---------------------------------------------------------------------------
def test_drafts_are_listed_only_for_admin_with_include_drafts(
    db_client, admin_user, user_account, draft_song, published_song
):
    # Anonymous: the flag is ignored, the catalogue stays public.
    anonymous = _catalogue(db_client, params={"include_drafts": 1, "limit": 100})
    assert {item["slug"] for item in anonymous["items"]} == {published_song["slug"]}
    assert anonymous["total"] == 1

    # Non-admin token: the flag is ignored as well.
    non_admin = _catalogue(
        db_client,
        params={"include_drafts": 1, "limit": 100},
        headers=user_account["headers"],
    )
    assert {item["slug"] for item in non_admin["items"]} == {published_song["slug"]}

    # Admin without the flag: unchanged published catalogue.
    admin_default = _catalogue(
        db_client, params={"limit": 100}, headers=admin_user["headers"]
    )
    assert {item["slug"] for item in admin_default["items"]} == {
        published_song["slug"]
    }

    # Admin with the flag: drafts appear next to the published songs.
    admin_drafts = _catalogue(
        db_client,
        params={"include_drafts": 1, "limit": 100},
        headers=admin_user["headers"],
    )
    assert admin_drafts["total"] == 2
    by_slug = {item["slug"]: item for item in admin_drafts["items"]}
    assert by_slug[draft_song["slug"]]["is_published"] is False
    assert by_slug[published_song["slug"]]["is_published"] is True


def test_draft_is_not_returned_by_an_invalid_token(
    db_client, draft_song, published_song
):
    response = db_client.get(
        f"{helpers.API}/songs",
        params={"include_drafts": 1, "limit": 100},
        headers=helpers.auth_headers("not-a-real-token"),
    )
    # A broken token must not turn a public read into an error.
    assert response.status_code == 200, response.text
    assert {item["slug"] for item in response.json()["items"]} == {
        published_song["slug"]
    }


# ---------------------------------------------------------------------------
# Detail and files
# ---------------------------------------------------------------------------
def test_draft_detail_requires_admin(db_client, admin_user, user_account, draft_song):
    assert (
        db_client.get(f"{helpers.API}/songs/{draft_song['slug']}").status_code == 404
    )
    assert (
        db_client.get(
            f"{helpers.API}/songs/{draft_song['slug']}",
            headers=user_account["headers"],
        ).status_code
        == 404
    )

    response = db_client.get(
        f"{helpers.API}/songs/{draft_song['slug']}", headers=admin_user["headers"]
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["id"] == draft_song["id"]
    assert body["is_published"] is False


def test_draft_file_download_requires_admin(
    db_client, admin_user, user_account, draft_song
):
    midi = _upload_midi(db_client, admin_user["headers"], draft_song["id"])

    detail = db_client.get(
        f"{helpers.API}/songs/{draft_song['slug']}", headers=admin_user["headers"]
    ).json()
    assert detail["assets"]["midi"]["id"] == midi["id"]
    url = detail["assets"]["midi"]["download_url"]
    assert url == (
        f"{helpers.API}/songs/{draft_song['slug']}/files/{midi['id']}/download"
    )

    assert db_client.get(url).status_code == 404
    assert (
        db_client.get(url, headers=user_account["headers"]).status_code == 404
    )

    response = db_client.get(url, headers=admin_user["headers"])
    assert response.status_code == 200, response.text
    assert response.content.startswith(b"MThd")


def test_archived_song_stays_hidden_from_admin(
    db_client, admin_user, draft_song
):
    midi = _upload_midi(db_client, admin_user["headers"], draft_song["id"])
    archived = db_client.delete(
        f"{helpers.API}/admin/songs/{draft_song['id']}", headers=admin_user["headers"]
    )
    assert archived.status_code == 204, archived.text

    listing = _catalogue(
        db_client,
        params={"include_drafts": 1, "limit": 100},
        headers=admin_user["headers"],
    )
    assert listing["items"] == []

    assert (
        db_client.get(
            f"{helpers.API}/songs/{draft_song['slug']}",
            headers=admin_user["headers"],
        ).status_code
        == 404
    )
    assert (
        db_client.get(
            f"{helpers.API}/songs/{draft_song['slug']}/files/{midi['id']}/download",
            headers=admin_user["headers"],
        ).status_code
        == 404
    )


# ---------------------------------------------------------------------------
# Preview sessions
# ---------------------------------------------------------------------------
def test_draft_session_is_rejected_for_non_admin(
    db_client, user_account, draft_song
):
    response = _start(db_client, user_account["headers"], draft_song["id"])
    assert response.status_code == 404, response.text


def test_admin_draft_session_is_a_preview(db_client, admin_user, draft_song):
    response = _start(db_client, admin_user["headers"], draft_song["id"])
    assert response.status_code == 201, response.text
    body = response.json()
    assert body["mode"] == "preview"
    assert body["status"] == "started"
    assert body["song_id"] == draft_song["id"]


def test_admin_daily_session_for_a_draft_is_rejected(
    db_client, admin_user, draft_song
):
    response = _start(
        db_client, admin_user["headers"], draft_song["id"], daily=True
    )
    assert response.status_code == 404, response.text


def test_admin_plays_a_published_song_as_normal(
    db_client, admin_user, published_song
):
    response = _start(db_client, admin_user["headers"], published_song["id"])
    assert response.status_code == 201, response.text
    assert response.json()["mode"] == "normal"


def test_preview_finish_awards_nothing_and_is_not_ranked(
    db_client, admin_user, draft_song
):
    session = _start(db_client, admin_user["headers"], draft_song["id"]).json()
    response = _finish(db_client, admin_user["headers"], session["id"])
    assert response.status_code == 200, response.text
    body = response.json()

    # The score itself is stored (history), the progression block is empty.
    assert body["session_id"] == session["id"]
    assert body["score_points"] == SCORE["score_points"]
    assert uuid.UUID(body["id"])
    assert body["xp_earned"] == 0
    assert body["level_up"] is False
    assert body["personal_best_updated"] is False
    assert body["mastery"] is None
    assert body["achievements_awarded"] == []

    # No personal best, no rating row, no best-scores entry.
    best = _best_scores(db_client, admin_user["headers"])
    assert best["rating_points"] == 0
    assert best["items"] == []
    assert _global_rating(db_client, admin_user["username"]) is None

    personal = db_client.get(
        f"{helpers.API}/profile/personal-bests", headers=admin_user["headers"]
    )
    assert personal.status_code == 200, personal.text
    assert personal.json()["items"] == []

    progression = db_client.get(
        f"{helpers.API}/profile/progression", headers=admin_user["headers"]
    )
    assert progression.status_code == 200, progression.text
    assert progression.json()["xp_total"] == 0

    # History still shows the attempt, so the player sees what they played.
    history = db_client.get(
        f"{helpers.API}/games/history", headers=admin_user["headers"]
    )
    assert history.status_code == 200, history.text
    assert [item["session_id"] for item in history.json()] == [session["id"]]


def test_preview_does_not_change_an_existing_rating(
    db_client, admin_user, draft_song, published_song
):
    normal = _start(db_client, admin_user["headers"], published_song["id"]).json()
    first = _finish(
        db_client, admin_user["headers"], normal["id"], {**SCORE, "score_points": 800}
    )
    assert first.status_code == 200, first.text
    assert first.json()["xp_earned"] > 0

    preview = _start(db_client, admin_user["headers"], draft_song["id"]).json()
    second = _finish(
        db_client,
        admin_user["headers"],
        preview["id"],
        {**SCORE, "score_points": 99_999, "accuracy_percent": 100.0},
    )
    assert second.status_code == 200, second.text
    assert second.json()["xp_earned"] == 0

    best = _best_scores(db_client, admin_user["headers"])
    assert best["rating_points"] == 800
    assert [item["song_slug"] for item in best["items"]] == [
        published_song["slug"]
    ]

    player = _global_rating(db_client, admin_user["username"])
    assert player is not None
    assert player["rating_points"] == 800
    assert player["games_played"] == 1

    # The per-song leaderboard is public-only, so a draft never appears there.
    song_board = db_client.get(
        f"{helpers.API}/leaderboard", params={"song_slug": draft_song["slug"]}
    )
    assert song_board.status_code == 200
    assert song_board.json()["items"] == []
