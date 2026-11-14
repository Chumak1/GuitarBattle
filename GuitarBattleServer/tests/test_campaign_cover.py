"""Campaign covers and descriptions.

The product rules pinned here:

* an uploaded cover is stored under ``campaign_media_dir`` and exposed as an
  origin-relative ``/media/campaigns/<uuid>.<ext>`` URL in both the admin and
  the public campaign payloads;
* the file signature decides the type (PNG/JPEG/WebP), never the client
  ``content_type``; unsupported types are a 415 and oversized files a 413;
* every upload gets a new uuid name, so replacing a cover produces a new URL
  and deletes the previous file;
* deleting the cover removes the file and clears the column;
* ``description`` is trimmed, an empty value clears it and >400 characters is
  rejected with 422.
"""
from __future__ import annotations

import base64
import uuid
from pathlib import Path

import pytest

from app.config import settings
from tests import helpers

ADMIN = f"{helpers.API}/admin/campaigns"
PUBLIC = f"{helpers.API}/campaigns"

# A real 1x1 transparent PNG: the upload route checks the signature, and the
# admin preview / client can decode it.
PNG_BYTES = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAE"
    "hQGAhKmMIQAAAABJRU5ErkJggg=="
)
JPEG_BYTES = b"\xff\xd8\xff\xe0" + b"\x00" * 128
WEBP_BYTES = b"RIFF" + b"\x00\x00\x00\x00" + b"WEBP" + b"\x00" * 128


@pytest.fixture()
def cover_dir(tmp_path, monkeypatch):
    """Private cover storage for one test; no real volume is touched."""
    directory = tmp_path / "campaigns"
    monkeypatch.setattr(settings, "campaign_media_dir", directory)
    return directory


def _create(client, admin_user, *, name: str = "Cover tower", description=None):
    payload = {"name": name, "bot_level": 1}
    if description is not None:
        payload["description"] = description
    return client.post(ADMIN, json=payload, headers=admin_user["headers"])


def _campaign_with_published_song(client, admin_user, *, name="Cover tower"):
    campaign = _create(client, admin_user, name=name).json()
    song = helpers.create_song(client, admin_user["headers"], title=f"{name} song")
    helpers.publish_song(client, admin_user["headers"], song["id"])
    response = client.put(
        f"{ADMIN}/{campaign['id']}/songs",
        json={"song_ids": [song["id"]]},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    return campaign


def _upload(client, admin_user, campaign_id, *, content=PNG_BYTES,
            filename="cover.png", content_type="image/png", headers=None):
    return client.post(
        f"{ADMIN}/{campaign_id}/cover",
        files={"file": (filename, content, content_type)},
        headers=headers if headers is not None else admin_user["headers"],
    )


def _public_item(client, campaign_id):
    items = client.get(PUBLIC).json()["items"]
    return next(item for item in items if item["id"] == campaign_id)


def _stored_files(directory: Path) -> list[Path]:
    if not directory.exists():
        return []
    return [path for path in directory.iterdir() if path.is_file()]


# ---------------------------------------------------------------------------
# Upload
# ---------------------------------------------------------------------------
def test_upload_sets_cover_url_in_admin_and_public_payloads(
    db_client, admin_user, cover_dir
):
    campaign = _campaign_with_published_song(db_client, admin_user)

    response = _upload(db_client, admin_user, campaign["id"])

    assert response.status_code == 201, response.text
    payload = response.json()
    url = payload["cover_image_url"]
    assert url is not None
    assert url.startswith("/media/campaigns/")
    assert payload["description"] is None

    stored = cover_dir / url.rsplit("/", 1)[-1]
    assert stored.is_file()
    assert stored.read_bytes() == PNG_BYTES

    admin = db_client.get(
        f"{ADMIN}/{campaign['id']}", headers=admin_user["headers"]
    ).json()
    assert admin["cover_image_url"] == url

    public = _public_item(db_client, campaign["id"])
    assert public["cover_image_url"] == url
    assert public["description"] is None


def test_upload_accepts_jpeg_and_webp_by_signature(db_client, admin_user, cover_dir):
    campaign = _create(db_client, admin_user, name="Formats tower").json()

    for filename, content in (
        ("cover.jpg", JPEG_BYTES),
        ("cover.jpeg", JPEG_BYTES),
        ("cover.webp", WEBP_BYTES),
    ):
        response = _upload(
            db_client,
            admin_user,
            campaign["id"],
            content=content,
            filename=filename,
            # The client-declared type is irrelevant: bytes decide.
            content_type="application/octet-stream",
        )
        assert response.status_code == 201, (filename, response.text)
        assert response.json()["cover_image_url"].endswith(
            Path(filename).suffix
        )


def test_replacing_the_cover_creates_a_new_url_and_removes_the_old_file(
    db_client, admin_user, cover_dir
):
    campaign = _create(db_client, admin_user, name="Replace tower").json()

    first = _upload(db_client, admin_user, campaign["id"]).json()
    first_url = first["cover_image_url"]
    first_path = cover_dir / first_url.rsplit("/", 1)[-1]
    assert first_path.is_file()

    second = _upload(
        db_client, admin_user, campaign["id"], content=JPEG_BYTES, filename="new.jpg"
    )
    assert second.status_code == 201, second.text
    second_url = second.json()["cover_image_url"]

    assert second_url != first_url
    assert second_url.startswith("/media/campaigns/")
    assert not first_path.exists()
    assert (cover_dir / second_url.rsplit("/", 1)[-1]).is_file()
    assert len(_stored_files(cover_dir)) == 1

    admin = db_client.get(
        f"{ADMIN}/{campaign['id']}", headers=admin_user["headers"]
    ).json()
    assert admin["cover_image_url"] == second_url


def test_delete_clears_the_field_and_removes_the_file(
    db_client, admin_user, cover_dir
):
    campaign = _campaign_with_published_song(db_client, admin_user, name="Delete tower")
    uploaded = _upload(db_client, admin_user, campaign["id"]).json()
    stored = cover_dir / uploaded["cover_image_url"].rsplit("/", 1)[-1]
    assert stored.is_file()

    response = db_client.delete(
        f"{ADMIN}/{campaign['id']}/cover", headers=admin_user["headers"]
    )

    assert response.status_code == 200, response.text
    assert response.json()["cover_image_url"] is None
    assert not stored.exists()
    assert _stored_files(cover_dir) == []
    assert (
        db_client.get(
            f"{ADMIN}/{campaign['id']}", headers=admin_user["headers"]
        ).json()["cover_image_url"]
        is None
    )
    assert _public_item(db_client, campaign["id"])["cover_image_url"] is None

    # Idempotent: deleting again keeps the payload valid.
    again = db_client.delete(
        f"{ADMIN}/{campaign['id']}/cover", headers=admin_user["headers"]
    )
    assert again.status_code == 200
    assert again.json()["cover_image_url"] is None


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------
def test_wrong_signature_or_extension_is_rejected_with_415(
    db_client, admin_user, cover_dir
):
    campaign = _create(db_client, admin_user, name="Bad cover tower").json()

    # Extension is allowed but the bytes are not a PNG.
    wrong_bytes = _upload(
        db_client,
        admin_user,
        campaign["id"],
        content=b"GIF89a" + b"\x00" * 64,
        filename="cover.png",
        content_type="image/png",
    )
    assert wrong_bytes.status_code == 415, wrong_bytes.text

    # Signature is a PNG but the extension is not accepted.
    wrong_extension = _upload(
        db_client, admin_user, campaign["id"], filename="cover.gif"
    )
    assert wrong_extension.status_code == 415, wrong_extension.text

    no_extension = _upload(db_client, admin_user, campaign["id"], filename="cover")
    assert no_extension.status_code == 415, no_extension.text

    assert _stored_files(cover_dir) == []
    assert (
        db_client.get(
            f"{ADMIN}/{campaign['id']}", headers=admin_user["headers"]
        ).json()["cover_image_url"]
        is None
    )


def test_oversized_cover_is_rejected_with_413(
    db_client, admin_user, cover_dir, monkeypatch
):
    campaign = _create(db_client, admin_user, name="Big cover tower").json()
    monkeypatch.setattr(settings, "max_campaign_cover_bytes", 64)

    response = _upload(
        db_client,
        admin_user,
        campaign["id"],
        content=PNG_BYTES + b"\x00" * 1024,
    )

    assert response.status_code == 413, response.text
    assert _stored_files(cover_dir) == []
    assert (
        db_client.get(
            f"{ADMIN}/{campaign['id']}", headers=admin_user["headers"]
        ).json()["cover_image_url"]
        is None
    )


# ---------------------------------------------------------------------------
# Description
# ---------------------------------------------------------------------------
def test_description_patch_sets_trims_and_clears(db_client, admin_user):
    campaign = _create(db_client, admin_user, name="Description tower").json()
    assert campaign["description"] is None
    url = f"{ADMIN}/{campaign['id']}"

    set_response = db_client.patch(
        url,
        json={"description": "  Hall of riffs  "},
        headers=admin_user["headers"],
    )
    assert set_response.status_code == 200, set_response.text
    assert set_response.json()["description"] == "Hall of riffs"

    cleared = db_client.patch(
        url, json={"description": "   "}, headers=admin_user["headers"]
    )
    assert cleared.status_code == 200, cleared.text
    assert cleared.json()["description"] is None

    db_client.patch(
        url,
        json={"description": "Long description"},
        headers=admin_user["headers"],
    )
    explicit_null = db_client.patch(
        url, json={"description": None}, headers=admin_user["headers"]
    )
    assert explicit_null.status_code == 200, explicit_null.text
    assert explicit_null.json()["description"] is None


def test_description_is_trimmed_on_create_and_exposed_publicly(
    db_client, admin_user
):
    campaign = _campaign_with_published_song(
        db_client, admin_user, name="Public description tower"
    )
    patched = db_client.patch(
        f"{ADMIN}/{campaign['id']}",
        json={"description": "Three stages of riffs"},
        headers=admin_user["headers"],
    )
    assert patched.status_code == 200, patched.text

    created = _create(
        db_client, admin_user, name="Created tower", description="  Trim me  "
    )
    assert created.status_code == 201, created.text
    assert created.json()["description"] == "Trim me"

    public = _public_item(db_client, campaign["id"])
    assert public["description"] == "Three stages of riffs"


def test_description_longer_than_400_characters_is_rejected(db_client, admin_user):
    campaign = _create(db_client, admin_user, name="Long text tower").json()

    response = db_client.patch(
        f"{ADMIN}/{campaign['id']}",
        json={"description": "x" * 401},
        headers=admin_user["headers"],
    )
    assert response.status_code == 422, response.text

    created = db_client.post(
        ADMIN,
        json={"name": "Too long", "bot_level": 1, "description": "x" * 401},
        headers=admin_user["headers"],
    )
    assert created.status_code == 422, created.text


# ---------------------------------------------------------------------------
# Access control
# ---------------------------------------------------------------------------
def test_cover_and_description_routes_require_admin(db_client, admin_user, user_account):
    campaign = _create(db_client, admin_user, name="Guarded tower").json()
    cover_url = f"{ADMIN}/{campaign['id']}/cover"

    assert _upload(db_client, admin_user, campaign["id"], headers={}).status_code == 401
    assert db_client.delete(cover_url).status_code == 401
    assert (
        db_client.patch(
            f"{ADMIN}/{campaign['id']}",
            json={"description": "nope"},
        ).status_code
        == 401
    )

    forbidden = _upload(
        db_client,
        admin_user,
        campaign["id"],
        headers=user_account["headers"],
    )
    assert forbidden.status_code == 403, forbidden.text
    assert (
        db_client.delete(cover_url, headers=user_account["headers"]).status_code
        == 403
    )
    # No cover was created by the rejected requests.
    assert (
        db_client.get(
            f"{ADMIN}/{campaign['id']}", headers=admin_user["headers"]
        ).json()["cover_image_url"]
        is None
    )


def test_cover_routes_unknown_campaign_return_404(db_client, admin_user, cover_dir):
    unknown = str(uuid.uuid4())
    assert _upload(db_client, admin_user, unknown).status_code == 404
    assert (
        db_client.delete(
            f"{ADMIN}/{unknown}/cover", headers=admin_user["headers"]
        ).status_code
        == 404
    )
