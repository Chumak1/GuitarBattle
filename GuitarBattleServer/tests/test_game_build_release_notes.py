"""Editable release notes (``release_notes``) for uploaded game builds.

The installer is uploaded once; its "what's new" text must be editable through
``PATCH /admin/site/game-builds/{id}`` and immediately visible in the public
``GET /api/v1/landing`` payload the JUCE client reads. These tests pin the
public contract, the admin round-trip, validation and that editing metadata
never changes the file or the activation state.
"""
from __future__ import annotations

from tests import helpers

BUILD_BYTES = b"PK\x03\x04" + b"0" * 256


def _upload_build(
    client,
    admin_user,
    *,
    version: str = "1.0.0",
    release_notes: str | None = None,
    filename: str = "GuitarBattle-1.0.0.zip",
):
    data = {"version": version}
    if release_notes is not None:
        data["release_notes"] = release_notes
    response = client.post(
        f"{helpers.API}/admin/site/game-builds",
        data=data,
        files={"file": (filename, BUILD_BYTES, "application/zip")},
        headers=admin_user["headers"],
    )
    assert response.status_code == 201, response.text
    return response.json()


def _activate(client, admin_user, build_id: int):
    response = client.post(
        f"{helpers.API}/admin/site/game-builds/{build_id}/activate",
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    return response.json()


def _patch(client, admin_user, build_id: int, payload):
    return client.patch(
        f"{helpers.API}/admin/site/game-builds/{build_id}",
        json=payload,
        headers=admin_user["headers"],
    )


def _landing(client):
    response = client.get(f"{helpers.API}/landing")
    assert response.status_code == 200, response.text
    return response.json()


def _admin_builds(client, admin_user):
    response = client.get(
        f"{helpers.API}/admin/site/game-builds", headers=admin_user["headers"]
    )
    assert response.status_code == 200, response.text
    return response.json()["items"]


def _find(items, build_id: int):
    for item in items:
        if item["id"] == build_id:
            return item
    raise AssertionError(f"build {build_id} is missing from {items!r}")


# ---------------------------------------------------------------------------
# Public contract
# ---------------------------------------------------------------------------
def test_landing_without_build_has_empty_release_notes(db_client):
    download = _landing(db_client)["download"]

    assert download["available"] is False
    assert download["release_notes"] == ""


def test_uploaded_release_notes_reach_landing_and_admin_list(db_client, admin_user):
    notes = "Добавлен режим тренировки\nИсправлен звук метронома"
    build = _upload_build(db_client, admin_user, release_notes=notes)
    assert build["release_notes"] == notes

    _activate(db_client, admin_user, build["id"])

    download = _landing(db_client)["download"]
    assert download["available"] is True
    assert download["version"] == "1.0.0"
    assert download["release_notes"] == notes

    listed = _find(_admin_builds(db_client, admin_user), build["id"])
    assert listed["release_notes"] == notes


def test_upload_strips_release_notes(db_client, admin_user):
    build = _upload_build(db_client, admin_user, release_notes="  Что нового  \n")

    assert build["release_notes"] == "Что нового"


# ---------------------------------------------------------------------------
# Editing through PATCH
# ---------------------------------------------------------------------------
def test_patch_updates_active_build_and_empty_value_clears(db_client, admin_user):
    build = _upload_build(db_client, admin_user, release_notes="Старое описание")
    _activate(db_client, admin_user, build["id"])

    response = _patch(
        db_client,
        admin_user,
        build["id"],
        {"release_notes": "  Новое описание\nвторая строка  "},
    )
    assert response.status_code == 200, response.text
    assert response.json()["release_notes"] == "Новое описание\nвторая строка"
    assert _landing(db_client)["download"]["release_notes"] == (
        "Новое описание\nвторая строка"
    )

    cleared = _patch(db_client, admin_user, build["id"], {"release_notes": ""})
    assert cleared.status_code == 200, cleared.text
    assert cleared.json()["release_notes"] == ""
    assert _landing(db_client)["download"]["release_notes"] == ""
    assert _find(_admin_builds(db_client, admin_user), build["id"])["release_notes"] == ""


def test_patch_edits_inactive_build_without_activating_it(db_client, admin_user):
    active = _upload_build(db_client, admin_user, version="1.0.0")
    _activate(db_client, admin_user, active["id"])
    inactive = _upload_build(db_client, admin_user, version="1.1.0")

    response = _patch(
        db_client, admin_user, inactive["id"], {"release_notes": "Будущая версия"}
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["is_active"] is False
    assert body["release_notes"] == "Будущая версия"

    download = _landing(db_client)["download"]
    assert download["version"] == "1.0.0"
    assert download["release_notes"] == ""


# ---------------------------------------------------------------------------
# Metadata editing must not touch the installer or activation
# ---------------------------------------------------------------------------
def test_patch_keeps_filename_size_and_activation(db_client, admin_user):
    build = _upload_build(
        db_client,
        admin_user,
        filename="GuitarBattle-Setup-1.0.0.zip",
        release_notes="До",
    )
    _activate(db_client, admin_user, build["id"])
    before = _find(_admin_builds(db_client, admin_user), build["id"])

    response = _patch(db_client, admin_user, build["id"], {"release_notes": "После"})
    assert response.status_code == 200, response.text
    after = response.json()

    assert after["filename"] == before["filename"]
    assert after["size_bytes"] == before["size_bytes"]
    assert after["content_type"] == before["content_type"]
    assert after["is_active"] is True

    # The bytes on disk are still served unchanged after the metadata edit.
    download = db_client.get(f"{helpers.API}/downloads/game")
    assert download.status_code == 200, download.text
    assert download.content == BUILD_BYTES


# ---------------------------------------------------------------------------
# Access control and validation
# ---------------------------------------------------------------------------
def test_patch_missing_build_returns_404(db_client, admin_user):
    response = _patch(db_client, admin_user, 999_999, {"release_notes": "x"})

    assert response.status_code == 404
    assert response.json()["detail"] == "Game build not found"


def test_patch_requires_admin(db_client, admin_user, user_account):
    build = _upload_build(db_client, admin_user)

    anonymous = db_client.patch(
        f"{helpers.API}/admin/site/game-builds/{build['id']}",
        json={"release_notes": "hack"},
    )
    assert anonymous.status_code in (401, 403)

    forbidden = db_client.patch(
        f"{helpers.API}/admin/site/game-builds/{build['id']}",
        json={"release_notes": "hack"},
        headers=user_account["headers"],
    )
    assert forbidden.status_code == 403

    assert _find(_admin_builds(db_client, admin_user), build["id"])["release_notes"] == ""


def test_patch_rejects_release_notes_longer_than_8000(db_client, admin_user):
    build = _upload_build(db_client, admin_user)

    response = _patch(db_client, admin_user, build["id"], {"release_notes": "я" * 8001})

    assert response.status_code == 422, response.text
    assert _find(_admin_builds(db_client, admin_user), build["id"])["release_notes"] == ""


def test_patch_rejects_unknown_fields(db_client, admin_user):
    build = _upload_build(db_client, admin_user)

    response = _patch(
        db_client, admin_user, build["id"], {"release_notes": "x", "version": "2.0.0"}
    )

    assert response.status_code == 422, response.text
    assert _find(_admin_builds(db_client, admin_user), build["id"])["release_notes"] == ""
