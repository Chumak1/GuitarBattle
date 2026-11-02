"""Per-platform game builds: one Windows build and one universal macOS build.

A build is bound to one platform, activation only unpublishes the builds of the
same platform, and the public download endpoint selects the build with
``?platform=``. ``GET /api/v1/landing`` keeps ``download`` pointing at the
Windows build (the JUCE client reads it) and adds the additive ``downloads``
map used by the macOS button on the page.

These tests pin the contract end to end: upload validation per platform,
activation isolation, the public file responses, the landing payload and the
server-rendered macOS download button.
"""
from __future__ import annotations

from pathlib import Path

import pytest
from alembic import command
from alembic.config import Config
from sqlalchemy import text
from sqlalchemy.exc import IntegrityError

from tests import helpers
from tests.conftest import SERVER_ROOT, TEST_DATABASE_DSN

WINDOWS_BYTES = b"PK\x03\x04" + b"w" * 256
MAC_BYTES = b"koly" + b"m" * 256
STATIC_DIR = Path(__file__).resolve().parents[1] / "app" / "static"


def _alembic_config() -> Config:
    config = Config()
    config.set_main_option("script_location", str(SERVER_ROOT / "migrations"))
    config.set_main_option("sqlalchemy.url", TEST_DATABASE_DSN.replace("%", "%%"))
    return config


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _upload(
    client,
    admin_user,
    *,
    filename: str,
    platform: str | None = None,
    version: str = "1.0.0",
    payload: bytes = WINDOWS_BYTES,
):
    data = {"version": version}
    if platform is not None:
        data["platform"] = platform
    return client.post(
        f"{helpers.API}/admin/site/game-builds",
        data=data,
        files={"file": (filename, payload, "application/octet-stream")},
        headers=admin_user["headers"],
    )


def _upload_ok(client, admin_user, *, filename: str, platform: str, **kwargs):
    response = _upload(
        client, admin_user, filename=filename, platform=platform, **kwargs
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


def _admin_builds(client, admin_user, platform: str | None = None):
    url = f"{helpers.API}/admin/site/game-builds"
    if platform is not None:
        url += f"?platform={platform}"
    response = client.get(url, headers=admin_user["headers"])
    assert response.status_code == 200, response.text
    return response.json()["items"]


def _landing(client):
    response = client.get(f"{helpers.API}/landing")
    assert response.status_code == 200, response.text
    return response.json()


def _active_ids(client, admin_user):
    return {item["id"] for item in _admin_builds(client, admin_user) if item["is_active"]}


# ---------------------------------------------------------------------------
# Migration
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_migration_backfills_existing_builds_with_windows(db_engine, clean_db):
    """Rows written before the migration become Windows builds and stay active."""
    config = _alembic_config()
    try:
        command.downgrade(config, "0016_game_build_release_notes")
        with db_engine.begin() as connection:
            columns = {
                row[0]
                for row in connection.execute(
                    text(
                        "SELECT column_name FROM information_schema.columns "
                        "WHERE table_name = 'game_builds'"
                    )
                )
            }
            assert "platform" not in columns

            connection.execute(
                text(
                    """
                    INSERT INTO game_builds (
                        version, release_notes, filename, stored_name,
                        content_type, size_bytes, is_active
                    )
                    VALUES (
                        '0.9.0', 'Legacy build', 'GuitarBattle-0.9.0.zip',
                        'build-legacy.zip', 'application/zip', 1024, true
                    )
                    """
                )
            )

        command.upgrade(config, "head")

        with db_engine.connect() as connection:
            row = connection.execute(
                text(
                    "SELECT platform, is_active, filename FROM game_builds "
                    "WHERE stored_name = 'build-legacy.zip'"
                )
            ).one()
            constraint = connection.execute(
                text(
                    "SELECT pg_get_constraintdef(oid) FROM pg_constraint "
                    "WHERE conname = 'ck_game_builds_platform'"
                )
            ).scalar_one()
        assert row.platform == "windows"
        # The previously published build is not deactivated by the migration.
        assert row.is_active is True
        assert row.filename == "GuitarBattle-0.9.0.zip"
        # The CHECK constraint only allows the two platforms.
        assert "'windows'" in constraint and "'macos'" in constraint
        assert "macos-arm64" not in constraint and "macos-x86_64" not in constraint
    finally:
        command.upgrade(config, "head")
        with db_engine.begin() as connection:
            connection.execute(
                text("DELETE FROM game_builds WHERE stored_name = 'build-legacy.zip'")
            )


@pytest.mark.db
def test_migration_rejects_unknown_platform(db_engine, clean_db):
    with pytest.raises(IntegrityError):
        with db_engine.begin() as connection:
            connection.execute(
                text(
                    """
                    INSERT INTO game_builds (
                        platform, version, release_notes, filename, stored_name,
                        content_type, size_bytes, is_active
                    )
                    VALUES (
                        'linux', '1.0.0', '', 'GuitarBattle.zip',
                        'build-linux.zip', 'application/zip', 1024, false
                    )
                    """
                )
            )


# ---------------------------------------------------------------------------
# Upload validation
# ---------------------------------------------------------------------------
def test_upload_without_platform_is_windows(db_client, admin_user):
    build = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0.zip",
        platform=None,
        payload=WINDOWS_BYTES,
    )

    assert build["platform"] == "windows"
    assert build["is_active"] is False
    assert build["url"] == "/api/v1/downloads/game"


def test_upload_dmg_for_macos(db_client, admin_user):
    build = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-universal.dmg",
        platform="macos",
        payload=MAC_BYTES,
    )

    assert build["platform"] == "macos"
    assert build["content_type"] == "application/x-apple-diskimage"
    assert build["url"] == "/api/v1/downloads/game?platform=macos"


def test_upload_zip_for_macos(db_client, admin_user):
    build = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-macos.zip",
        platform="macos",
        payload=MAC_BYTES,
    )

    assert build["platform"] == "macos"
    assert build["content_type"] == "application/zip"


@pytest.mark.parametrize(
    ("platform", "filename"),
    [
        ("macos", "GuitarBattle-Setup.exe"),
        ("macos", "GuitarBattle-Setup.msi"),
        ("windows", "GuitarBattle-universal.dmg"),
    ],
)
def test_upload_rejects_extension_from_another_platform(
    db_client, admin_user, platform: str, filename: str
):
    response = _upload(db_client, admin_user, filename=filename, platform=platform)

    assert response.status_code == 415, response.text
    detail = response.json()["detail"]
    if platform == "windows":
        assert ".exe" in detail and ".msi" in detail
    else:
        assert ".dmg" in detail
    # Nothing was stored for a rejected pair.
    assert _admin_builds(db_client, admin_user) == []


def test_upload_rejects_unknown_platform(db_client, admin_user):
    response = _upload(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0.dmg",
        platform="linux",
    )

    assert response.status_code == 422, response.text
    assert "platform" in response.json()["detail"]


def test_upload_rejects_removed_macos_architecture(db_client, admin_user):
    """The split macOS platforms were replaced by one universal build."""
    response = _upload(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-arm64.dmg",
        platform="macos-arm64",
        payload=MAC_BYTES,
    )

    assert response.status_code == 422, response.text


def test_upload_requires_admin(db_client, admin_user):
    response = db_client.post(
        f"{helpers.API}/admin/site/game-builds",
        data={"platform": "macos"},
        files={"file": ("GuitarBattle.dmg", MAC_BYTES, "application/octet-stream")},
    )

    assert response.status_code in (401, 403)


# ---------------------------------------------------------------------------
# Activation is per platform
# ---------------------------------------------------------------------------
def test_activating_macos_keeps_windows_build_active(db_client, admin_user):
    windows = _upload_ok(
        db_client, admin_user, filename="GuitarBattle.zip", platform="windows"
    )
    mac = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-universal.dmg",
        platform="macos",
        payload=MAC_BYTES,
    )
    _activate(db_client, admin_user, windows["id"])

    activated = _activate(db_client, admin_user, mac["id"])

    assert activated["is_active"] is True
    assert _active_ids(db_client, admin_user) == {windows["id"], mac["id"]}


def test_only_one_active_build_per_platform(db_client, admin_user):
    first = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-universal.dmg",
        platform="macos",
        version="1.0.0",
        payload=MAC_BYTES,
    )
    second = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.1.0-universal.dmg",
        platform="macos",
        version="1.1.0",
        payload=MAC_BYTES,
    )
    windows = _upload_ok(
        db_client, admin_user, filename="GuitarBattle-1.0.0.zip", platform="windows"
    )
    _activate(db_client, admin_user, first["id"])
    _activate(db_client, admin_user, windows["id"])

    _activate(db_client, admin_user, second["id"])

    active = _active_ids(db_client, admin_user)
    assert first["id"] not in active
    assert second["id"] in active
    # Windows is a different platform and keeps its published build.
    assert windows["id"] in active


# ---------------------------------------------------------------------------
# Admin list
# ---------------------------------------------------------------------------
def test_admin_list_exposes_and_filters_platform(db_client, admin_user):
    windows = _upload_ok(
        db_client, admin_user, filename="GuitarBattle.zip", platform="windows"
    )
    mac = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-universal.dmg",
        platform="macos",
        payload=MAC_BYTES,
    )

    everything = _admin_builds(db_client, admin_user)
    assert {item["platform"] for item in everything} == {"windows", "macos"}

    only_mac = _admin_builds(db_client, admin_user, platform="macos")
    assert [item["id"] for item in only_mac] == [mac["id"]]

    only_windows = _admin_builds(db_client, admin_user, platform="windows")
    assert [item["id"] for item in only_windows] == [windows["id"]]


def test_admin_list_rejects_unknown_platform_filter(db_client, admin_user):
    for value in ("linux", "macos-arm64"):
        response = db_client.get(
            f"{helpers.API}/admin/site/game-builds?platform={value}",
            headers=admin_user["headers"],
        )

        assert response.status_code == 422, value


# ---------------------------------------------------------------------------
# Public downloads
# ---------------------------------------------------------------------------
def test_download_without_platform_serves_the_windows_build(db_client, admin_user):
    """Regression: the desktop client asks for /downloads/game with no query."""
    windows = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0.zip",
        platform="windows",
        payload=WINDOWS_BYTES,
    )
    _activate(db_client, admin_user, windows["id"])
    mac = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-universal.dmg",
        platform="macos",
        payload=MAC_BYTES,
    )
    _activate(db_client, admin_user, mac["id"])

    response = db_client.get(f"{helpers.API}/downloads/game")

    assert response.status_code == 200
    assert response.content == WINDOWS_BYTES
    assert "GuitarBattle-1.0.0.zip" in response.headers["content-disposition"]


def test_download_selects_the_platform_build(db_client, admin_user):
    windows = _upload_ok(
        db_client, admin_user, filename="GuitarBattle.zip", platform="windows"
    )
    _activate(db_client, admin_user, windows["id"])
    mac = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-universal.dmg",
        platform="macos",
        payload=MAC_BYTES,
    )
    _activate(db_client, admin_user, mac["id"])

    response = db_client.get(
        f"{helpers.API}/downloads/game", params={"platform": "macos"}
    )

    assert response.status_code == 200
    assert response.content == MAC_BYTES
    assert response.headers["content-type"] == "application/x-apple-diskimage"
    assert "GuitarBattle-universal.dmg" in response.headers["content-disposition"]


def test_download_404_for_a_platform_without_build(db_client, admin_user):
    windows = _upload_ok(
        db_client, admin_user, filename="GuitarBattle.zip", platform="windows"
    )
    _activate(db_client, admin_user, windows["id"])

    response = db_client.get(
        f"{helpers.API}/downloads/game", params={"platform": "macos"}
    )

    assert response.status_code == 404
    assert "macos" in response.json()["detail"]


@pytest.mark.parametrize("value", ["linux", "macos-arm64", "macos-x86_64"])
def test_download_rejects_unknown_platform(db_client, value: str):
    response = db_client.get(
        f"{helpers.API}/downloads/game", params={"platform": value}
    )

    assert response.status_code == 422, value


# ---------------------------------------------------------------------------
# Landing payload
# ---------------------------------------------------------------------------
def test_landing_keeps_windows_download_and_adds_per_platform_map(
    db_client, admin_user
):
    windows = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0.zip",
        platform="windows",
        version="1.0.0",
        payload=WINDOWS_BYTES,
    )
    _activate(db_client, admin_user, windows["id"])
    mac = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-universal.dmg",
        platform="macos",
        version="1.0.0",
        payload=MAC_BYTES,
    )
    _activate(db_client, admin_user, mac["id"])

    payload = _landing(db_client)

    # Backwards compatible field: the Windows build, no query string.
    download = payload["download"]
    assert download["available"] is True
    assert download["version"] == "1.0.0"
    assert download["filename"] == "GuitarBattle-1.0.0.zip"
    assert download["url"] == "/api/v1/downloads/game"

    downloads = payload["downloads"]
    assert set(downloads) == {"windows", "macos"}
    assert downloads["windows"]["available"] is True
    assert downloads["windows"]["url"] == "/api/v1/downloads/game"
    assert downloads["macos"]["available"] is True
    assert downloads["macos"]["filename"] == "GuitarBattle-1.0.0-universal.dmg"
    assert downloads["macos"]["url"] == "/api/v1/downloads/game?platform=macos"


def test_landing_without_builds_has_empty_downloads(db_client):
    payload = _landing(db_client)

    assert payload["download"]["available"] is False
    assert payload["download"]["url"] == "/api/v1/downloads/game"
    assert set(payload["downloads"]) == {"windows", "macos"}
    assert all(
        info is None or info["available"] is False
        for info in payload["downloads"].values()
    )


# ---------------------------------------------------------------------------
# Landing page markup and dictionaries
# ---------------------------------------------------------------------------
MACOS_LABELS = {"ru": "Скачать для macOS", "en": "Download for macOS"}


@pytest.mark.parametrize("lang,path", [("ru", "/"), ("en", "/en")])
def test_landing_page_has_the_macos_download_button(client, lang: str, path: str):
    html = client.get(path).text

    # One plain link next to the Windows button, no architecture picker.
    assert "data-download-macos" in html
    assert 'href="/api/v1/downloads/game?platform=macos"' in html
    assert MACOS_LABELS[lang] in html
    assert 'data-i18n="landing.downloadMac"' in html

    for gone in (
        "data-macos-open",
        "macDownloadModal",
        'aria-modal="true"',
        "data-mac-option",
        "macos-arm64",
        "macos-x86_64",
        "Apple Silicon (M1",
    ):
        assert gone not in html, gone


def test_macos_copy_exists_in_both_dictionaries():
    from app.services.landing_content import LANDING_TEXT

    keys = ("landing.downloadMac", "landing.mac.soon")
    browser = (STATIC_DIR / "app" / "landing-i18n.js").read_text(encoding="utf-8")
    for key in keys:
        for lang in ("ru", "en"):
            assert key in LANDING_TEXT[lang], f"{lang}/{key} is missing on the server"
        assert f"'{key}':" in browser, f"{key} is missing in landing-i18n.js"

    assert LANDING_TEXT["ru"]["landing.downloadMac"] == "Скачать для macOS"
    assert LANDING_TEXT["en"]["landing.downloadMac"] == "Download for macOS"
    assert LANDING_TEXT["ru"]["landing.mac.soon"] == "Скоро"
    assert LANDING_TEXT["en"]["landing.mac.soon"] == "Coming soon"

    # The architecture picker copy is gone with the modal.
    for lang in ("ru", "en"):
        for removed in (
            "landing.mac.title",
            "landing.mac.text",
            "landing.mac.arm64",
            "landing.mac.intel",
            "landing.mac.close",
        ):
            assert removed not in LANDING_TEXT[lang], f"{lang}/{removed} is stale"
