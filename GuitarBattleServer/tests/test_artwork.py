"""Administrator artwork slots and player avatars.

Product rules pinned here:

* the three slots (``hero-battle``, ``hero-duel``, ``default-avatar``) store one
  image each under ``artwork_media_dir`` and expose it as an origin-relative
  ``/media/artwork/<slot>-<uuid>.<ext>`` URL in ``GET /site/config``;
* the file signature decides the type (PNG/JPEG/WebP), never the client
  ``content_type``: unsupported types are a 415, oversized files a 413;
* replacing an image produces a new URL and deletes the previous file;
* deleting an image clears the slot and leaves the public field ``null``;
* a player avatar is stored under ``avatar_media_dir`` and exposed as
  ``/media/avatars/avatar-<uuid>.<ext>`` in ``/auth/me`` and the public card;
* every route is administrator-only (artwork) or Bearer-only (avatar).
"""
from __future__ import annotations

import base64
from pathlib import Path

import pytest

from app.config import settings
from tests import helpers

ADMIN = f"{helpers.API}/admin/artwork"
CONFIG = f"{helpers.API}/site/config"
AVATAR = f"{helpers.API}/profile/avatar"

PNG_BYTES = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAE"
    "hQGAhKmMIQAAAABJRU5ErkJggg=="
)
JPEG_BYTES = b"\xff\xd8\xff\xe0" + b"\x00" * 128
WEBP_BYTES = b"RIFF" + b"\x00\x00\x00\x00" + b"WEBP" + b"\x00" * 128


@pytest.fixture()
def artwork_dir(tmp_path, monkeypatch):
    directory = tmp_path / "artwork"
    monkeypatch.setattr(settings, "artwork_media_dir", directory)
    return directory


@pytest.fixture()
def avatar_dir(tmp_path, monkeypatch):
    directory = tmp_path / "avatars"
    monkeypatch.setattr(settings, "avatar_media_dir", directory)
    return directory


def _stored_files(directory: Path) -> list[Path]:
    if not directory.exists():
        return []
    return [path for path in directory.iterdir() if path.is_file()]


def _upload(client, admin_user, slot, *, content=PNG_BYTES, filename="hero.png",
            content_type="image/png", headers=None):
    return client.post(
        f"{ADMIN}/{slot}/image",
        files={"file": (filename, content, content_type)},
        headers=headers if headers is not None else admin_user["headers"],
    )


def _config(client):
    response = client.get(CONFIG)
    assert response.status_code == 200, response.text
    return response.json()


def _slots(client, admin_user):
    response = client.get(ADMIN, headers=admin_user["headers"])
    assert response.status_code == 200, response.text
    return {item["slot"]: item for item in response.json()["items"]}


# ---------------------------------------------------------------------------
# Administrator artwork
# ---------------------------------------------------------------------------
def test_empty_config_has_null_urls(db_client):
    assert _config(db_client) == {
        "hero_battle_url": None,
        "hero_duel_url": None,
        "default_avatar_url": None,
    }


def test_upload_sets_url_in_admin_list_and_public_config(
    db_client, admin_user, artwork_dir
):
    response = _upload(db_client, admin_user, "hero-battle")

    assert response.status_code == 201, response.text
    payload = response.json()
    assert payload["slot"] == "hero-battle"
    url = payload["url"]
    assert url is not None
    assert url.startswith("/media/artwork/hero-battle-")
    assert url.endswith(".png")

    stored = artwork_dir / url.rsplit("/", 1)[-1]
    assert stored.is_file()
    assert stored.read_bytes() == PNG_BYTES

    slots = _slots(db_client, admin_user)
    assert set(slots) == {"hero-battle", "hero-duel", "default-avatar"}
    assert slots["hero-battle"]["url"] == url
    assert slots["hero-duel"]["url"] is None

    config = _config(db_client)
    assert config["hero_battle_url"] == url
    assert config["hero_duel_url"] is None
    assert config["default_avatar_url"] is None


def test_landing_response_carries_the_same_artwork_fields(
    db_client, admin_user, artwork_dir
):
    url = _upload(db_client, admin_user, "hero-duel").json()["url"]

    landing = db_client.get(f"{helpers.API}/landing").json()
    # Existing fields stay untouched and the artwork keys are additive.
    assert landing["hero_duel_url"] == url
    assert landing["hero_battle_url"] is None
    assert "downloads" in landing and "updates" in landing


def test_upload_accepts_jpeg_and_webp_by_signature(
    db_client, admin_user, artwork_dir
):
    for slot, filename, content in (
        ("hero-battle", "hero.jpg", JPEG_BYTES),
        ("hero-duel", "hero.webp", WEBP_BYTES),
        ("default-avatar", "face.jpeg", JPEG_BYTES),
    ):
        response = _upload(
            db_client,
            admin_user,
            slot,
            content=content,
            filename=filename,
            # The client-declared type is irrelevant: the bytes decide.
            content_type="application/octet-stream",
        )
        assert response.status_code == 201, (slot, response.text)
        assert response.json()["url"].endswith(Path(filename).suffix)


def test_replacing_an_image_creates_a_new_url_and_removes_the_old_file(
    db_client, admin_user, artwork_dir
):
    first_url = _upload(db_client, admin_user, "hero-battle").json()["url"]
    first_path = artwork_dir / first_url.rsplit("/", 1)[-1]
    assert first_path.is_file()

    second = _upload(
        db_client, admin_user, "hero-battle", content=JPEG_BYTES, filename="new.jpg"
    )
    assert second.status_code == 201, second.text
    second_url = second.json()["url"]

    assert second_url != first_url
    assert not first_path.exists()
    assert (artwork_dir / second_url.rsplit("/", 1)[-1]).is_file()
    assert len(_stored_files(artwork_dir)) == 1
    assert _config(db_client)["hero_battle_url"] == second_url


def test_delete_clears_the_slot_and_removes_the_file(
    db_client, admin_user, artwork_dir
):
    url = _upload(db_client, admin_user, "default-avatar").json()["url"]
    stored = artwork_dir / url.rsplit("/", 1)[-1]
    assert stored.is_file()

    response = db_client.delete(
        f"{ADMIN}/default-avatar/image", headers=admin_user["headers"]
    )

    assert response.status_code == 200, response.text
    assert response.json()["url"] is None
    assert not stored.exists()
    assert _stored_files(artwork_dir) == []
    assert _config(db_client)["default_avatar_url"] is None
    assert _slots(db_client, admin_user)["default-avatar"]["url"] is None

    # Idempotent: deleting again keeps the payload valid.
    again = db_client.delete(
        f"{ADMIN}/default-avatar/image", headers=admin_user["headers"]
    )
    assert again.status_code == 200
    assert again.json()["url"] is None


def test_wrong_signature_or_extension_is_rejected_with_415(
    db_client, admin_user, artwork_dir
):
    wrong_bytes = _upload(
        db_client,
        admin_user,
        "hero-battle",
        content=b"GIF89a" + b"\x00" * 64,
        filename="hero.png",
    )
    assert wrong_bytes.status_code == 415, wrong_bytes.text

    wrong_extension = _upload(
        db_client, admin_user, "hero-battle", filename="hero.gif"
    )
    assert wrong_extension.status_code == 415, wrong_extension.text

    no_extension = _upload(db_client, admin_user, "hero-battle", filename="hero")
    assert no_extension.status_code == 415, no_extension.text

    assert _stored_files(artwork_dir) == []
    assert _config(db_client)["hero_battle_url"] is None


def test_oversized_artwork_is_rejected_with_413(
    db_client, admin_user, artwork_dir, monkeypatch
):
    monkeypatch.setattr(settings, "max_artwork_bytes", 64)

    response = _upload(
        db_client, admin_user, "hero-battle", content=PNG_BYTES + b"\x00" * 1024
    )

    assert response.status_code == 413, response.text
    assert _stored_files(artwork_dir) == []
    assert _config(db_client)["hero_battle_url"] is None


def test_unknown_slot_is_rejected(db_client, admin_user):
    assert _upload(db_client, admin_user, "hero-unknown").status_code == 422
    assert (
        db_client.delete(
            f"{ADMIN}/hero-unknown/image", headers=admin_user["headers"]
        ).status_code
        == 422
    )


def test_artwork_routes_require_admin(db_client, admin_user, user_account):
    assert _upload(db_client, admin_user, "hero-battle", headers={}).status_code == 401
    assert (
        _upload(
            db_client, admin_user, "hero-battle", headers=user_account["headers"]
        ).status_code
        == 403
    )
    assert db_client.delete(f"{ADMIN}/hero-battle/image").status_code == 401
    assert (
        db_client.get(ADMIN, headers=user_account["headers"]).status_code == 403
    )


# ---------------------------------------------------------------------------
# Player avatar
# ---------------------------------------------------------------------------
def _upload_avatar(client, account, *, content=PNG_BYTES, filename="me.png",
                   content_type="image/png", headers=None):
    return client.post(
        AVATAR,
        files={"file": (filename, content, content_type)},
        headers=headers if headers is not None else account["headers"],
    )


def test_avatar_upload_is_exposed_in_me_and_public_profile(
    db_client, user_account, avatar_dir
):
    response = _upload_avatar(db_client, user_account)

    assert response.status_code == 201, response.text
    url = response.json()["avatar_image_url"]
    assert url is not None
    assert url.startswith("/media/avatars/avatar-")
    stored = avatar_dir / url.rsplit("/", 1)[-1]
    assert stored.is_file()
    assert stored.read_bytes() == PNG_BYTES

    me = db_client.get(
        f"{helpers.API}/auth/me", headers=user_account["headers"]
    ).json()
    assert me["avatar_image_url"] == url

    card = db_client.get(
        f"{helpers.API}/profile/users/{user_account['id']}",
        headers=user_account["headers"],
    ).json()
    assert card["avatar_image_url"] == url

    # Friends/requests/conversations all use the compact player card.
    friends = db_client.get(
        f"{helpers.API}/social/friends", headers=user_account["headers"]
    )
    assert friends.status_code == 200, friends.text


def test_avatar_replace_and_delete(db_client, user_account, avatar_dir):
    first_url = _upload_avatar(db_client, user_account).json()["avatar_image_url"]
    first_path = avatar_dir / first_url.rsplit("/", 1)[-1]

    second = _upload_avatar(
        db_client, user_account, content=WEBP_BYTES, filename="me.webp"
    )
    assert second.status_code == 201, second.text
    second_url = second.json()["avatar_image_url"]
    assert second_url != first_url
    assert not first_path.exists()
    assert len(_stored_files(avatar_dir)) == 1

    removed = db_client.delete(AVATAR, headers=user_account["headers"])
    assert removed.status_code == 200, removed.text
    assert removed.json()["avatar_image_url"] is None
    assert _stored_files(avatar_dir) == []
    me = db_client.get(
        f"{helpers.API}/auth/me", headers=user_account["headers"]
    ).json()
    assert me["avatar_image_url"] is None

    # Idempotent.
    again = db_client.delete(AVATAR, headers=user_account["headers"])
    assert again.status_code == 200
    assert again.json()["avatar_image_url"] is None


def test_avatar_validation(db_client, user_account, avatar_dir, monkeypatch):
    wrong_type = _upload_avatar(
        db_client,
        user_account,
        content=b"GIF89a" + b"\x00" * 64,
        filename="me.png",
    )
    assert wrong_type.status_code == 415, wrong_type.text
    assert _stored_files(avatar_dir) == []

    monkeypatch.setattr(settings, "max_avatar_bytes", 64)
    too_big = _upload_avatar(
        db_client, user_account, content=PNG_BYTES + b"\x00" * 1024
    )
    assert too_big.status_code == 413, too_big.text
    assert _stored_files(avatar_dir) == []


def test_avatar_requires_a_bearer_token(db_client, user_account, avatar_dir):
    assert _upload_avatar(db_client, user_account, headers={}).status_code == 401
    assert db_client.delete(AVATAR).status_code == 401
