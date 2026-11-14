"""Gameplay visuals: UI levels (characters), stage levels and the song choice.

The tests exercise the same HTTP contract the admin SPA and the JUCE client
use: multipart uploads, standalone stage creation (name + background only),
the per-song PATCH fields and the additive public catalogue fields.
"""
import uuid

import pytest

from tests import helpers

pytestmark = pytest.mark.db

CHARACTERS_PATH = f"{helpers.API}/admin/ui-levels"
STAGES_PATH = f"{helpers.API}/admin/stage-levels"


def png_bytes(width: int, height: int) -> bytes:
    """A PNG header with the requested IHDR dimensions.

    The upload validator reads the signature and the IHDR chunk only (no image
    decoding dependency), which is exactly the size information it needs to
    enforce the 4x2 spritesheet grid.
    """
    ihdr = (
        b"\x00\x00\x00\x0dIHDR"
        + width.to_bytes(4, "big")
        + height.to_bytes(4, "big")
        + b"\x08\x06\x00\x00\x00"
        + b"\x00\x00\x00\x00"
    )
    return b"\x89PNG\r\n\x1a\n" + ihdr


def jpeg_bytes() -> bytes:
    return b"\xff\xd8\xff\xe0" + b"\x00" * 64


def create_character(
    client,
    headers,
    *,
    name: str | None = None,
    width: int = 64,
    height: int = 32,
    content: bytes | None = None,
    filename: str = "bard.png",
) -> dict:
    response = client.post(
        CHARACTERS_PATH,
        data={"name": name or helpers.unique("Bard")},
        files={
            "file": (
                filename,
                content if content is not None else png_bytes(width, height),
                "image/png",
            )
        },
        headers=headers,
    )
    assert response.status_code == 201, response.text
    return response.json()


def create_stage(
    client,
    headers,
    *,
    name: str | None = None,
    content: bytes | None = None,
    filename: str = "stage.png",
) -> dict:
    response = client.post(
        STAGES_PATH,
        data={"name": name or helpers.unique("Stage")},
        files={
            "file": (
                filename,
                content if content is not None else png_bytes(320, 180),
                "image/png",
            )
        },
        headers=headers,
    )
    assert response.status_code == 201, response.text
    return response.json()


STAGE_FIELDS = {
    "id",
    "name",
    "background_url",
    "content_type",
    "size_bytes",
    "original_name",
    "created_at",
    "updated_at",
}


# ---------------------------------------------------------------------------
# UI LEVELS
# ---------------------------------------------------------------------------
def test_create_list_and_delete_character(db_client, admin_user, gameplay_storage):
    character = create_character(db_client, admin_user["headers"], name="Bard")

    assert character["name"] == "Bard"
    assert character["content_type"] == "image/png"
    assert character["size_bytes"] > 0
    assert character["sheet_url"].startswith("/media/characters/")
    stored = gameplay_storage / "characters" / character["sheet_url"].rsplit("/", 1)[1]
    assert stored.is_file()

    listing = db_client.get(CHARACTERS_PATH, headers=admin_user["headers"])
    assert listing.status_code == 200
    assert [item["id"] for item in listing.json()["items"]] == [character["id"]]

    response = db_client.delete(
        f"{CHARACTERS_PATH}/{character['id']}", headers=admin_user["headers"]
    )
    assert response.status_code == 204
    assert db_client.get(CHARACTERS_PATH, headers=admin_user["headers"]).json()[
        "items"
    ] == []
    assert not stored.exists()


def test_character_name_must_be_unique(db_client, admin_user):
    create_character(db_client, admin_user["headers"], name="Bluesman")
    response = db_client.post(
        CHARACTERS_PATH,
        data={"name": "bluesman"},
        files={"file": ("b.png", png_bytes(64, 32), "image/png")},
        headers=admin_user["headers"],
    )
    assert response.status_code == 409, response.text
    assert "already exists" in response.json()["detail"]


@pytest.mark.parametrize(
    "content,filename,expected",
    [
        (jpeg_bytes(), "bard.jpg", 415),
        (png_bytes(32, 32), "square.png", 422),
        (png_bytes(62, 32), "odd.png", 422),
        (b"not an image", "fake.png", 415),
    ],
)
def test_character_spritesheet_is_validated(
    db_client, admin_user, content, filename, expected
):
    response = db_client.post(
        CHARACTERS_PATH,
        data={"name": helpers.unique("Bad")},
        files={"file": (filename, content, "image/png")},
        headers=admin_user["headers"],
    )
    assert response.status_code == expected, response.text


def test_character_upload_rejects_blank_name(db_client, admin_user):
    response = db_client.post(
        CHARACTERS_PATH,
        data={"name": "   "},
        files={"file": ("b.png", png_bytes(64, 32), "image/png")},
        headers=admin_user["headers"],
    )
    assert response.status_code == 422, response.text


def test_character_can_be_replaced(db_client, admin_user, gameplay_storage):
    character = create_character(db_client, admin_user["headers"])
    old_key = character["sheet_url"].rsplit("/", 1)[1]

    response = db_client.post(
        f"{CHARACTERS_PATH}/{character['id']}/replace",
        files={"file": ("new.png", png_bytes(128, 64), "image/png")},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    updated = response.json()
    assert updated["size_bytes"] == len(png_bytes(128, 64))
    new_key = updated["sheet_url"].rsplit("/", 1)[1]
    assert new_key != old_key
    assert (gameplay_storage / "characters" / new_key).is_file()
    assert not (gameplay_storage / "characters" / old_key).exists()


def test_character_delete_is_not_blocked_by_stages(db_client, admin_user):
    character = create_character(db_client, admin_user["headers"], name="Jazzman")
    stage = create_stage(db_client, admin_user["headers"], name="Club")

    response = db_client.delete(
        f"{CHARACTERS_PATH}/{character['id']}", headers=admin_user["headers"]
    )
    assert response.status_code == 204, response.text

    stages = db_client.get(STAGES_PATH, headers=admin_user["headers"]).json()["items"]
    assert [item["id"] for item in stages] == [stage["id"]]
    assert "character_id" not in stages[0]
    assert "character_name" not in stages[0]


# ---------------------------------------------------------------------------
# STAGE LEVELS
# ---------------------------------------------------------------------------
def test_stage_creation_needs_only_name_and_background(
    db_client, admin_user, gameplay_storage
):
    stage = create_stage(db_client, admin_user["headers"], name="Night Club")

    assert set(stage) == STAGE_FIELDS
    assert "character_id" not in stage
    assert "character_name" not in stage
    assert stage["name"] == "Night Club"
    assert stage["content_type"] == "image/png"
    assert stage["size_bytes"] > 0
    assert stage["background_url"].startswith("/media/stages/")
    stored = gameplay_storage / "stages" / stage["background_url"].rsplit("/", 1)[1]
    assert stored.is_file()

    listing = db_client.get(STAGES_PATH, headers=admin_user["headers"])
    assert listing.status_code == 200
    items = listing.json()["items"]
    assert [item["id"] for item in items] == [stage["id"]]
    assert set(items[0]) == STAGE_FIELDS


def test_stage_creation_ignores_legacy_character_name(db_client, admin_user):
    """An old cached admin page may still send ``character_name``.

    The field is not part of the form model any more, so it is ignored instead
    of producing a character 422.
    """
    response = db_client.post(
        STAGES_PATH,
        data={"name": "Legacy", "character_name": "Nobody"},
        files={"file": ("stage.png", png_bytes(320, 180), "image/png")},
        headers=admin_user["headers"],
    )
    assert response.status_code == 201, response.text
    body = response.json()
    assert set(body) == STAGE_FIELDS
    assert body["name"] == "Legacy"


def test_stage_create_list_replace_and_delete(db_client, admin_user, gameplay_storage):
    stage = create_stage(db_client, admin_user["headers"], name="Rooftop")
    old_key = stage["background_url"].rsplit("/", 1)[1]

    listing = db_client.get(STAGES_PATH, headers=admin_user["headers"])
    assert listing.status_code == 200
    assert [item["id"] for item in listing.json()["items"]] == [stage["id"]]

    replaced = db_client.post(
        f"{STAGES_PATH}/{stage['id']}/replace",
        data={"name": "Rooftop Night"},
        files={"file": ("stage.jpg", jpeg_bytes(), "image/jpeg")},
        headers=admin_user["headers"],
    )
    assert replaced.status_code == 200, replaced.text
    updated = replaced.json()
    assert set(updated) == STAGE_FIELDS
    assert updated["name"] == "Rooftop Night"
    assert updated["content_type"] == "image/jpeg"
    assert updated["background_url"].endswith(".jpg")
    new_key = updated["background_url"].rsplit("/", 1)[1]
    assert (gameplay_storage / "stages" / new_key).is_file()
    assert not (gameplay_storage / "stages" / old_key).exists()

    assert db_client.delete(
        f"{STAGES_PATH}/{stage['id']}", headers=admin_user["headers"]
    ).status_code == 204
    assert not (gameplay_storage / "stages" / new_key).exists()
    assert db_client.get(STAGES_PATH, headers=admin_user["headers"]).json()[
        "items"
    ] == []


def test_stage_replace_requires_a_change(db_client, admin_user):
    stage = create_stage(db_client, admin_user["headers"], name="Empty Replace")

    response = db_client.post(
        f"{STAGES_PATH}/{stage['id']}/replace",
        headers=admin_user["headers"],
    )
    assert response.status_code == 422, response.text
    assert "new name or a new background" in response.json()["detail"]

    missing = db_client.post(
        f"{STAGES_PATH}/{uuid.uuid4()}/replace",
        data={"name": "Nope"},
        headers=admin_user["headers"],
    )
    assert missing.status_code == 404, missing.text


def test_stage_background_is_served_from_media_mount(db_client, admin_user):
    stage = create_stage(db_client, admin_user["headers"], name="Stage")

    response = db_client.get(stage["background_url"])
    assert response.status_code == 200, response.text
    assert response.headers["content-type"].startswith("image/png")
    assert response.content == png_bytes(320, 180)


@pytest.mark.parametrize(
    "content,filename,expected",
    [
        (b"<html>", "stage.png", 415),
        (b"GIF89a" + b"\x00" * 32, "stage.gif", 415),
        (b"", "empty.png", 422),
        (png_bytes(320, 180), "stage.png", 201),
        (jpeg_bytes(), "stage.jpg", 201),
    ],
)
def test_stage_background_is_validated(
    db_client, admin_user, content, filename, expected
):
    response = db_client.post(
        STAGES_PATH,
        data={"name": helpers.unique("Stage")},
        files={"file": (filename, content, "image/png")},
        headers=admin_user["headers"],
    )
    assert response.status_code == expected, response.text


def test_stage_upload_rejects_blank_name(db_client, admin_user):
    response = db_client.post(
        STAGES_PATH,
        data={"name": "   "},
        files={"file": ("stage.png", png_bytes(320, 180), "image/png")},
        headers=admin_user["headers"],
    )
    assert response.status_code == 422, response.text


# ---------------------------------------------------------------------------
# SONGS
# ---------------------------------------------------------------------------
def test_song_visuals_are_saved_and_exposed_publicly(
    db_client, admin_user, draft_song
):
    character = create_character(db_client, admin_user["headers"], name="Bard")
    stage = create_stage(db_client, admin_user["headers"], name="Tavern")

    updated = db_client.patch(
        f"{helpers.API}/admin/songs/{draft_song['id']}",
        json={"character_id": character["id"], "stage_id": stage["id"]},
        headers=admin_user["headers"],
    )
    assert updated.status_code == 200, updated.text
    body = updated.json()
    assert body["character_id"] == character["id"]
    assert body["character_name"] == "Bard"
    assert body["stage_id"] == stage["id"]
    assert body["stage_name"] == "Tavern"
    assert body["character_sheet_url"] == character["sheet_url"]
    assert body["stage_background_url"] == stage["background_url"]

    admin_list = db_client.get(
        f"{helpers.API}/admin/songs", headers=admin_user["headers"]
    ).json()["items"]
    assert admin_list[0]["character_name"] == "Bard"
    assert admin_list[0]["stage_name"] == "Tavern"

    helpers.publish_song(db_client, admin_user["headers"], draft_song["id"])
    item = db_client.get(f"{helpers.API}/songs").json()["items"][0]
    assert item["character_name"] == "Bard"
    assert item["character_sheet_url"] == character["sheet_url"]
    assert item["stage_name"] == "Tavern"
    assert item["stage_background_url"] == stage["background_url"]

    detail = db_client.get(f"{helpers.API}/songs/{draft_song['slug']}").json()
    assert detail["character_name"] == "Bard"
    assert detail["stage_name"] == "Tavern"


def test_public_visual_fields_are_null_without_selection(
    db_client, admin_user, published_song
):
    item = db_client.get(f"{helpers.API}/songs").json()["items"][0]
    assert item["character_name"] is None
    assert item["character_sheet_url"] is None
    assert item["stage_name"] is None
    assert item["stage_background_url"] is None


def test_song_update_clears_visuals_with_explicit_null(
    db_client, admin_user, draft_song
):
    character = create_character(db_client, admin_user["headers"], name="Bard")
    stage = create_stage(db_client, admin_user["headers"], name="Tavern")
    db_client.patch(
        f"{helpers.API}/admin/songs/{draft_song['id']}",
        json={"character_id": character["id"], "stage_id": stage["id"]},
        headers=admin_user["headers"],
    )

    cleared = db_client.patch(
        f"{helpers.API}/admin/songs/{draft_song['id']}",
        json={"character_id": None, "stage_id": None},
        headers=admin_user["headers"],
    )
    assert cleared.status_code == 200, cleared.text
    assert cleared.json()["character_name"] is None
    assert cleared.json()["stage_name"] is None


def test_song_accepts_any_stage_for_a_character(db_client, admin_user, draft_song):
    """Stages are standalone: the admin may pair any stage with any character."""
    first = create_character(db_client, admin_user["headers"], name="Bard")
    other = create_character(db_client, admin_user["headers"], name="Bluesman")
    stage = create_stage(db_client, admin_user["headers"], name="Tavern")

    response = db_client.patch(
        f"{helpers.API}/admin/songs/{draft_song['id']}",
        json={"character_id": other["id"], "stage_id": stage["id"]},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["character_id"] == other["id"]
    assert body["character_name"] == "Bluesman"
    assert body["stage_id"] == stage["id"]
    assert body["stage_name"] == "Tavern"
    assert first["name"] == "Bard"


def test_deleting_stage_detaches_it_from_song(db_client, admin_user, draft_song):
    stage = create_stage(db_client, admin_user["headers"], name="Tavern")
    db_client.patch(
        f"{helpers.API}/admin/songs/{draft_song['id']}",
        json={"stage_id": stage["id"]},
        headers=admin_user["headers"],
    )

    assert db_client.delete(
        f"{STAGES_PATH}/{stage['id']}", headers=admin_user["headers"]
    ).status_code == 204

    detail = db_client.get(
        f"{helpers.API}/admin/songs/{draft_song['id']}",
        headers=admin_user["headers"],
    )
    assert detail.status_code == 200
    assert detail.json()["stage_id"] is None
    assert detail.json()["stage_name"] is None


def test_deleting_character_keeps_stage_and_clears_song_selection(
    db_client, admin_user, draft_song
):
    character = create_character(db_client, admin_user["headers"], name="Bard")
    stage = create_stage(db_client, admin_user["headers"], name="Tavern")
    db_client.patch(
        f"{helpers.API}/admin/songs/{draft_song['id']}",
        json={"character_id": character["id"], "stage_id": stage["id"]},
        headers=admin_user["headers"],
    )

    assert db_client.delete(
        f"{CHARACTERS_PATH}/{character['id']}", headers=admin_user["headers"]
    ).status_code == 204

    detail = db_client.get(
        f"{helpers.API}/admin/songs/{draft_song['id']}",
        headers=admin_user["headers"],
    )
    assert detail.status_code == 200
    assert detail.json()["character_id"] is None
    assert detail.json()["character_name"] is None
    assert detail.json()["stage_id"] == stage["id"]
    assert detail.json()["stage_name"] == "Tavern"
    assert db_client.get(STAGES_PATH, headers=admin_user["headers"]).json()[
        "items"
    ][0]["id"] == stage["id"]


# ---------------------------------------------------------------------------
# Authorization and admin UI surface
# ---------------------------------------------------------------------------
def test_visuals_endpoints_require_an_administrator(db_client, user_account):
    assert db_client.get(CHARACTERS_PATH, headers=user_account["headers"]).status_code == 403
    assert db_client.get(STAGES_PATH, headers=user_account["headers"]).status_code == 403
    assert db_client.get(CHARACTERS_PATH).status_code == 401


def test_admin_ui_has_the_two_tabs_and_song_selectors():
    from pathlib import Path

    admin_dir = Path(__file__).resolve().parents[1] / "app" / "static" / "admin"
    html = (admin_dir / "index.html").read_text(encoding="utf-8")
    assert 'data-admin-section="ui-levels"' in html
    assert 'data-admin-section="stage-levels"' in html
    assert 'id="sectionUiLevels"' in html
    assert 'id="sectionStageLevels"' in html
    assert 'id="songCharacter"' in html
    assert 'id="songStage"' in html
    # STAGE LEVELS form: scene name + background upload only.
    assert 'id="stageName"' in html
    assert 'id="stageFile"' in html
    assert 'id="stageCharacter"' not in html
    assert 'id="stageLevelForm"' in html

    script = (admin_dir / "admin.js").read_text(encoding="utf-8")
    assert "character_id" in script
    assert "stage_id" in script
    assert 'el("songCharacter")' in script

    gameplay_script = (admin_dir / "admin-gameplay.js").read_text(encoding="utf-8")
    assert "/ui-levels" in gameplay_script
    assert "/stage-levels" in gameplay_script
    assert "renderStageCharacterSelect" not in gameplay_script
    assert 'data.append("character_name"' not in gameplay_script
    assert "stage.character_name" not in gameplay_script
