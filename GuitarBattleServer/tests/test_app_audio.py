"""Global background music: public descriptor, admin upload/replace/delete.

Background music is an application-wide asset, not a song asset. The public URL
is a stable route, so replacing the file must not change it or leave a broken
link.
"""
import hashlib

import pytest

from app.config import settings
from app.models.app_audio import AppAudioAsset
from tests import helpers

pytestmark = pytest.mark.db

PUBLIC = f"{helpers.API}/app/background-music"
ADMIN = f"{helpers.API}/admin/app/background-music"
FILE_URL = f"{helpers.API}/app/background-music/file"
EMPTY = {"url": None, "content_type": None, "size_bytes": 0}


def _upload(client, admin_user, *, marker=0x33, size=512, name="background.mp3"):
    data = helpers.build_mp3_bytes(marker=marker, size=size)
    response = client.post(
        ADMIN,
        files={"file": (name, data, "audio/mpeg")},
        headers=admin_user["headers"],
    )
    return response, data


def test_background_music_is_empty_by_default(db_client, admin_user):
    public = db_client.get(PUBLIC)
    assert public.status_code == 200
    assert public.json() == EMPTY

    admin = db_client.get(ADMIN, headers=admin_user["headers"])
    assert admin.status_code == 200
    assert admin.json()["url"] is None
    assert admin.json()["content_type"] is None
    assert admin.json()["size_bytes"] == 0
    assert admin.json()["uploaded_at"] is None

    # No file to stream yet; still a clean 404, not a crash.
    assert db_client.get(FILE_URL).status_code == 404


def test_background_music_requires_admin(db_client, user_account):
    data = helpers.build_mp3_bytes()
    assert db_client.get(ADMIN).status_code == 401
    assert (
        db_client.post(ADMIN, files={"file": ("bg.mp3", data, "audio/mpeg")}).status_code
        == 401
    )
    assert db_client.delete(ADMIN).status_code == 401

    assert db_client.get(ADMIN, headers=user_account["headers"]).status_code == 403
    upload = db_client.post(
        ADMIN,
        files={"file": ("bg.mp3", data, "audio/mpeg")},
        headers=user_account["headers"],
    )
    assert upload.status_code == 403
    assert db_client.delete(ADMIN, headers=user_account["headers"]).status_code == 403


def test_background_music_upload_replace_delete(
    db_client, admin_user, db_session, app_audio_storage
):
    created, first_bytes = _upload(db_client, admin_user)
    assert created.status_code == 201, created.text
    body = created.json()
    assert body["url"] == f"http://testserver{FILE_URL}"
    assert body["content_type"] == "audio/mpeg"
    assert body["size_bytes"] == len(first_bytes)
    assert body["uploaded_at"] is not None

    public = db_client.get(PUBLIC)
    assert public.status_code == 200
    assert public.json() == {
        "url": body["url"],
        "content_type": "audio/mpeg",
        "size_bytes": len(first_bytes),
    }
    streamed = db_client.get(FILE_URL)
    assert streamed.status_code == 200
    assert streamed.content == first_bytes
    assert streamed.headers["content-type"] == "audio/mpeg"

    first_asset = db_session.query(AppAudioAsset).one()
    first_key = first_asset.object_key
    assert first_asset.sha256 == hashlib.sha256(first_bytes).hexdigest()
    assert (app_audio_storage / first_key).is_file()

    # Replacement keeps the public URL stable, swaps the bytes and removes the
    # previous file only after the new row is committed.
    replaced, second_bytes = _upload(db_client, admin_user, marker=0x44, size=700)
    assert replaced.status_code == 200, replaced.text
    replaced_body = replaced.json()
    assert replaced_body["url"] == body["url"]
    assert replaced_body["size_bytes"] == len(second_bytes)
    assert db_client.get(FILE_URL).content == second_bytes

    db_session.expire_all()
    second_asset = db_session.query(AppAudioAsset).one()
    assert second_asset.object_key != first_key
    assert (app_audio_storage / second_asset.object_key).is_file()
    assert not (app_audio_storage / first_key).exists()

    deleted = db_client.delete(ADMIN, headers=admin_user["headers"])
    assert deleted.status_code == 204
    assert db_client.get(PUBLIC).json() == EMPTY
    assert db_client.get(FILE_URL).status_code == 404
    assert db_session.query(AppAudioAsset).count() == 0
    assert not (app_audio_storage / second_asset.object_key).exists()

    # Idempotent delete.
    assert db_client.delete(ADMIN, headers=admin_user["headers"]).status_code == 204


def test_background_music_validation(db_client, admin_user, monkeypatch):
    wrong_extension = db_client.post(
        ADMIN,
        files={"file": ("bg.wav", helpers.build_mp3_bytes(), "audio/mpeg")},
        headers=admin_user["headers"],
    )
    assert wrong_extension.status_code == 400

    garbage = db_client.post(
        ADMIN,
        files={"file": ("bg.mp3", b"definitely not mp3", "audio/mpeg")},
        headers=admin_user["headers"],
    )
    assert garbage.status_code == 400
    assert "MP3" in garbage.json()["detail"]

    empty = db_client.post(
        ADMIN,
        files={"file": ("bg.mp3", b"", "audio/mpeg")},
        headers=admin_user["headers"],
    )
    assert empty.status_code == 400

    monkeypatch.setattr(settings, "max_audio_upload_bytes", 64)
    oversized = db_client.post(
        ADMIN,
        files={"file": ("bg.mp3", helpers.build_mp3_bytes(size=256), "audio/mpeg")},
        headers=admin_user["headers"],
    )
    assert oversized.status_code == 413
    assert "limit" in oversized.json()["detail"].lower()

    # A rejected upload never leaves a row or a public URL behind.
    assert db_client.get(PUBLIC).json() == EMPTY
