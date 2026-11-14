"""UI sounds: registry, manifest, admin upload/replace/delete, user prefs.

The tests exercise the same HTTP contract as the admin SPA and the JUCE client:

* the registry is fixed and seeded by migration ``0032_ui_sounds``;
* the manifest lists only active events with a file and exposes no filesystem
  paths, only public download URLs carrying the per-file version;
* uploads are validated by extension *and* signature, the size limit is
  enforced, a replacement bumps ``version`` and removes the old file;
* gain is clamped and ``active`` toggles without touching the file;
* admin routes are admin-only, user settings belong to the token's account.
"""
from __future__ import annotations

import pytest

from app.config import settings
from app.services.ui_sound_registry import UI_SOUND_KEYS, UI_SOUND_REGISTRY
from tests import helpers

pytestmark = pytest.mark.db

ADMIN = f"{helpers.API}/admin/ui-sounds"
MANIFEST = f"{helpers.API}/ui-sounds/manifest"
EVENTS = f"{helpers.API}/ui-sounds/events"
PROFILE = f"{helpers.API}/profile/ui-sounds"
ADMIN_DEFAULTS = {
    "active": True,
    "gain": 1.0,
    "version": 0,
    "has_file": False,
    "preview_url": None,
    "original_name": None,
    "content_type": None,
    "size_bytes": None,
}


def mp3_bytes(*, size: int = 256, marker: int = 0x33) -> bytes:
    return helpers.build_mp3_bytes(marker=marker, size=size)


def wav_bytes(*, size: int = 256) -> bytes:
    header = b"RIFF" + (size - 8).to_bytes(4, "little") + b"WAVE"
    return header + b"\x00" * max(0, size - len(header))


def ogg_bytes(*, size: int = 256) -> bytes:
    return b"OggS" + b"\x00" * max(0, size - 4)


def upload(client, headers, key: str, *, content: bytes | None = None, filename: str = "clip.mp3"):
    return client.post(
        f"{ADMIN}/{key}/file",
        files={"file": (filename, content if content is not None else mp3_bytes(), "audio/mpeg")},
        headers=headers,
    )


def admin_item(client, headers, key: str) -> dict:
    response = client.get(ADMIN, headers=headers)
    assert response.status_code == 200, response.text
    for item in response.json()["items"]:
        if item["key"] == key:
            return item
    raise AssertionError(f"{key} is missing from the admin list")


def manifest(client) -> dict:
    response = client.get(MANIFEST)
    assert response.status_code == 200, response.text
    return response.json()


@pytest.fixture(autouse=True)
def clean_ui_sounds(db_engine, gameplay_storage):
    """Reset the reference registry and its files before every test.

    ``ui_sound_events`` is reference data and therefore survives the suite's
    ``TRUNCATE`` (like ``achievements``), but these tests deliberately upload
    files, toggle events and change gains. Restoring the seeded state keeps the
    tests independent of their order.
    """
    from app.database import SessionLocal
    from app.models.ui_sound import UiSoundEvent

    session = SessionLocal()
    try:
        session.query(UiSoundEvent).update(
            {
                UiSoundEvent.active: True,
                UiSoundEvent.gain: 1.0,
                UiSoundEvent.stored_name: None,
                UiSoundEvent.original_name: None,
                UiSoundEvent.content_type: None,
                UiSoundEvent.size_bytes: None,
                UiSoundEvent.version: 0,
            }
        )
        session.commit()
    finally:
        session.close()

    directory = gameplay_storage / "ui_sounds"
    if directory.exists():
        for path in directory.iterdir():
            if path.is_file():
                path.unlink()

    yield


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------
def test_registry_is_seeded_from_the_fixed_list(db_client, admin_user):
    response = db_client.get(ADMIN, headers=admin_user["headers"])
    assert response.status_code == 200, response.text
    items = response.json()["items"]

    assert [item["key"] for item in items] == [entry.key for entry in UI_SOUND_REGISTRY]
    assert len(items) == len(UI_SOUND_KEYS) == 24
    assert {item["category"] for item in items} == {"navigation", "action", "system"}
    for item in items:
        for field, default in ADMIN_DEFAULTS.items():
            assert item[field] == default, (item["key"], field)

    # A fresh registry has no files, so the anonymous manifest is empty.
    empty = manifest(db_client)
    assert empty["items"] == []
    assert isinstance(empty["version"], int)


def test_registry_has_unique_keys_and_known_categories():
    """The migration seed and the client registry must not drift."""
    keys = [entry.key for entry in UI_SOUND_REGISTRY]
    assert len(keys) == len(set(keys)) == len(UI_SOUND_KEYS)
    assert all(entry.category in {"navigation", "action", "system"} for entry in UI_SOUND_REGISTRY)


# ---------------------------------------------------------------------------
# Manifest
# ---------------------------------------------------------------------------
def test_manifest_lists_only_active_events_with_files(db_client, admin_user):
    headers = admin_user["headers"]
    assert upload(db_client, headers, "nav.menu").status_code == 200
    assert upload(
        db_client, headers, "error", content=wav_bytes(), filename="error.wav"
    ).status_code == 200

    first = manifest(db_client)
    assert {item["key"] for item in first["items"]} == {"nav.menu", "error"}
    by_key = {item["key"]: item for item in first["items"]}
    assert by_key["nav.menu"] == {
        "key": "nav.menu",
        "name": "Menu item",
        "category": "navigation",
        "gain": 1.0,
        "url": by_key["nav.menu"]["url"],
        "version": 1,
    }
    assert by_key["nav.menu"]["url"].startswith(
        "http://testserver/api/v1/ui-sounds/nav.menu/file"
    )
    assert by_key["nav.menu"]["url"].endswith("v=1")
    # No filesystem path or internal storage name is exposed.
    for item in first["items"]:
        assert "/data/" not in item["url"]
        assert "ui_sounds" not in item["url"]
        assert "gameplay_media" not in item["url"]
    assert isinstance(first["version"], int)

    # The manifest is revalidated, not blindly cached.
    cached = db_client.get(MANIFEST, headers={"If-None-Match": f'"ui-sounds-{first["version"]}"'})
    assert cached.status_code == 304

    # A disabled event disappears from the manifest but keeps its file.
    disabled = db_client.patch(
        f"{ADMIN}/nav.menu", json={"active": False}, headers=headers
    )
    assert disabled.status_code == 200, disabled.text
    second = manifest(db_client)
    assert [item["key"] for item in second["items"]] == ["error"]
    assert second["version"] != first["version"]
    assert admin_item(db_client, headers, "nav.menu")["has_file"] is True

    # Deleting the remaining file empties the manifest.
    assert db_client.delete(f"{ADMIN}/error/file", headers=headers).status_code == 204
    assert manifest(db_client)["items"] == []


def test_file_cache_headers_follow_the_version(db_client, admin_user):
    headers = admin_user["headers"]
    assert upload(db_client, headers, "button").status_code == 200

    current = db_client.get(f"{helpers.API}/ui-sounds/button/file?v=1")
    assert current.status_code == 200
    assert current.content == mp3_bytes()
    assert current.headers["content-type"] == "audio/mpeg"
    assert "immutable" in current.headers["cache-control"]

    stale = db_client.get(f"{helpers.API}/ui-sounds/button/file?v=0")
    assert stale.status_code == 200
    assert "no-cache" in stale.headers["cache-control"]

    no_version = db_client.get(f"{helpers.API}/ui-sounds/button/file")
    assert no_version.status_code == 200
    assert "no-cache" in no_version.headers["cache-control"]


# ---------------------------------------------------------------------------
# Upload validation
# ---------------------------------------------------------------------------
def test_upload_validates_extension_signature_and_size(db_client, admin_user, monkeypatch):
    headers = admin_user["headers"]

    unknown = upload(db_client, headers, "nope.unknown")
    assert unknown.status_code == 404

    wrong_extension = upload(db_client, headers, "button", filename="clip.txt")
    assert wrong_extension.status_code == 400

    mismatched = upload(
        db_client, headers, "button", content=wav_bytes(), filename="clip.mp3"
    )
    assert mismatched.status_code == 400

    garbage = upload(db_client, headers, "button", content=b"not audio", filename="clip.mp3")
    assert garbage.status_code == 400

    empty = upload(db_client, headers, "button", content=b"", filename="clip.mp3")
    assert empty.status_code == 400

    monkeypatch.setattr(settings, "max_ui_sound_bytes", 64)
    oversized = upload(db_client, headers, "button", content=mp3_bytes(size=256))
    assert oversized.status_code == 413

    # Rejections never leave a file or a version bump behind.
    item = admin_item(db_client, headers, "button")
    assert item["has_file"] is False
    assert item["version"] == 0
    assert manifest(db_client)["items"] == []


@pytest.mark.parametrize(
    "content,filename,content_type",
    [
        (mp3_bytes(), "clip.mp3", "audio/mpeg"),
        (wav_bytes(), "clip.wav", "audio/wav"),
        (ogg_bytes(), "clip.ogg", "audio/ogg"),
    ],
)
def test_supported_formats_are_accepted(
    db_client, admin_user, content, filename, content_type
):
    response = upload(db_client, admin_user["headers"], "button", content=content, filename=filename)
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["has_file"] is True
    assert body["version"] == 1
    assert body["content_type"] == content_type
    assert body["size_bytes"] == len(content)
    assert body["original_name"] == filename
    assert body["preview_url"].startswith(
        "http://testserver/api/v1/admin/ui-sounds/button/file"
    )


# ---------------------------------------------------------------------------
# Replace / delete
# ---------------------------------------------------------------------------
def test_replace_bumps_version_and_removes_the_previous_file(
    db_client, admin_user, db_session, gameplay_storage
):
    headers = admin_user["headers"]
    first = upload(db_client, headers, "nav.section").json()
    assert first["version"] == 1

    directory = gameplay_storage / "ui_sounds"
    first_files = {path.name for path in directory.iterdir()}
    assert len(first_files) == 1

    replaced = upload(
        db_client, headers, "nav.section", content=wav_bytes(size=512), filename="new.wav"
    )
    assert replaced.status_code == 200, replaced.text
    body = replaced.json()
    assert body["version"] == 2
    assert body["content_type"] == "audio/wav"
    assert body["size_bytes"] == 512
    assert body["original_name"] == "new.wav"

    second_files = {path.name for path in directory.iterdir()}
    assert len(second_files) == 1
    assert second_files.isdisjoint(first_files)

    shown = admin_item(db_client, headers, "nav.section")
    assert shown["version"] == 2
    assert shown["size_bytes"] == 512
    assert manifest(db_client)["items"][0]["version"] == 2


def test_delete_clears_the_file_and_bumps_version(
    db_client, admin_user, gameplay_storage
):
    headers = admin_user["headers"]
    assert upload(db_client, headers, "notification").status_code == 200

    deleted = db_client.delete(f"{ADMIN}/notification/file", headers=headers)
    assert deleted.status_code == 204

    item = admin_item(db_client, headers, "notification")
    assert item["version"] == 2
    assert item["has_file"] is False
    assert item["original_name"] is None
    assert item["content_type"] is None
    assert item["size_bytes"] is None
    assert item["preview_url"] is None
    assert list((gameplay_storage / "ui_sounds").iterdir()) == []
    assert manifest(db_client)["items"] == []

    # Deleting an already empty event is idempotent and does not bump again.
    assert db_client.delete(f"{ADMIN}/notification/file", headers=headers).status_code == 204
    assert admin_item(db_client, headers, "notification")["version"] == 2

    # Unknown keys are 404, never a silent success.
    assert db_client.delete(f"{ADMIN}/unknown.key/file", headers=headers).status_code == 404


# ---------------------------------------------------------------------------
# PATCH active/gain
# ---------------------------------------------------------------------------
def test_patch_clamps_gain_and_toggles_active(db_client, admin_user):
    headers = admin_user["headers"]

    high = db_client.patch(f"{ADMIN}/toggle", json={"gain": 2.5}, headers=headers)
    assert high.status_code == 200, high.text
    assert high.json()["gain"] == 1.0

    low = db_client.patch(f"{ADMIN}/toggle", json={"gain": -0.4}, headers=headers)
    assert low.status_code == 200
    assert low.json()["gain"] == 0.0

    exact = db_client.patch(
        f"{ADMIN}/toggle", json={"gain": 0.42, "active": False}, headers=headers
    )
    assert exact.status_code == 200
    assert exact.json()["gain"] == pytest.approx(0.42)
    assert exact.json()["active"] is False

    assert db_client.patch(
        f"{ADMIN}/toggle", json={"gain": "loud"}, headers=headers
    ).status_code == 422
    assert db_client.patch(
        f"{ADMIN}/unknown.key", json={"active": True}, headers=headers
    ).status_code == 404


# ---------------------------------------------------------------------------
# Authorization
# ---------------------------------------------------------------------------
def test_admin_endpoints_are_admin_only(db_client, admin_user, user_account):
    key = "button"
    other = "nav.back"

    anonymous = [
        db_client.get(ADMIN),
        db_client.post(f"{ADMIN}/{key}/file", files={"file": ("c.mp3", mp3_bytes(), "audio/mpeg")}),
        db_client.delete(f"{ADMIN}/{key}/file"),
        db_client.patch(f"{ADMIN}/{key}", json={"active": False}),
        db_client.get(f"{ADMIN}/{key}/file"),
    ]
    assert [response.status_code for response in anonymous] == [401] * 5

    user_headers = user_account["headers"]
    forbidden = [
        db_client.get(ADMIN, headers=user_headers),
        db_client.post(
            f"{ADMIN}/{key}/file",
            files={"file": ("c.mp3", mp3_bytes(), "audio/mpeg")},
            headers=user_headers,
        ),
        db_client.delete(f"{ADMIN}/{key}/file", headers=user_headers),
        db_client.patch(f"{ADMIN}/{key}", json={"active": False}, headers=user_headers),
        db_client.get(f"{ADMIN}/{key}/file", headers=user_headers),
    ]
    assert [response.status_code for response in forbidden] == [403] * 5

    # Admin preview works only once a file exists.
    assert db_client.get(f"{ADMIN}/{other}/file", headers=admin_user["headers"]).status_code == 404
    assert upload(db_client, admin_user["headers"], other).status_code == 200
    preview = db_client.get(f"{ADMIN}/{other}/file", headers=admin_user["headers"])
    assert preview.status_code == 200
    assert preview.content == mp3_bytes()
    assert "no-cache" in preview.headers["cache-control"]


# ---------------------------------------------------------------------------
# Events + user preferences
# ---------------------------------------------------------------------------
def test_events_require_authentication_and_include_preferences(
    db_client, admin_user, user_account
):
    assert db_client.get(EVENTS).status_code == 401

    upload(db_client, admin_user["headers"], "level.up")
    response = db_client.get(EVENTS, headers=user_account["headers"])
    assert response.status_code == 200, response.text
    body = response.json()

    assert len(body["items"]) == 24
    assert {item["key"] for item in body["items"]} == UI_SOUND_KEYS
    by_key = {item["key"]: item for item in body["items"]}
    assert by_key["level.up"]["has_file"] is True
    assert by_key["level.up"]["name"] == "Level up"
    assert by_key["level.up"]["category"] == "system"
    assert by_key["nav.back"]["has_file"] is False
    assert body["settings"] == {
        "enabled": True,
        "volume": 0.8,
        "navigation": True,
        "action": True,
        "system": True,
        "updated_at": body["settings"]["updated_at"],
    }
    assert body["settings"]["updated_at"] is not None


def test_user_preferences_default_patch_and_isolation(
    db_client, user_account, second_user
):
    assert db_client.get(PROFILE).status_code == 401
    assert db_client.patch(PROFILE, json={"enabled": False}).status_code == 401

    defaults = db_client.get(PROFILE, headers=user_account["headers"])
    assert defaults.status_code == 200, defaults.text
    body = defaults.json()
    assert body["updated_at"] is not None
    assert {key: body[key] for key in ("enabled", "volume", "navigation", "action", "system")} == {
        "enabled": True,
        "volume": 0.8,
        "navigation": True,
        "action": True,
        "system": True,
    }

    updated = db_client.patch(
        PROFILE,
        json={
            "enabled": False,
            "volume": 2.0,
            "navigation": False,
            "action": True,
            "system": False,
        },
        headers=user_account["headers"],
    )
    assert updated.status_code == 200, updated.text
    body = updated.json()
    assert body["enabled"] is False
    assert body["volume"] == 1.0  # clamped
    assert body["navigation"] is False
    assert body["action"] is True
    assert body["system"] is False

    persisted = db_client.get(PROFILE, headers=user_account["headers"]).json()
    assert persisted == body

    clamped = db_client.patch(
        PROFILE, json={"volume": -3}, headers=user_account["headers"]
    )
    assert clamped.status_code == 200
    assert clamped.json()["volume"] == 0.0

    # A second account keeps its own defaults; the first one is untouched.
    other_defaults = db_client.get(PROFILE, headers=second_user["headers"])
    assert other_defaults.status_code == 200
    assert other_defaults.json()["enabled"] is True
    assert other_defaults.json()["volume"] == 0.8
    assert other_defaults.json()["navigation"] is True

    other = db_client.patch(
        PROFILE, json={"volume": 0.25}, headers=second_user["headers"]
    )
    assert other.status_code == 200
    assert other.json()["volume"] == 0.25
    assert db_client.get(PROFILE, headers=user_account["headers"]).json()["volume"] == 0.0

    # The endpoint has no user id field: a body trying to target somebody else
    # cannot write their row.
    ignored = db_client.patch(
        PROFILE,
        json={"volume": 0.5, "user_id": str(second_user["id"])},
        headers=user_account["headers"],
    )
    assert ignored.status_code == 200
    assert ignored.json()["volume"] == 0.5
    assert db_client.get(PROFILE, headers=second_user["headers"]).json()["volume"] == 0.25


def test_preferences_are_persisted_in_the_database(
    db_client, user_account, db_session
):
    from uuid import UUID

    from app.models.ui_sound import UserUiSoundSettings

    db_client.patch(
        PROFILE, json={"volume": 0.35, "system": False}, headers=user_account["headers"]
    )
    row = db_session.get(UserUiSoundSettings, UUID(user_account["id"]))
    assert row is not None
    assert row.volume == pytest.approx(0.35)
    assert row.system is False
    assert row.enabled is True
