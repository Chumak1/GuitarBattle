"""Battle ("Tower riffs") run position and ``POST /campaigns/{id}/reset``.

``user_campaign_progress.best_stage`` is a cumulative counter. The stage the
player is currently on is ``best_stage % stage_count``, so every read path has
to report that value instead of ``min(best_stage, stage_count)``. Clamping
destroys the modulo: a 3-stage tower at ``best_stage=4`` is on stage 1, not
"finished".

The rules pinned here:

* a completed tower reports ``best_stage == stage_count`` and ``completed``;
* winning stage 0 of a finished tower starts the next run and the response (and
  ``GET /campaigns/progress``) reports ``1``, not the old completion;
* ``POST /campaigns/{id}/reset`` starts a new run on the server: raw
  ``best_stage`` becomes ``0`` while ``completed_at``/``completion_count`` are
  history and survive; it is idempotent and never grants XP or a reward.
"""
from __future__ import annotations

import uuid

import pytest
from sqlalchemy import select

from app.models.character_unlocks import UserCampaignProgress
from tests import helpers

pytestmark = pytest.mark.db

API = helpers.API
CAMPAIGNS_ADMIN = f"{API}/admin/campaigns"


# ---------------------------------------------------------------------------
# Helpers (same HTTP contract the client uses)
# ---------------------------------------------------------------------------
def create_campaign(client, admin_headers, *, song_count: int) -> dict:
    if isinstance(admin_headers, dict) and "headers" in admin_headers:
        admin_headers = admin_headers["headers"]

    created = client.post(
        CAMPAIGNS_ADMIN,
        json={"name": "Reset tower " + uuid.uuid4().hex[:8], "bot_level": 1},
        headers=admin_headers,
    )
    assert created.status_code == 201, created.text
    campaign = created.json()

    songs = []
    for index in range(song_count):
        song = helpers.create_song(
            client, admin_headers, title=f"Reset Song {index}"
        )
        helpers.publish_song(client, admin_headers, song["id"])
        songs.append(song)

    updated = client.put(
        f"{CAMPAIGNS_ADMIN}/{campaign['id']}/songs",
        json={"songs": [{"song_id": song["id"]} for song in songs]},
        headers=admin_headers,
    )
    assert updated.status_code == 200, updated.text
    return updated.json()


def stage_result(
    client,
    headers,
    campaign_id: str,
    position: int,
    *,
    won: bool = True,
):
    payload = {
        "run_id": str(uuid.uuid4()),
        "won": won,
        "score_points": 400,
        "accuracy_percent": 90.0,
        "perfect_count": 90,
        "great_count": 0,
        "good_count": 0,
        "miss_count": 0,
        "completion_percent": 100.0,
        "notes_total": 100,
        "notes_played": 100,
    }
    return client.post(
        f"{API}/campaigns/{campaign_id}/stages/{position}/result",
        json=payload,
        headers=headers,
    )


def reset(client, headers, campaign_id: str):
    return client.post(
        f"{API}/campaigns/{campaign_id}/reset", headers=headers
    )


def campaign_progress(client, headers, campaign_id: str) -> dict:
    response = client.get(f"{API}/campaigns/progress", headers=headers)
    assert response.status_code == 200, response.text
    return next(
        item
        for item in response.json()["items"]
        if item["campaign_id"] == campaign_id
    )


def stored_progress(db_session, user_id: str, campaign_id: str):
    return db_session.scalar(
        select(UserCampaignProgress).where(
            UserCampaignProgress.user_id == uuid.UUID(user_id),
            UserCampaignProgress.campaign_id == uuid.UUID(campaign_id),
        )
    )


# ---------------------------------------------------------------------------
# Reported bug: a replayed finished tower must report stage 1, not "finished"
# ---------------------------------------------------------------------------
def test_completed_two_stage_tower_reports_stage_count(
    db_client, admin_user, user_account
):
    campaign = create_campaign(db_client, admin_user, song_count=2)
    headers = user_account["headers"]

    first = stage_result(db_client, headers, campaign["id"], 0)
    assert first.status_code == 200, first.text
    assert first.json()["best_stage"] == 1

    second = stage_result(db_client, headers, campaign["id"], 1)
    assert second.status_code == 200, second.text
    body = second.json()
    assert body["best_stage"] == 2
    assert body["completed"] is True

    assert campaign_progress(db_client, headers, campaign["id"]) == {
        "campaign_id": campaign["id"],
        "best_stage": 2,
        "stage_count": 2,
        "completed": True,
    }


def test_replay_after_completion_reports_stage_one(db_client, admin_user, user_account):
    """The reported bug: raw=3 of a 2-stage tower is stage 0 of a new run."""
    campaign = create_campaign(db_client, admin_user, song_count=2)
    headers = user_account["headers"]

    for position in (0, 1):
        won = stage_result(db_client, headers, campaign["id"], position)
        assert won.status_code == 200, won.text
    assert won.json()["best_stage"] == 2
    assert won.json()["completed"] is True

    replay = stage_result(db_client, headers, campaign["id"], 0)
    assert replay.status_code == 200, replay.text
    assert replay.json()["won"] is True
    # Raw counter is 3, so the current run is on stage 1 (3 % 2). It must not
    # be clamped to 2, which the client would read as "BATTLE COMPLETE".
    assert replay.json()["best_stage"] == 1

    progress = campaign_progress(db_client, headers, campaign["id"])
    assert progress["best_stage"] == 1
    assert progress["stage_count"] == 2
    # Completion history is kept independently of the position.
    assert progress["completed"] is True


# ---------------------------------------------------------------------------
# Reset
# ---------------------------------------------------------------------------
def test_reset_keeps_history_and_is_idempotent(
    db_client, db_session, admin_user, user_account
):
    campaign = create_campaign(db_client, admin_user, song_count=2)
    headers = user_account["headers"]

    for position in (0, 1):
        won = stage_result(db_client, headers, campaign["id"], position)
        assert won.status_code == 200, won.text

    before = stored_progress(db_session, user_account["id"], campaign["id"])
    assert before is not None
    assert before.completion_count == 1
    assert before.completed_at is not None

    response = reset(db_client, headers, campaign["id"])
    assert response.status_code == 200, response.text
    assert response.json() == {
        "campaign_id": campaign["id"],
        "best_stage": 0,
        "stage_count": 2,
        "completed": True,
        "unlocked_character": None,
        "already_owned": False,
    }

    db_session.expire_all()
    after = stored_progress(db_session, user_account["id"], campaign["id"])
    assert after is not None
    assert after.best_stage == 0
    # History/anti-farm data survives the reset.
    assert after.completion_count == 1
    assert after.completed_at is not None

    assert campaign_progress(db_client, headers, campaign["id"]) == {
        "campaign_id": campaign["id"],
        "best_stage": 0,
        "stage_count": 2,
        "completed": True,
    }

    # A second reset is a no-op (same answer, same stored history).
    again = reset(db_client, headers, campaign["id"])
    assert again.status_code == 200, again.text
    assert again.json() == response.json()
    db_session.expire_all()
    stored = stored_progress(db_session, user_account["id"], campaign["id"])
    assert stored is not None
    assert stored.best_stage == 0
    assert stored.completion_count == 1


def test_after_reset_the_next_run_starts_at_stage_zero(
    db_client, db_session, admin_user, user_account
):
    campaign = create_campaign(db_client, admin_user, song_count=2)
    headers = user_account["headers"]

    for position in (0, 1):
        assert (
            stage_result(db_client, headers, campaign["id"], position).status_code
            == 200
        )

    assert reset(db_client, headers, campaign["id"]).status_code == 200

    first = stage_result(db_client, headers, campaign["id"], 0)
    assert first.status_code == 200, first.text
    assert first.json()["best_stage"] == 1

    # Stage 1 is the current stage now: no 409 stage_not_current.
    second = stage_result(db_client, headers, campaign["id"], 1)
    assert second.status_code == 200, second.text
    assert second.json()["best_stage"] == 2
    assert second.json()["completed"] is True

    db_session.expire_all()
    stored = stored_progress(db_session, user_account["id"], campaign["id"])
    assert stored is not None
    assert stored.best_stage == 2
    assert stored.completion_count == 2


def test_mid_run_reset_restarts_the_tower(
    db_client, db_session, admin_user, user_account
):
    campaign = create_campaign(db_client, admin_user, song_count=3)
    headers = user_account["headers"]

    first = stage_result(db_client, headers, campaign["id"], 0)
    assert first.status_code == 200, first.text
    assert first.json()["best_stage"] == 1

    second = stage_result(db_client, headers, campaign["id"], 1)
    assert second.status_code == 200, second.text
    assert second.json()["best_stage"] == 2

    response = reset(db_client, headers, campaign["id"])
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["best_stage"] == 0
    assert body["stage_count"] == 3
    assert body["completed"] is False
    assert body["unlocked_character"] is None

    db_session.expire_all()
    stored = stored_progress(db_session, user_account["id"], campaign["id"])
    assert stored is not None
    assert stored.best_stage == 0
    assert stored.completion_count == 0
    assert stored.completed_at is None

    # Stage 0 is accepted; stage 1 is still ahead (not skipped).
    replay = stage_result(db_client, headers, campaign["id"], 0)
    assert replay.status_code == 200, replay.text
    assert replay.json()["best_stage"] == 1

    progress = campaign_progress(db_client, headers, campaign["id"])
    assert progress["best_stage"] == 1
    assert progress["completed"] is False


def test_reset_requires_auth_and_known_campaign(
    db_client, db_session, admin_user, user_account
):
    campaign = create_campaign(db_client, admin_user, song_count=1)

    unknown = reset(db_client, user_account["headers"], str(uuid.uuid4()))
    assert unknown.status_code == 404, unknown.text

    anonymous = reset(db_client, {}, campaign["id"])
    assert anonymous.status_code == 401, anonymous.text

    # A player who never played the tower gets no progress row; repeated
    # resets stay idempotent.
    first = reset(db_client, user_account["headers"], campaign["id"])
    second = reset(db_client, user_account["headers"], campaign["id"])
    assert first.status_code == 200, first.text
    assert second.status_code == 200, second.text
    assert first.json() == second.json()
    assert first.json()["best_stage"] == 0
    assert first.json()["completed"] is False
    assert first.json()["stage_count"] == 1
    assert stored_progress(db_session, user_account["id"], campaign["id"]) is None
