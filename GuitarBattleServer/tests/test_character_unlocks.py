"""Character ownership, server-side tower progress and reward unlocks.

Product rules pinned here:

* a new account owns exactly the starter characters (Ray + Luna) and nothing
  else;
* a locked character cannot be selected through ``PATCH /auth/me``;
* ``GET /characters`` marks ownership for anonymous and signed-in callers and
  describes the unlock condition;
* ``POST /campaigns/{id}/progress`` is monotonic, clamped to the published stage
  count, grants the tower's reward once on completion and never accepts a
  character id from the client.
"""
from __future__ import annotations

import uuid

import pytest
from sqlalchemy import select, update

from app.models.character_unlocks import UserCampaignProgress, UserCharacter
from app.models.gameplay_visuals import UiCharacter
from tests import helpers

pytestmark = pytest.mark.db

API = helpers.API
CHARACTERS_ADMIN = f"{API}/admin/ui-levels"
CAMPAIGNS_ADMIN = f"{API}/admin/campaigns"
PUBLIC_CHARACTER_KEYS = {
    "id",
    "name",
    "sheet_url",
    "unlocked",
    "unlock_campaign_id",
    "unlock_campaign_name",
    "unlock_condition",
}


def png_bytes(width: int = 64, height: int = 32) -> bytes:
    ihdr = (
        b"\x00\x00\x00\x0dIHDR"
        + width.to_bytes(4, "big")
        + height.to_bytes(4, "big")
        + b"\x08\x06\x00\x00\x00"
        + b"\x00\x00\x00\x00"
    )
    return b"\x89PNG\r\n\x1a\n" + ihdr


def create_character(client, headers, *, name: str) -> dict:
    response = client.post(
        CHARACTERS_ADMIN,
        data={"name": name},
        files={"file": ("sheet.png", png_bytes(), "image/png")},
        headers=headers,
    )
    assert response.status_code == 201, response.text
    return response.json()


def mark_starters(db_session, *names: str) -> None:
    db_session.execute(
        update(UiCharacter)
        .where(UiCharacter.name.in_(list(names)))
        .values(is_starter=True)
    )
    db_session.commit()


def create_campaign(
    client, admin_headers, *, song_count: int = 2, reward_character_id: str | None = None
) -> dict:
    if isinstance(admin_headers, dict) and "headers" in admin_headers:
        admin_headers = admin_headers["headers"]
    payload = {"name": "Tower " + uuid.uuid4().hex[:8], "bot_level": 1}
    if reward_character_id is not None:
        payload["reward_character_id"] = reward_character_id
    created = client.post(CAMPAIGNS_ADMIN, json=payload, headers=admin_headers)
    assert created.status_code == 201, created.text
    campaign = created.json()

    songs = []
    for index in range(song_count):
        song = helpers.create_song(
            client, admin_headers, title=f"Tower Song {index}"
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


def report(client, headers, campaign_id: str, best_stage: int):
    return client.post(
        f"{API}/campaigns/{campaign_id}/progress",
        json={"best_stage": best_stage},
        headers=headers,
    )


def character_payload(client, headers=None) -> dict[str, dict]:
    response = client.get(f"{API}/characters", headers=headers or {})
    assert response.status_code == 200, response.text
    return {item["name"]: item for item in response.json()}


# ---------------------------------------------------------------------------
# Registration and ownership
# ---------------------------------------------------------------------------
def test_new_account_owns_ray_and_luna_only(db_client, db_session, admin_user):
    create_character(db_client, admin_user["headers"], name="Ray")
    create_character(db_client, admin_user["headers"], name="Luna")
    create_character(db_client, admin_user["headers"], name="Jackson")
    mark_starters(db_session, "Ray", "Luna")

    account = helpers.register_user(db_client)

    owned = db_session.execute(
        select(UiCharacter.name)
        .join(UserCharacter, UserCharacter.character_id == UiCharacter.id)
        .where(UserCharacter.user_id == uuid.UUID(account["id"]))
    ).scalars().all()
    assert sorted(owned) == ["Luna", "Ray"]

    catalogue = character_payload(db_client, account["headers"])
    assert catalogue["Ray"]["unlocked"] is True
    assert catalogue["Luna"]["unlocked"] is True
    assert catalogue["Jackson"]["unlocked"] is False
    for item in catalogue.values():
        assert set(item) == PUBLIC_CHARACTER_KEYS


def test_anonymous_catalogue_unlocks_only_starters(db_client, db_session, admin_user):
    create_character(db_client, admin_user["headers"], name="Luna")
    create_character(db_client, admin_user["headers"], name="Jackson")
    mark_starters(db_session, "Luna")

    anonymous = character_payload(db_client)
    assert anonymous["Luna"]["unlocked"] is True
    assert anonymous["Jackson"]["unlocked"] is False
    assert anonymous["Jackson"]["unlock_condition"] is None


# ---------------------------------------------------------------------------
# PATCH /auth/me ownership enforcement
# ---------------------------------------------------------------------------
def test_patch_me_rejects_locked_and_accepts_owned_characters(
    db_client, db_session, admin_user, user_account
):
    ray = create_character(db_client, admin_user["headers"], name="Ray")
    luna = create_character(db_client, admin_user["headers"], name="Luna")
    jackson = create_character(db_client, admin_user["headers"], name="Jackson")
    mark_starters(db_session, "Ray", "Luna")

    # The account already owns the starters; grant the campaign reward directly
    # to cover the "granted character is selectable" path.
    db_session.execute(
        UserCharacter.__table__.insert().values(
            user_id=uuid.UUID(user_account["id"]),
            character_id=uuid.UUID(jackson["id"]),
        )
    )
    db_session.commit()

    catalogue = character_payload(db_client, user_account["headers"])
    for name in ("Ray", "Luna", "Jackson"):
        assert catalogue[name]["unlocked"] is True

    for name in ("Ray", "Luna", "Jackson"):
        response = db_client.patch(
            f"{API}/auth/me",
            json={"character_id": catalogue[name]["id"]},
            headers=user_account["headers"],
        )
        assert response.status_code == 200, response.text
        assert response.json()["character_name"] == name

    # A character that is neither starter nor granted stays locked.
    locked = create_character(db_client, admin_user["headers"], name="Vex")
    locked_public = character_payload(db_client, user_account["headers"])["Vex"]
    assert locked_public["unlocked"] is False
    response = db_client.patch(
        f"{API}/auth/me",
        json={"character_id": locked_public["id"]},
        headers=user_account["headers"],
    )
    assert response.status_code == 422, response.text
    assert response.json()["detail"] == "Character is locked"

    # Unknown ids keep their own error.
    unknown = db_client.patch(
        f"{API}/auth/me",
        json={"character_id": 987654321},
        headers=user_account["headers"],
    )
    assert unknown.status_code == 422
    assert unknown.json()["detail"] == "Unknown character_id"

    # Explicit null still clears the selection.
    cleared = db_client.patch(
        f"{API}/auth/me", json={"character_id": None}, headers=user_account["headers"]
    )
    assert cleared.status_code == 200
    assert cleared.json()["character_id"] is None

    # The fixtures created rows we did not use; keep flake-free references.
    assert ray["id"] and luna["id"]


# ---------------------------------------------------------------------------
# Campaign progress and rewards
# ---------------------------------------------------------------------------
def test_progress_is_monotonic_and_clamped(
    db_client, user_account, admin_user
):
    campaign = create_campaign(db_client, admin_user, song_count=2)

    first = report(db_client, user_account["headers"], campaign["id"], 1)
    assert first.status_code == 200, first.text
    body = first.json()
    assert body["campaign_id"] == campaign["id"]
    assert body["best_stage"] == 1
    assert body["stage_count"] == 2
    assert body["completed"] is False
    assert body["unlocked_character"] is None

    # A smaller report never decreases the stored best.
    lower = report(db_client, user_account["headers"], campaign["id"], 0)
    assert lower.status_code == 200, lower.text
    assert lower.json()["best_stage"] == 1
    assert lower.json()["completed"] is False

    # An over-large report is clamped to the published stage count.
    clamped = report(db_client, user_account["headers"], campaign["id"], 99)
    assert clamped.status_code == 200, clamped.text
    assert clamped.json()["best_stage"] == 2
    assert clamped.json()["completed"] is True

    progress = db_client.get(
        f"{API}/campaigns/progress", headers=user_account["headers"]
    )
    assert progress.status_code == 200, progress.text
    item = next(
        entry
        for entry in progress.json()["items"]
        if entry["campaign_id"] == campaign["id"]
    )
    assert item == {
        "campaign_id": campaign["id"],
        "best_stage": 2,
        "stage_count": 2,
        "completed": True,
    }


def test_progress_requires_authentication(db_client, admin_user):
    campaign = create_campaign(db_client, admin_user, song_count=1)
    assert (
        db_client.post(
            f"{API}/campaigns/{campaign['id']}/progress", json={"best_stage": 1}
        ).status_code
        == 401
    )
    assert db_client.get(f"{API}/campaigns/progress").status_code == 401


def test_reward_is_granted_only_after_the_final_stage_once(
    db_client, db_session, user_account, admin_user
):
    jackson = create_character(db_client, admin_user["headers"], name="Jackson")
    campaign = create_campaign(
        db_client,
        admin_user,
        song_count=2,
        reward_character_id=jackson["id"],
    )

    before = report(db_client, user_account["headers"], campaign["id"], 1)
    assert before.status_code == 200, before.text
    assert before.json()["completed"] is False
    assert before.json()["unlocked_character"] is None
    assert db_session.scalar(
        select(UserCharacter).where(
            UserCharacter.user_id == uuid.UUID(user_account["id"]),
            UserCharacter.character_id == uuid.UUID(jackson["id"]),
        )
    ) is None

    completed = report(db_client, user_account["headers"], campaign["id"], 2)
    assert completed.status_code == 200, completed.text
    body = completed.json()
    assert body["completed"] is True
    assert body["already_owned"] is False
    assert body["unlocked_character"]["name"] == "Jackson"
    assert body["unlocked_character"]["id"] > 0
    assert body["unlocked_character"]["sheet_url"].startswith("/media/characters/")

    row = db_session.scalar(
        select(UserCharacter).where(
            UserCharacter.user_id == uuid.UUID(user_account["id"]),
            UserCharacter.character_id == uuid.UUID(jackson["id"]),
        )
    )
    assert row is not None
    assert str(row.source_campaign_id) == campaign["id"]

    # And it is immediately selectable.
    catalogue = character_payload(db_client, user_account["headers"])
    assert catalogue["Jackson"]["unlocked"] is True
    selected = db_client.patch(
        f"{API}/auth/me",
        json={"character_id": catalogue["Jackson"]["id"]},
        headers=user_account["headers"],
    )
    assert selected.status_code == 200, selected.text

    # Replaying the completed tower grants nothing twice and reports it.
    replay = report(db_client, user_account["headers"], campaign["id"], 2)
    assert replay.status_code == 200, replay.text
    assert replay.json()["completed"] is True
    assert replay.json()["unlocked_character"] is None
    assert replay.json()["already_owned"] is True
    assert db_session.execute(
        select(UserCharacter).where(
            UserCharacter.user_id == uuid.UUID(user_account["id"]),
            UserCharacter.character_id == uuid.UUID(jackson["id"]),
        )
    ).scalars().all() == [row]


def test_already_owned_reward_is_not_reported_as_new(
    db_client, db_session, user_account, admin_user
):
    ray = create_character(db_client, admin_user["headers"], name="Ray")
    mark_starters(db_session, "Ray")
    campaign = create_campaign(
        db_client, admin_user, song_count=1, reward_character_id=ray["id"]
    )

    completed = report(db_client, user_account["headers"], campaign["id"], 1)
    assert completed.status_code == 200, completed.text
    body = completed.json()
    assert body["completed"] is True
    assert body["unlocked_character"] is None
    assert body["already_owned"] is True


def test_campaign_without_reward_completes_without_unlocks(
    db_client, db_session, user_account, admin_user
):
    campaign = create_campaign(db_client, admin_user, song_count=1)

    completed = report(db_client, user_account["headers"], campaign["id"], 1)
    assert completed.status_code == 200, completed.text
    body = completed.json()
    assert body["completed"] is True
    assert body["unlocked_character"] is None
    assert body["already_owned"] is False

    assert db_session.scalar(
        select(UserCampaignProgress).where(
            UserCampaignProgress.user_id == uuid.UUID(user_account["id"]),
            UserCampaignProgress.campaign_id == uuid.UUID(campaign["id"]),
        )
    ) is not None


def test_progress_rejects_a_character_id_from_the_client(db_client, admin_user):
    campaign = create_campaign(db_client, admin_user, song_count=1)
    response = db_client.post(
        f"{API}/campaigns/{campaign['id']}/progress",
        json={"best_stage": 1, "character_id": 1},
        headers={},
    )
    # Anonymous is 401 before validation; the important part is the schema.
    assert response.status_code in (401, 422)

    account = helpers.register_user(db_client)
    response = db_client.post(
        f"{API}/campaigns/{campaign['id']}/progress",
        json={"best_stage": 1, "character_id": 1},
        headers=account["headers"],
    )
    assert response.status_code == 422, response.text


def test_unknown_campaign_progress_is_404(db_client, user_account):
    response = report(db_client, user_account["headers"], str(uuid.uuid4()), 1)
    assert response.status_code == 404


# ---------------------------------------------------------------------------
# Admin reward configuration
# ---------------------------------------------------------------------------
def test_admin_saves_reward_and_public_payload_exposes_it(
    db_client, admin_user
):
    character = create_character(db_client, admin_user["headers"], name="Jackson")
    campaign = create_campaign(db_client, admin_user, song_count=1)

    patched = db_client.patch(
        f"{CAMPAIGNS_ADMIN}/{campaign['id']}",
        json={
            "reward_character_id": character["id"],
            "reward_condition": "  Complete the Jackson Tower  ",
        },
        headers=admin_user["headers"],
    )
    assert patched.status_code == 200, patched.text
    admin_body = patched.json()
    assert admin_body["reward_character_id"] == character["id"]
    assert admin_body["reward_character_name"] == "Jackson"
    assert admin_body["reward_condition"] == "Complete the Jackson Tower"

    # Reopening the editor returns the same values.
    detail = db_client.get(
        f"{CAMPAIGNS_ADMIN}/{campaign['id']}", headers=admin_user["headers"]
    ).json()
    assert detail["reward_character_id"] == character["id"]
    assert detail["reward_character_name"] == "Jackson"

    public = client_public(db_client, campaign["id"])
    assert public["reward_character_name"] == "Jackson"
    assert public["reward_condition"] == "Complete the Jackson Tower"
    jackson_public_id = character_payload(db_client)["Jackson"]["id"]
    assert isinstance(public["reward_character_id"], int)
    assert public["reward_character_id"] == jackson_public_id

    catalogue = character_payload(db_client)
    jackson = catalogue["Jackson"]
    assert jackson["unlock_campaign_id"] == campaign["id"]
    assert jackson["unlock_campaign_name"] == campaign["name"]
    assert jackson["unlock_condition"] == "Complete the Jackson Tower"

    # Clearing the reward removes the unlock description too.
    cleared = db_client.patch(
        f"{CAMPAIGNS_ADMIN}/{campaign['id']}",
        json={"reward_character_id": None, "reward_condition": None},
        headers=admin_user["headers"],
    )
    assert cleared.status_code == 200, cleared.text
    assert cleared.json()["reward_character_id"] is None
    assert cleared.json()["reward_character_name"] is None
    assert cleared.json()["reward_condition"] is None
    assert character_payload(db_client)["Jackson"]["unlock_condition"] is None


def client_public(client, campaign_id: str) -> dict:
    response = client.get(f"{API}/campaigns")
    assert response.status_code == 200, response.text
    return next(
        item for item in response.json()["items"] if item["id"] == campaign_id
    )


def test_derived_unlock_condition_without_custom_text(db_client, admin_user):
    character = create_character(db_client, admin_user["headers"], name="Luna")
    campaign = create_campaign(
        db_client, admin_user, song_count=1, reward_character_id=character["id"]
    )

    public = client_public(db_client, campaign["id"])
    assert public["reward_condition"] == f"Complete {campaign['name']}"
    assert character_payload(db_client)["Luna"]["unlock_condition"] == (
        f"Complete {campaign['name']}"
    )


def test_admin_rejects_unknown_reward_character_and_long_condition(
    db_client, admin_user
):
    campaign = create_campaign(db_client, admin_user, song_count=0)

    unknown = db_client.patch(
        f"{CAMPAIGNS_ADMIN}/{campaign['id']}",
        json={"reward_character_id": str(uuid.uuid4())},
        headers=admin_user["headers"],
    )
    assert unknown.status_code == 422, unknown.text
    assert "reward_character_id" in unknown.text

    too_long = db_client.patch(
        f"{CAMPAIGNS_ADMIN}/{campaign['id']}",
        json={"reward_condition": "x" * 201},
        headers=admin_user["headers"],
    )
    assert too_long.status_code == 422, too_long.text

    # Explicit null for NOT NULL metadata is still rejected.
    assert (
        db_client.patch(
            f"{CAMPAIGNS_ADMIN}/{campaign['id']}",
            json={"bot_level": None},
            headers=admin_user["headers"],
        ).status_code
        == 422
    )


def test_legacy_campaign_payloads_still_parse(db_client, admin_user):
    created = db_client.post(
        CAMPAIGNS_ADMIN,
        json={"name": "Legacy tower", "bot_level": 1},
        headers=admin_user["headers"],
    )
    assert created.status_code == 201, created.text
    body = created.json()
    assert body["reward_character_id"] is None
    assert body["reward_character_name"] is None
    assert body["reward_condition"] is None

    # Unknown fields stay rejected (extra="forbid").
    rejected = db_client.post(
        CAMPAIGNS_ADMIN,
        json={"name": "Extra tower", "bot_level": 1, "reward": "Jackson"},
        headers=admin_user["headers"],
    )
    assert rejected.status_code == 422
