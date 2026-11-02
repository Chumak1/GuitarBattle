"""Two artifacts per platform: installers for the site, portable ZIPs for updates.

A build now belongs to a ``(platform, artifact)`` pair:

* ``installer`` — what the public download button installs (Windows
  ``.exe``/``.msi``, macOS ``.dmg``, legacy ``.zip``). Served by
  ``GET /api/v1/downloads/game``;
* ``portable`` — the ZIP the installed client unpacks to update itself, served
  by the new ``GET /api/v1/downloads/game/update?platform=...`` and exposed as
  the additive ``updates`` map of ``GET /api/v1/landing``.

These tests pin the contract end to end: the migration backfill, per-pair
upload validation, activation isolation between the two artifacts, both public
file endpoints, the landing payload, the admin filters and the admin UI markup.
"""
from __future__ import annotations

from pathlib import Path

import pytest
from alembic import command
from alembic.config import Config
from sqlalchemy import text
from sqlalchemy.exc import IntegrityError

from app.schemas.site import DownloadInfo, GameBuildItem, LandingResponse
from app.services.game_builds import (
    BUILD_ARTIFACTS,
    DEFAULT_BUILD_ARTIFACT,
    allowed_extensions,
    artifact_check_sql,
    content_type_for,
    normalize_artifact,
    requirements_text,
)
from tests import helpers
from tests.conftest import SERVER_ROOT, TEST_DATABASE_DSN

WINDOWS_BYTES = b"MZ" + b"w" * 256
PORTABLE_BYTES = b"PK\x03\x04" + b"p" * 256
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
    artifact: str | None = None,
    version: str = "1.0.0",
    payload: bytes = PORTABLE_BYTES,
):
    data = {"version": version}
    if platform is not None:
        data["platform"] = platform
    if artifact is not None:
        data["artifact"] = artifact
    return client.post(
        f"{helpers.API}/admin/site/game-builds",
        data=data,
        files={"file": (filename, payload, "application/octet-stream")},
        headers=admin_user["headers"],
    )


def _upload_ok(client, admin_user, *, filename: str, **kwargs):
    response = _upload(client, admin_user, filename=filename, **kwargs)
    assert response.status_code == 201, response.text
    return response.json()


def _activate(client, admin_user, build_id: int):
    response = client.post(
        f"{helpers.API}/admin/site/game-builds/{build_id}/activate",
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    return response.json()


def _admin_builds(
    client,
    admin_user,
    *,
    platform: str | None = None,
    artifact: str | None = None,
):
    query = []
    if platform is not None:
        query.append(f"platform={platform}")
    if artifact is not None:
        query.append(f"artifact={artifact}")
    url = f"{helpers.API}/admin/site/game-builds"
    if query:
        url += "?" + "&".join(query)
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
# Unit tests: the per-pair extension rules (no database)
# ---------------------------------------------------------------------------
def test_artifact_constants_match_the_documented_contract():
    assert BUILD_ARTIFACTS == ("installer", "portable")
    assert DEFAULT_BUILD_ARTIFACT == "installer"
    assert artifact_check_sql() == "artifact IN ('installer', 'portable')"


@pytest.mark.parametrize(
    ("platform", "artifact", "expected"),
    [
        ("windows", "installer", (".exe", ".msi", ".zip")),
        ("macos", "installer", (".dmg", ".zip")),
        ("windows", "portable", (".zip",)),
        ("macos", "portable", (".zip",)),
        ("windows", None, (".exe", ".msi", ".zip")),
        ("linux", "installer", ()),
    ],
)
def test_allowed_extensions_depend_on_platform_and_artifact(
    platform, artifact, expected
):
    if artifact is None:
        assert allowed_extensions(platform) == expected
    else:
        assert allowed_extensions(platform, artifact) == expected


@pytest.mark.parametrize(
    ("platform", "artifact", "suffix"),
    [
        ("windows", "installer", ".exe"),
        ("windows", "installer", ".msi"),
        ("windows", "installer", ".zip"),
        ("macos", "installer", ".dmg"),
        ("macos", "installer", ".zip"),
        ("windows", "portable", ".zip"),
        ("macos", "portable", ".zip"),
    ],
)
def test_content_type_accepts_documented_pairs(platform, artifact, suffix):
    assert content_type_for(platform, suffix, artifact)


@pytest.mark.parametrize(
    ("platform", "artifact", "suffix"),
    [
        ("windows", "portable", ".exe"),
        ("windows", "portable", ".msi"),
        ("macos", "portable", ".dmg"),
        ("windows", "installer", ".dmg"),
        ("macos", "installer", ".exe"),
    ],
)
def test_content_type_rejects_wrong_artifact_pair(platform, artifact, suffix):
    assert content_type_for(platform, suffix, artifact) is None


def test_normalize_artifact_canonicalizes_and_rejects_unknown():
    assert normalize_artifact(" Portable ") == "portable"
    assert normalize_artifact("INSTALLER") == "installer"
    assert normalize_artifact("") == ""
    assert normalize_artifact(None) == ""
    assert normalize_artifact("update") == ""


def test_requirements_text_names_the_rejected_pair():
    windows = requirements_text("windows", "portable")
    macos = requirements_text("macos", "portable")

    assert windows == "Windows portable ZIP must be .zip"
    assert macos == "macOS portable ZIP must be .zip"
    assert ".exe" in requirements_text("windows", "installer")


def test_game_build_item_artifact_defaults_to_installer():
    item = GameBuildItem.model_validate(
        {
            "id": 1,
            "platform": "windows",
            "filename": "GuitarBattle.exe",
            "size_bytes": 10,
            "content_type": "application/octet-stream",
            "is_active": True,
            "uploaded_at": "2026-01-01T00:00:00Z",
            "url": "/api/v1/downloads/game",
        }
    )

    assert item.artifact == "installer"


def test_landing_response_updates_defaults_to_empty_map():
    landing = LandingResponse()

    assert landing.updates.windows is None
    assert landing.updates.macos is None
    # DownloadInfo keeps its shape: no fields were added or renamed.
    assert set(DownloadInfo.model_fields) == {
        "available",
        "version",
        "filename",
        "size_bytes",
        "url",
        "release_notes",
    }


# ---------------------------------------------------------------------------
# Migration
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_migration_backfills_existing_builds_as_installers(db_engine, clean_db):
    """Rows written before the split are installers and stay published."""
    config = _alembic_config()
    try:
        command.downgrade(config, "0017_game_build_platform")
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
            assert "artifact" not in columns

            connection.execute(
                text(
                    """
                    INSERT INTO game_builds (
                        platform, version, release_notes, filename, stored_name,
                        content_type, size_bytes, is_active
                    )
                    VALUES (
                        'macos', '0.9.0', 'Legacy build',
                        'GuitarBattle-0.9.0.dmg', 'build-legacy-artifact.dmg',
                        'application/x-apple-diskimage', 1024, true
                    )
                    """
                )
            )

        command.upgrade(config, "head")

        with db_engine.connect() as connection:
            row = connection.execute(
                text(
                    "SELECT artifact, platform, is_active FROM game_builds "
                    "WHERE stored_name = 'build-legacy-artifact.dmg'"
                )
            ).one()
            constraint = connection.execute(
                text(
                    "SELECT pg_get_constraintdef(oid) FROM pg_constraint "
                    "WHERE conname = 'ck_game_builds_artifact'"
                )
            ).scalar_one()
            index = connection.execute(
                text(
                    "SELECT indexname FROM pg_indexes "
                    "WHERE tablename = 'game_builds' "
                    "AND indexname = 'ix_game_builds_artifact'"
                )
            ).scalar_one_or_none()

        assert row.artifact == "installer"
        assert row.platform == "macos"
        # The migration never deactivates an existing build.
        assert row.is_active is True
        assert "'installer'" in constraint and "'portable'" in constraint
        assert "ix" in index
    finally:
        command.upgrade(config, "head")
        with db_engine.begin() as connection:
            connection.execute(
                text(
                    "DELETE FROM game_builds "
                    "WHERE stored_name = 'build-legacy-artifact.dmg'"
                )
            )


@pytest.mark.db
def test_migration_rejects_unknown_artifact(db_engine, clean_db):
    with pytest.raises(IntegrityError):
        with db_engine.begin() as connection:
            connection.execute(
                text(
                    """
                    INSERT INTO game_builds (
                        platform, artifact, version, release_notes, filename,
                        stored_name, content_type, size_bytes, is_active
                    )
                    VALUES (
                        'windows', 'update', '1.0.0', '', 'GuitarBattle.zip',
                        'build-unknown-artifact.zip', 'application/zip', 1024, false
                    )
                    """
                )
            )


# ---------------------------------------------------------------------------
# Upload validation per (platform, artifact)
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_portable_zip_is_accepted_for_windows(db_client, admin_user):
    build = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-win64-portable.zip",
        platform="windows",
        artifact="portable",
        payload=PORTABLE_BYTES,
    )

    assert build["artifact"] == "portable"
    assert build["platform"] == "windows"
    assert build["content_type"] == "application/zip"
    assert build["is_active"] is False
    # A portable row must advertise the update endpoint, not the installer.
    assert build["url"] == "/api/v1/downloads/game/update?platform=windows"


@pytest.mark.db
def test_portable_zip_is_accepted_for_macos(db_client, admin_user):
    build = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-macos-portable.zip",
        platform="macos",
        artifact="portable",
        payload=PORTABLE_BYTES,
    )

    assert build["artifact"] == "portable"
    assert build["content_type"] == "application/zip"
    assert build["url"] == "/api/v1/downloads/game/update?platform=macos"


@pytest.mark.db
def test_upload_without_artifact_is_an_installer(db_client, admin_user):
    build = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-Setup-1.0.0.exe",
        platform="windows",
        payload=WINDOWS_BYTES,
    )

    assert build["artifact"] == "installer"
    assert build["url"] == "/api/v1/downloads/game"


@pytest.mark.db
@pytest.mark.parametrize(
    ("platform", "filename", "payload"),
    [
        ("windows", "GuitarBattle-Setup-1.0.0.exe", WINDOWS_BYTES),
        ("windows", "GuitarBattle-Setup-1.0.0.msi", WINDOWS_BYTES),
        ("macos", "GuitarBattle-1.0.0-universal.dmg", MAC_BYTES),
    ],
)
def test_portable_rejects_installer_extensions(
    db_client, admin_user, platform: str, filename: str, payload: bytes
):
    response = _upload(
        db_client,
        admin_user,
        filename=filename,
        platform=platform,
        artifact="portable",
        payload=payload,
    )

    assert response.status_code == 415, response.text
    assert ".zip" in response.json()["detail"]
    # Nothing was stored for a rejected pair.
    assert _admin_builds(db_client, admin_user) == []


@pytest.mark.db
def test_upload_rejects_unknown_artifact(db_client, admin_user):
    response = _upload(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0.zip",
        platform="windows",
        artifact="delta",
    )

    assert response.status_code == 422, response.text
    assert "artifact" in response.json()["detail"]


# ---------------------------------------------------------------------------
# Activation is per (platform, artifact)
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_activating_portable_keeps_the_installer_active(db_client, admin_user):
    installer = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-Setup-1.0.0.exe",
        platform="windows",
        artifact="installer",
        payload=WINDOWS_BYTES,
    )
    portable = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-win64-portable.zip",
        platform="windows",
        artifact="portable",
        payload=PORTABLE_BYTES,
    )
    _activate(db_client, admin_user, installer["id"])

    activated = _activate(db_client, admin_user, portable["id"])

    assert activated["is_active"] is True
    assert _active_ids(db_client, admin_user) == {installer["id"], portable["id"]}


@pytest.mark.db
def test_activating_installer_keeps_the_portable_build_active(db_client, admin_user):
    portable = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-macos-portable.zip",
        platform="macos",
        artifact="portable",
        payload=PORTABLE_BYTES,
    )
    installer = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-universal.dmg",
        platform="macos",
        artifact="installer",
        payload=MAC_BYTES,
    )
    _activate(db_client, admin_user, portable["id"])

    _activate(db_client, admin_user, installer["id"])

    assert _active_ids(db_client, admin_user) == {portable["id"], installer["id"]}


@pytest.mark.db
def test_only_one_active_portable_build_per_platform(db_client, admin_user):
    first = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-win64-portable.zip",
        platform="windows",
        version="1.0.0",
    )
    second = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.1.0-win64-portable.zip",
        platform="windows",
        version="1.1.0",
    )
    installer = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-Setup-1.0.0.exe",
        platform="windows",
        artifact="installer",
        payload=WINDOWS_BYTES,
    )
    _activate(db_client, admin_user, first["id"])
    _activate(db_client, admin_user, installer["id"])

    _activate(db_client, admin_user, second["id"])

    active = _active_ids(db_client, admin_user)
    assert first["id"] not in active
    assert second["id"] in active
    # The installer of the same platform keeps its own active slot.
    assert installer["id"] in active


# ---------------------------------------------------------------------------
# Public downloads
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_download_game_serves_the_installer_even_with_a_portable_build(
    db_client, admin_user
):
    installer = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-Setup-1.0.0.exe",
        platform="windows",
        artifact="installer",
        payload=WINDOWS_BYTES,
    )
    portable = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-win64-portable.zip",
        platform="windows",
        artifact="portable",
        payload=PORTABLE_BYTES,
    )
    _activate(db_client, admin_user, installer["id"])
    _activate(db_client, admin_user, portable["id"])

    response = db_client.get(f"{helpers.API}/downloads/game")

    assert response.status_code == 200
    assert response.content == WINDOWS_BYTES
    assert "GuitarBattle-Setup-1.0.0.exe" in response.headers["content-disposition"]


@pytest.mark.db
def test_download_update_serves_the_portable_zip(db_client, admin_user):
    mac_installer = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-universal.dmg",
        platform="macos",
        artifact="installer",
        payload=MAC_BYTES,
    )
    portable = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-macos-portable.zip",
        platform="macos",
        artifact="portable",
        payload=PORTABLE_BYTES,
    )
    _activate(db_client, admin_user, mac_installer["id"])
    _activate(db_client, admin_user, portable["id"])

    response = db_client.get(
        f"{helpers.API}/downloads/game/update", params={"platform": "macos"}
    )

    assert response.status_code == 200
    assert response.content == PORTABLE_BYTES
    assert response.headers["content-type"] == "application/zip"
    assert "GuitarBattle-1.0.0-macos-portable.zip" in response.headers[
        "content-disposition"
    ]


@pytest.mark.db
def test_download_update_without_portable_build_is_404(db_client, admin_user):
    installer = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-Setup-1.0.0.exe",
        platform="windows",
        artifact="installer",
        payload=WINDOWS_BYTES,
    )
    _activate(db_client, admin_user, installer["id"])

    response = db_client.get(f"{helpers.API}/downloads/game/update")

    assert response.status_code == 404
    assert "windows" in response.json()["detail"]


@pytest.mark.db
def test_download_update_rejects_unknown_platform(db_client):
    response = db_client.get(
        f"{helpers.API}/downloads/game/update", params={"platform": "linux"}
    )

    assert response.status_code == 422


# ---------------------------------------------------------------------------
# Landing payload
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_landing_separates_installers_from_portable_updates(db_client, admin_user):
    windows_installer = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-Setup-1.0.0.exe",
        platform="windows",
        artifact="installer",
        version="1.0.0",
        payload=WINDOWS_BYTES,
    )
    windows_portable = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.1-win64-portable.zip",
        platform="windows",
        artifact="portable",
        version="1.0.1",
        payload=PORTABLE_BYTES,
    )
    mac_portable = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.1-macos-portable.zip",
        platform="macos",
        artifact="portable",
        version="1.0.1",
        payload=PORTABLE_BYTES,
    )
    _activate(db_client, admin_user, windows_installer["id"])
    _activate(db_client, admin_user, windows_portable["id"])
    _activate(db_client, admin_user, mac_portable["id"])

    payload = _landing(db_client)

    # The site button keeps the installer, both in the legacy and the map field.
    download = payload["download"]
    assert download["available"] is True
    assert download["filename"] == "GuitarBattle-Setup-1.0.0.exe"
    assert download["url"] == "/api/v1/downloads/game"
    assert payload["downloads"]["windows"]["filename"] == "GuitarBattle-Setup-1.0.0.exe"
    assert payload["downloads"]["macos"]["available"] is False

    # The client reads the portable channel from the additive `updates` map.
    updates = payload["updates"]
    assert set(updates) == {"windows", "macos"}
    assert updates["windows"]["available"] is True
    assert updates["windows"]["version"] == "1.0.1"
    assert updates["windows"]["filename"] == "GuitarBattle-1.0.1-win64-portable.zip"
    assert updates["windows"]["url"] == (
        "/api/v1/downloads/game/update?platform=windows"
    )
    assert updates["macos"]["available"] is True
    assert updates["macos"]["url"] == "/api/v1/downloads/game/update?platform=macos"


@pytest.mark.db
def test_landing_updates_are_unavailable_without_portable_build(db_client, admin_user):
    installer = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-Setup-1.0.0.exe",
        platform="windows",
        artifact="installer",
        payload=WINDOWS_BYTES,
    )
    _activate(db_client, admin_user, installer["id"])

    payload = _landing(db_client)

    assert payload["download"]["available"] is True
    assert payload["updates"]["windows"]["available"] is False
    assert payload["updates"]["windows"]["url"] == (
        "/api/v1/downloads/game/update?platform=windows"
    )
    assert payload["updates"]["macos"]["available"] is False


# ---------------------------------------------------------------------------
# Admin list
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_admin_list_filters_by_platform_and_artifact(db_client, admin_user):
    windows_installer = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-Setup-1.0.0.exe",
        platform="windows",
        artifact="installer",
        payload=WINDOWS_BYTES,
    )
    windows_portable = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-win64-portable.zip",
        platform="windows",
        artifact="portable",
    )
    mac_portable = _upload_ok(
        db_client,
        admin_user,
        filename="GuitarBattle-1.0.0-macos-portable.zip",
        platform="macos",
        artifact="portable",
    )

    everything = _admin_builds(db_client, admin_user)
    assert {item["artifact"] for item in everything} == {"installer", "portable"}

    assert [item["id"] for item in _admin_builds(db_client, admin_user, artifact="installer")] == [
        windows_installer["id"]
    ]
    assert {
        item["id"]
        for item in _admin_builds(db_client, admin_user, artifact="portable")
    } == {windows_portable["id"], mac_portable["id"]}
    assert {
        item["id"]
        for item in _admin_builds(
            db_client, admin_user, platform="windows", artifact="portable"
        )
    } == {windows_portable["id"]}
    assert _admin_builds(
        db_client, admin_user, platform="macos", artifact="installer"
    ) == []


@pytest.mark.db
def test_admin_list_rejects_unknown_artifact_filter(db_client, admin_user):
    response = db_client.get(
        f"{helpers.API}/admin/site/game-builds?artifact=delta",
        headers=admin_user["headers"],
    )

    assert response.status_code == 422


# ---------------------------------------------------------------------------
# Admin UI markup and dictionary (static, no database)
# ---------------------------------------------------------------------------
def _static(path: str) -> str:
    return (STATIC_DIR / path).read_text(encoding="utf-8")


def test_admin_form_has_platform_and_artifact_selectors():
    html = _static("admin/index.html")

    assert 'id="buildPlatform"' in html
    assert 'id="buildArtifact"' in html
    assert "Установщик" in html
    assert "Portable ZIP для автообновления" in html
    assert 'value="portable"' in html
    # The initial accept attribute describes the default pair: Windows installer.
    assert 'accept=".exe,.msi,.zip"' in html
    assert 'id="buildFileHint"' in html


def test_admin_table_has_the_artifact_column():
    html = _static("admin/index.html")

    assert '<th scope="col">Артефакт</th>' in html
    # A new cell means the loading row spans one more column.
    assert 'colspan="9"' in html


def test_admin_filter_has_the_artifact_selector():
    html = _static("admin/index.html")

    assert 'id="buildsArtifactFilter"' in html
    assert "Все артефакты" in html


def test_admin_script_drives_accept_hints_and_badges_by_pair():
    script = _static("admin/admin-site.js")

    assert "const BUILD_ARTIFACTS" in script
    assert "badge-artifact-installer" in script
    assert "badge-artifact-portable" in script
    # accept/hint come from the platform + artifact pair, not from the platform.
    assert "artifact.accept[platform.value]" in script
    assert "artifact.extensions[platform.value]" in script
    assert 'input.setAttribute("data-artifact", artifact.value)' in script
    # The upload and the list filters send the artifact.
    assert 'form.append("artifact", artifact.value)' in script
    assert 'params.set("artifact", state.buildsArtifact)' in script
    # The summary reports installer and portable separately.
    assert "perArtifact" in script
    assert "artifact.short + " in script
