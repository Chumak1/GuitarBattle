"""Public client asset manifest and bundle endpoints.

These tests do not need the database: the router only serves two generated
files from ``settings.client_assets_dir``.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from app.config import settings

PREFIX = "/api/v1/client-assets"
BUNDLE_BYTES = b"PK\x03\x04fake-guitarbattle-bundle"


def _manifest_payload(bundle: bytes) -> dict[str, object]:
    archive_sha = hashlib.sha256(bundle).hexdigest()
    sample = b"fake-wav"
    return {
        "schema": 1,
        "version": archive_sha,
        "archive_url": f"{PREFIX}/bundle.zip",
        "archive_size_bytes": len(bundle),
        "archive_sha256": archive_sha,
        "files": [
            {
                "path": "metronome/wooden-metronome-click.wav",
                "size_bytes": len(sample),
                "sha256": hashlib.sha256(sample).hexdigest(),
            }
        ],
    }


@pytest.fixture()
def assets_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.setattr(settings, "client_assets_dir", tmp_path)
    return tmp_path


def _write_assets(directory: Path) -> dict[str, object]:
    manifest = _manifest_payload(BUNDLE_BYTES)
    (directory / "bundle.zip").write_bytes(BUNDLE_BYTES)
    (directory / "manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8"
    )
    return manifest


def test_manifest_is_served(client, assets_dir: Path):
    manifest = _write_assets(assets_dir)

    response = client.get(f"{PREFIX}/manifest")

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("application/json")
    assert response.headers["cache-control"] == "no-cache"
    assert response.json() == manifest


def test_manifest_has_expected_contract_fields(client, assets_dir: Path):
    _write_assets(assets_dir)

    body = client.get(f"{PREFIX}/manifest").json()

    assert set(body) == {
        "schema",
        "version",
        "archive_url",
        "archive_size_bytes",
        "archive_sha256",
        "files",
    }
    assert body["schema"] == 1
    assert body["version"] == body["archive_sha256"]
    assert body["archive_url"] == f"{PREFIX}/bundle.zip"
    assert body["archive_size_bytes"] == len(BUNDLE_BYTES)
    assert len(body["version"]) == 64
    assert body["version"] == body["version"].lower()
    assert body["files"] == [
        {
            "path": "metronome/wooden-metronome-click.wav",
            "size_bytes": body["files"][0]["size_bytes"],
            "sha256": body["files"][0]["sha256"],
        }
    ]
    assert set(body["files"][0]) == {"path", "size_bytes", "sha256"}


def test_bundle_is_served_as_zip_download(client, assets_dir: Path):
    _write_assets(assets_dir)

    response = client.get(f"{PREFIX}/bundle.zip")

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("application/zip")
    assert response.headers["cache-control"] == "no-cache"
    assert 'filename="guitarbattle-sounds.zip"' in response.headers["content-disposition"]
    assert response.content == BUNDLE_BYTES


def test_bundle_head_reports_size_without_body(client, assets_dir: Path):
    _write_assets(assets_dir)

    response = client.head(f"{PREFIX}/bundle.zip")

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("application/zip")
    assert int(response.headers["content-length"]) == len(BUNDLE_BYTES)
    assert response.content == b""


def test_manifest_returns_503_when_missing(client, assets_dir: Path):
    response = client.get(f"{PREFIX}/manifest")

    assert response.status_code == 503
    assert response.json()["detail"] == "Client asset manifest is not available"


def test_bundle_returns_503_when_missing(client, assets_dir: Path):
    (assets_dir / "manifest.json").write_text("{}", encoding="utf-8")

    response = client.get(f"{PREFIX}/bundle.zip")

    assert response.status_code == 503
    assert response.json()["detail"] == "Client asset bundle is not available"


def test_manifest_returns_503_when_bundle_is_missing(client, assets_dir: Path):
    manifest = _manifest_payload(BUNDLE_BYTES)
    (assets_dir / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    response = client.get(f"{PREFIX}/manifest")

    assert response.status_code == 503
    assert response.json()["detail"] == "Client asset bundle is not available"


def test_asset_path_cannot_escape_assets_dir(assets_dir: Path):
    """Filenames are fixed, but the containment guard must still reject escape."""
    from fastapi import HTTPException

    from app.api.v1.client_assets import _asset_path

    with pytest.raises(HTTPException) as excinfo:
        _asset_path("../outside.txt")

    assert excinfo.value.status_code == 503
