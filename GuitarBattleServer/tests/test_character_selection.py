"""Player character selection: public catalogue, profile choice and duel state.

These tests use the same HTTP contract as the desktop client:

* ``GET /api/v1/characters`` — public, bare JSON array, integer ``id``;
* ``GET/PATCH /api/v1/auth/me`` — ``character_id`` / ``character_name`` /
  ``character_sheet_url``;
* ``DuelMatchState`` — ``you_*`` / ``opponent_*`` character fields;
* the spritesheet is actually served from ``/media/characters/``.
"""
from __future__ import annotations

import uuid

import pytest

from app.models.character_unlocks import UserCharacter
from app.models.user import User
from tests import helpers

pytestmark = pytest.mark.db

API = helpers.API
CHARACTERS_ADMIN = f"{API}/admin/ui-levels"
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
    """PNG header with the requested IHDR dimensions (4x2 spritesheet)."""
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


def create_duel(client, headers, song_id: str, *, mode: str = "bot") -> dict:
    payload = {"song_id": song_id, "mode": mode}
    if mode == "bot":
        payload["bot_level"] = "easy"
    response = client.post(f"{API}/duels", json=payload, headers=headers)
    assert response.status_code == 201, response.text
    return response.json()


def choose_character(client, headers, character_id: int) -> dict:
    response = client.patch(
        f"{API}/auth/me", json={"character_id": character_id}, headers=headers
    )
    assert response.status_code == 200, response.text
    return response.json()


def grant_character(db_session, user: dict, character: dict) -> None:
    """Put a non-starter character into the collection without a tower.

    Ownership is enforced server-side, so tests that only need a chosen
    spritesheet grant it directly instead of playing a whole campaign.
    """
    db_session.execute(
        UserCharacter.__table__.insert().values(
            user_id=uuid.UUID(user["id"]),
            character_id=uuid.UUID(character["id"]),
        )
    )
    db_session.commit()


# ---------------------------------------------------------------------------
# Public catalogue
# ---------------------------------------------------------------------------
def test_public_character_catalogue_needs_no_auth_and_is_sorted(
    db_client, admin_user
):
    # Empty list is a valid response.
    empty = db_client.get(f"{API}/characters")
    assert empty.status_code == 200
    assert empty.json() == []

    zed = create_character(db_client, admin_user["headers"], name="Zed")
    alice = create_character(db_client, admin_user["headers"], name="Alice")

    response = db_client.get(f"{API}/characters")  # no Authorization header
    assert response.status_code == 200, response.text
    body = response.json()
    assert [item["name"] for item in body] == ["Alice", "Zed"]
    assert all(isinstance(item["id"], int) for item in body)
    assert body[0]["id"] != body[1]["id"]
    for item in body:
        assert set(item) == PUBLIC_CHARACTER_KEYS
        assert item["sheet_url"].startswith("/media/characters/")
        assert item["sheet_url"].endswith(".png")
        # Anonymous requests see only starters as unlocked; these two are not.
        assert item["unlocked"] is False

    # The administrative responses keep their internal UUID ``id`` unchanged.
    admin_listing = db_client.get(
        f"{API}/admin/ui-levels", headers=admin_user["headers"]
    )
    admin_ids = {item["id"] for item in admin_listing.json()["items"]}
    assert admin_ids == {alice["id"], zed["id"]}
    assert not admin_ids & {str(item["id"]) for item in body}


def test_character_sheet_is_served_by_the_media_mount(db_client, admin_user):
    character = create_character(db_client, admin_user["headers"], name="Bard")

    response = db_client.get(character["sheet_url"])
    assert response.status_code == 200, response.text
    assert response.headers["content-type"] == "image/png"
    assert response.content.startswith(b"\x89PNG\r\n\x1a\n")


# ---------------------------------------------------------------------------
# Profile choice
# ---------------------------------------------------------------------------
def test_me_character_fields_and_patch_set_clear_unknown(
    db_client, user_account, admin_user, db_session
):
    character = create_character(db_client, admin_user["headers"], name="Bard")
    grant_character(db_session, user_account, character)
    public_id = db_client.get(f"{API}/characters").json()[0]["id"]

    profile = db_client.get(f"{API}/auth/me", headers=user_account["headers"])
    assert profile.status_code == 200
    assert profile.json()["character_id"] is None
    assert profile.json()["character_name"] is None
    assert profile.json()["character_sheet_url"] is None

    selected = choose_character(
        db_client, user_account["headers"], public_id
    )
    assert selected["character_id"] == public_id
    assert selected["character_name"] == "Bard"
    assert selected["character_sheet_url"] == character["sheet_url"]

    # The choice survives a fresh read.
    again = db_client.get(f"{API}/auth/me", headers=user_account["headers"])
    assert again.json()["character_id"] == public_id
    assert again.json()["character_name"] == "Bard"

    # Explicit null clears it.
    cleared = choose_character(db_client, user_account["headers"], None)
    assert cleared["character_id"] is None
    assert cleared["character_name"] is None
    assert cleared["character_sheet_url"] is None

    # Unknown id is a validation error, not a silent no-op.
    unknown = db_client.patch(
        f"{API}/auth/me", json={"character_id": 987654321}, headers=user_account["headers"]
    )
    assert unknown.status_code == 422
    assert unknown.json()["detail"] == "Unknown character_id"

    # Omitting the field keeps the current selection (and still allows the
    # existing display-name update).
    choose_character(db_client, user_account["headers"], public_id)
    renamed = db_client.patch(
        f"{API}/auth/me",
        json={"display_name": "Renamed"},
        headers=user_account["headers"],
    )
    assert renamed.status_code == 200, renamed.text
    assert renamed.json()["character_id"] == public_id
    assert renamed.json()["display_name"] == "Renamed"


def test_patch_me_rejects_malformed_character_id(db_client, user_account):
    response = db_client.patch(
        f"{API}/auth/me", json={"character_id": 0}, headers=user_account["headers"]
    )
    assert response.status_code == 422

    response = db_client.patch(
        f"{API}/auth/me", json={"character_id": "abc"}, headers=user_account["headers"]
    )
    assert response.status_code == 422


# ---------------------------------------------------------------------------
# Duels
# ---------------------------------------------------------------------------
def test_duel_state_exposes_both_players_characters(
    db_client, user_account, second_user, published_song, admin_user, db_session
):
    first_character = create_character(
        db_client, admin_user["headers"], name="Bard"
    )
    second_character = create_character(
        db_client, admin_user["headers"], name="Bluesman"
    )
    grant_character(db_session, user_account, first_character)
    grant_character(db_session, second_user, second_character)
    catalogue = db_client.get(f"{API}/characters").json()
    ids = {item["name"]: item["id"] for item in catalogue}
    choose_character(db_client, user_account["headers"], ids["Bard"])
    choose_character(db_client, second_user["headers"], ids["Bluesman"])

    first = create_duel(
        db_client, user_account["headers"], published_song["id"], mode="online"
    )
    match_id = first["match_id"]
    assert first["you_character_name"] == "Bard"
    assert first["you_character_sheet_url"] == first_character["sheet_url"]
    # No opponent has joined yet.
    assert first["opponent_character_name"] is None
    assert first["opponent_character_sheet_url"] is None

    joined = create_duel(
        db_client, second_user["headers"], published_song["id"], mode="online"
    )
    assert joined["match_id"] == match_id
    assert joined["your_side"] == "opponent"
    assert joined["you_character_name"] == "Bluesman"
    assert joined["opponent_character_name"] == "Bard"

    # The REST snapshot is per-viewer, like ``your_side``.
    state = db_client.get(
        f"{API}/duels/{match_id}", headers=user_account["headers"]
    )
    assert state.status_code == 200, state.text
    body = state.json()
    assert body["your_side"] == "player"
    assert body["you_character_name"] == "Bard"
    assert body["you_character_sheet_url"] == first_character["sheet_url"]
    assert body["opponent_character_name"] == "Bluesman"
    assert body["opponent_character_sheet_url"] == second_character["sheet_url"]

    # Existing participant fields are untouched.
    assert {item["side"] for item in body["participants"]} == {
        "player",
        "opponent",
    }


def test_duel_character_fields_are_null_without_a_selection(
    db_client, user_account, published_song
):
    match = create_duel(db_client, user_account["headers"], published_song["id"])
    assert match["you_character_name"] is None
    assert match["you_character_sheet_url"] is None
    assert match["opponent_character_name"] is None
    assert match["opponent_character_sheet_url"] is None

# ---------------------------------------------------------------------------
# New-account default character
# ---------------------------------------------------------------------------
def test_new_account_defaults_to_ray(db_client, db_session, admin_user):
    ray = create_character(db_client, admin_user["headers"], name="Ray")

    account = helpers.register_user(db_client)

    stored = db_session.get(User, uuid.UUID(account["id"]))
    assert stored is not None
    assert str(stored.character_id) == ray["id"]

    profile = db_client.get(f"{API}/auth/me", headers=account["headers"])
    assert profile.status_code == 200, profile.text
    body = profile.json()

    public = db_client.get(f"{API}/characters").json()
    ray_public = next(item for item in public if item["name"] == "Ray")
    assert body["character_id"] == ray_public["id"]
    assert body["character_name"] == "Ray"
    assert body["character_sheet_url"] == ray["sheet_url"]


def test_new_account_without_ray_keeps_no_character(db_client, db_session):
    account = helpers.register_user(db_client)

    stored = db_session.get(User, uuid.UUID(account["id"]))
    assert stored is not None
    assert stored.character_id is None

    profile = db_client.get(f"{API}/auth/me", headers=account["headers"])
    assert profile.status_code == 200, profile.text
    body = profile.json()
    assert body["character_id"] is None
    assert body["character_name"] is None
    assert body["character_sheet_url"] is None
