"""Per-song Preview Song (the full mix) contract.

Preview lives in ``song_files`` with ``role = "preview"`` but is outside
``ASSET_ROLES``: it never changes readiness, publish requirements, scoring or
the gameplay asset list. Only the additive ``preview_url`` field exposes it.
"""
import uuid
from pathlib import Path

import pytest

from app.config import settings
from tests import helpers

ADMIN_STATIC_DIR = Path(__file__).resolve().parents[1] / "app" / "static" / "admin"


def _upload_preview(client, admin_user, song_id, *, marker=0x11, size=256, name="preview.mp3"):
    data = helpers.build_mp3_bytes(marker=marker, size=size)
    response = client.post(
        f"{helpers.API}/admin/songs/{song_id}/files/audio/preview",
        files={"file": (name, data, "audio/mpeg")},
        headers=admin_user["headers"],
    )
    assert response.status_code == 201, response.text
    return response.json(), data


def _preview_url_for(slug, file_id):
    return f"http://testserver{helpers.API}/songs/{slug}/files/{file_id}/download"


@pytest.mark.db
def test_preview_absent_is_null(db_client, published_song):
    detail = db_client.get(f"{helpers.API}/songs/{published_song['slug']}")
    assert detail.status_code == 200
    assert detail.json()["preview_url"] is None

    listing = db_client.get(f"{helpers.API}/songs")
    assert listing.status_code == 200
    assert listing.json()["items"][0]["preview_url"] is None


@pytest.mark.db
def test_preview_upload_replace_and_public_url(
    db_client, admin_user, published_song, song_storage
):
    first, first_bytes = _upload_preview(db_client, admin_user, published_song["id"])
    assert first["role"] == "preview"
    assert first["content_type"] == "audio/mpeg"
    assert (song_storage / first["object_key"]).is_file()

    detail = db_client.get(f"{helpers.API}/songs/{published_song['slug']}")
    assert detail.status_code == 200
    body = detail.json()
    assert body["preview_url"] == _preview_url_for(published_song["slug"], first["id"])
    # The preview is not part of the gameplay asset list.
    assert body["files"] == []
    assert body["assets"]["midi"] is None
    assert body["readiness"]["available_roles"] == []

    listing = db_client.get(f"{helpers.API}/songs").json()
    assert listing["items"][0]["preview_url"] == body["preview_url"]

    download = db_client.get(body["preview_url"])
    assert download.status_code == 200
    assert download.content == first_bytes

    # Replacing the preview publishes the new version and archives the old one.
    second, second_bytes = _upload_preview(
        db_client, admin_user, published_song["id"], marker=0x22, size=320
    )
    detail = db_client.get(f"{helpers.API}/songs/{published_song['slug']}").json()
    assert detail["preview_url"] == _preview_url_for(published_song["slug"], second["id"])
    assert db_client.get(detail["preview_url"]).content == second_bytes

    admin_detail = db_client.get(
        f"{helpers.API}/admin/songs/{published_song['id']}",
        headers=admin_user["headers"],
    )
    assert admin_detail.status_code == 200
    assert admin_detail.json()["assets"]["preview"]["id"] == second["id"]

    versions = [
        item for item in admin_detail.json()["files"] if item["role"] == "preview"
    ]
    assert [item["id"] for item in versions] == [second["id"], first["id"]]
    assert [item["is_active"] for item in versions] == [True, False]

    # Deleting removes every preview version and clears the public field.
    deleted = db_client.delete(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/preview",
        headers=admin_user["headers"],
    )
    assert deleted.status_code == 204
    detail = db_client.get(f"{helpers.API}/songs/{published_song['slug']}").json()
    assert detail["preview_url"] is None
    assert db_client.get(f"{helpers.API}/songs").json()["items"][0]["preview_url"] is None
    assert not (song_storage / first["object_key"]).exists()
    assert not (song_storage / second["object_key"]).exists()

    # Idempotent delete.
    again = db_client.delete(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/preview",
        headers=admin_user["headers"],
    )
    assert again.status_code == 204


@pytest.mark.db
def test_preview_does_not_affect_readiness_or_publish(
    db_client, admin_user, published_song
):
    _upload_preview(db_client, admin_user, published_song["id"])
    midi = helpers.build_midi_bytes(bpm=120, note=64)
    upload = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/midi",
        files={"file": ("song.mid", midi, "audio/midi")},
        headers=admin_user["headers"],
    )
    assert upload.status_code == 201, upload.text

    readiness = db_client.get(
        f"{helpers.API}/songs/{published_song['slug']}"
    ).json()["readiness"]
    assert readiness["state"] == "midi_only"
    assert readiness["available_roles"] == ["midi"]
    assert readiness["missing_roles"] == ["drums", "bass", "guitar", "others"]
    assert readiness["is_playable"] is True
    assert readiness["publish_ready"] is False

    published = helpers.publish_song(
        db_client, admin_user["headers"], published_song["id"]
    )
    assert published["is_published"] is True
    assert published["readiness"]["publish_ready"] is False

    readiness_endpoint = db_client.get("/health/ready")
    assert readiness_endpoint.status_code == 200


@pytest.mark.db
def test_preview_download_is_not_public_for_archived_or_missing(
    db_client, admin_user, published_song
):
    uploaded, _ = _upload_preview(db_client, admin_user, published_song["id"])
    unknown = db_client.get(
        f"{helpers.API}/songs/{published_song['slug']}/files/"
        f"{uuid.uuid4()}/download"
    )
    assert unknown.status_code == 404

    db_client.delete(
        f"{helpers.API}/admin/songs/{published_song['id']}",
        headers=admin_user["headers"],
    )
    archived = db_client.get(
        f"{helpers.API}/songs/{published_song['slug']}/files/{uploaded['id']}/download"
    )
    assert archived.status_code == 404


@pytest.mark.db
def test_preview_does_not_bump_gameplay_asset_version(
    db_client, admin_user, published_song
):
    before = db_client.get(f"{helpers.API}/songs/{published_song['slug']}").json()["version"]

    _upload_preview(db_client, admin_user, published_song["id"])
    assert db_client.get(f"{helpers.API}/songs/{published_song['slug']}").json()["version"] == before

    db_client.delete(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/preview",
        headers=admin_user["headers"],
    )
    assert db_client.get(f"{helpers.API}/songs/{published_song['slug']}").json()["version"] == before

    # A gameplay stem still bumps the version used by the client asset cache.
    drum = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/audio/drums",
        files={"file": ("drums.mp3", helpers.build_mp3_bytes(marker=0x55), "audio/mpeg")},
        headers=admin_user["headers"],
    )
    assert drum.status_code == 201, drum.text
    assert db_client.get(f"{helpers.API}/songs/{published_song['slug']}").json()["version"] == before + 1


@pytest.mark.db
def test_preview_upload_validation(db_client, admin_user, published_song, monkeypatch):
    wrong_extension = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/audio/preview",
        files={"file": ("preview.wav", helpers.build_mp3_bytes(), "audio/mpeg")},
        headers=admin_user["headers"],
    )
    assert wrong_extension.status_code == 415

    garbage = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/audio/preview",
        files={"file": ("preview.mp3", b"not an mp3 file", "audio/mpeg")},
        headers=admin_user["headers"],
    )
    assert garbage.status_code == 422

    monkeypatch.setattr(settings, "max_audio_upload_bytes", 64)
    oversized = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/audio/preview",
        files={"file": ("preview.mp3", helpers.build_mp3_bytes(size=256), "audio/mpeg")},
        headers=admin_user["headers"],
    )
    assert oversized.status_code == 413
    assert "limit" in oversized.json()["detail"].lower()


@pytest.mark.db
def test_preview_upload_requires_admin(db_client, user_account, published_song):
    data = helpers.build_mp3_bytes()
    anonymous = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/audio/preview",
        files={"file": ("preview.mp3", data, "audio/mpeg")},
    )
    assert anonymous.status_code == 401

    regular = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/audio/preview",
        files={"file": ("preview.mp3", data, "audio/mpeg")},
        headers=user_account["headers"],
    )
    assert regular.status_code == 403

    delete = db_client.delete(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/preview",
        headers=user_account["headers"],
    )
    assert delete.status_code == 403


# ---------------------------------------------------------------------------
# Admin list/detail: additive ``preview_url`` used by the web admin player
# ---------------------------------------------------------------------------
def _admin_songs(client, admin_headers):
    response = client.get(f"{helpers.API}/admin/songs", headers=admin_headers)
    assert response.status_code == 200, response.text
    return response.json()


@pytest.mark.db
def test_admin_preview_url_follows_upload_and_delete(
    db_client, admin_user, published_song
):
    headers = admin_user["headers"]
    song_id = published_song["id"]

    listing = _admin_songs(db_client, headers)
    assert listing["items"][0]["preview_url"] is None

    uploaded, preview_bytes = _upload_preview(db_client, admin_user, song_id)
    expected = (
        f"{helpers.API}/admin/songs/{song_id}/files/{uploaded['id']}/download"
    )

    listing = _admin_songs(db_client, headers)
    assert listing["items"][0]["preview_url"] == expected

    detail = db_client.get(f"{helpers.API}/admin/songs/{song_id}", headers=headers)
    assert detail.status_code == 200, detail.text
    assert detail.json()["preview_url"] == expected
    # ``preview_url`` mirrors the existing file manager field.
    assert detail.json()["assets"]["preview"]["download_url"] == expected

    # The admin download route is the only way to play it: no anonymous access.
    assert db_client.get(expected).status_code == 401
    download = db_client.get(expected, headers=headers)
    assert download.status_code == 200, download.text
    assert download.content == preview_bytes

    deleted = db_client.delete(
        f"{helpers.API}/admin/songs/{song_id}/files/preview", headers=headers
    )
    assert deleted.status_code == 204
    assert _admin_songs(db_client, headers)["items"][0]["preview_url"] is None
    detail = db_client.get(f"{helpers.API}/admin/songs/{song_id}", headers=headers)
    assert detail.json()["preview_url"] is None


# ---------------------------------------------------------------------------
# Static assets: the songs page shows LVL and ships the inline player
# ---------------------------------------------------------------------------
def test_admin_songs_page_has_level_badge_and_inline_player():
    html = (ADMIN_STATIC_DIR / "index.html").read_text(encoding="utf-8")
    assert 'id="songPreviewPlayer"' in html
    assert 'id="songPreviewAudio"' in html
    assert 'id="songPreviewClose"' in html
    assert ">LVL</th>" in html
    # Songs table: Песня, Статус, Готовность, LVL, BPM, Размер, Длительность,
    # Обновлена, Действия.
    assert 'colspan="9"' in html

    # The player reports download progress (determinate or indeterminate).
    assert 'id="songPreviewProgress"' in html
    assert 'id="songPreviewProgressBar"' in html
    assert 'id="songPreviewProgressText"' in html
    assert 'class="progress"' in html

    script = (ADMIN_STATIC_DIR / "admin.js").read_text(encoding="utf-8")
    assert 'data-action="preview"' in script
    assert "preview_url" in script
    assert "songPreviewAudio" in script
    assert "URL.createObjectURL" in script
    assert "URL.revokeObjectURL" in script

    # Progress comes from XHR (fetch has no download progress) and is abortable.
    assert 'xhr.responseType = "blob"' in script
    assert 'addEventListener("progress"' in script
    assert "xhr.abort()" in script
    assert "songPreviewProgressBar" in script
    assert "songPreviewProgressText" in script
    assert "aria-valuenow" in script

    css = (ADMIN_STATIC_DIR / "admin.css").read_text(encoding="utf-8")
    assert ".badge-level" in css
    assert ".song-preview-progress" in css
