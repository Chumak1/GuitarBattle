"""Landing gallery, game modes and trailer: the parts of the public site the
administrator edits after the static copy was written.

Pinned here:

* ``POST /admin/site/landing/screenshots`` stores an image under
  ``landing_media_dir`` as ``shot-<uuid>.<ext>`` and returns the whole gallery;
* the gallery is capped at :data:`MAX_SCREENSHOTS`, and only images are
  accepted (the signature decides, never the client ``content_type``);
* ``PATCH`` replaces the list (order + RU/EN captions) and deletes files the
  list no longer references; ``DELETE`` removes one entry and its file;
* the hero background is never deleted by the gallery cleanup;
* ``GET /landing`` serves the gallery to the public page;
* the trailer block accepts YouTube links only and stores the canonical URL;
* the template ships the modes/screens/trailer sections, the gallery stays
  hidden without screenshots, and the RU/EN variants embed their own trailer.
"""
from __future__ import annotations

import base64
from pathlib import Path

import pytest

from app.config import settings
from app.schemas.site import MAX_SCREENSHOTS
from tests import helpers

ADMIN = f"{helpers.API}/admin/site/landing"
SHOTS = f"{ADMIN}/screenshots"

PNG_BYTES = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAE"
    "hQGAhKmMIQAAAABJRU5ErkJggg=="
)
JPEG_BYTES = b"\xff\xd8\xff\xe0" + b"\x00" * 128


@pytest.fixture()
def landing_dir(tmp_path, monkeypatch):
    directory = tmp_path / "landing"
    monkeypatch.setattr(settings, "landing_media_dir", directory)
    return directory


def _upload(
    client,
    admin_user,
    *,
    content: bytes = PNG_BYTES,
    filename: str = "shot.png",
    content_type: str = "image/png",
    headers: dict[str, str] | None = None,
):
    return client.post(
        SHOTS,
        files={"file": (filename, content, content_type)},
        headers=admin_user["headers"] if headers is None else headers,
    )


def _gallery(client, admin_user) -> list[dict]:
    response = client.get(ADMIN, headers=admin_user["headers"])
    assert response.status_code == 200, response.text
    return response.json()["screenshots"]


def _public(client) -> dict:
    response = client.get(f"{helpers.API}/landing")
    assert response.status_code == 200, response.text
    return response.json()


def _stored_name(url: str) -> str:
    return url.rsplit("/", 1)[-1]


# ---------------------------------------------------------------------------
# Upload
# ---------------------------------------------------------------------------
def test_upload_returns_the_gallery_and_writes_the_file(
    db_client, admin_user, landing_dir
):
    response = _upload(db_client, admin_user)

    assert response.status_code == 201, response.text
    screenshots = response.json()["screenshots"]
    assert len(screenshots) == 1
    item = screenshots[0]
    assert item["url"].startswith("/media/landing/shot-")
    assert item["url"].endswith(".png")
    assert item["caption_ru"] == "" and item["caption_en"] == ""

    stored = landing_dir / _stored_name(item["url"])
    assert stored.is_file()
    assert stored.read_bytes() == PNG_BYTES

    admin = _gallery(db_client, admin_user)
    assert admin == screenshots


def test_public_landing_serves_the_gallery(db_client, admin_user, landing_dir):
    url = _upload(db_client, admin_user).json()["screenshots"][0]["url"]

    public = _public(db_client)
    assert public["screenshots"] == [
        {"url": url, "caption_ru": "", "caption_en": ""}
    ]


def test_non_image_upload_is_rejected(db_client, admin_user, landing_dir):
    response = _upload(
        db_client,
        admin_user,
        content=b"not an image at all",
        filename="shot.png",
    )

    assert response.status_code == 415, response.text
    if landing_dir.exists():
        assert list(landing_dir.iterdir()) == []


def test_unsupported_extension_is_rejected(db_client, admin_user, landing_dir):
    response = _upload(
        db_client,
        admin_user,
        content=PNG_BYTES,
        filename="shot.bmp",
    )

    assert response.status_code == 415, response.text


def test_gallery_is_capped(db_client, admin_user, landing_dir):
    for index in range(MAX_SCREENSHOTS):
        response = _upload(db_client, admin_user, filename=f"shot-{index}.png")
        assert response.status_code == 201, response.text

    overflow = _upload(db_client, admin_user, filename="one-too-many.png")
    assert overflow.status_code == 409, overflow.text
    assert len(_gallery(db_client, admin_user)) == MAX_SCREENSHOTS


def test_upload_requires_an_administrator(db_client, user_account, landing_dir):
    anonymous = _upload(
        db_client, user_account, headers={}
    )  # no Authorization header
    assert anonymous.status_code == 401, anonymous.text

    forbidden = _upload(db_client, user_account)
    assert forbidden.status_code == 403, forbidden.text


# ---------------------------------------------------------------------------
# Order, captions and removal
# ---------------------------------------------------------------------------
def test_patch_reorders_and_captions(db_client, admin_user, landing_dir):
    first = _upload(db_client, admin_user, filename="first.png").json()["screenshots"][0]
    second = _upload(db_client, admin_user, filename="second.png").json()["screenshots"][1]

    reordered = [
        {**second, "caption_ru": "  Дуэль  ", "caption_en": " Duel "},
        first,
    ]
    response = db_client.patch(
        SHOTS,
        json={"screenshots": reordered},
        headers=admin_user["headers"],
    )

    assert response.status_code == 200, response.text
    saved = response.json()["screenshots"]
    assert [item["url"] for item in saved] == [second["url"], first["url"]]
    assert saved[0]["caption_ru"] == "Дуэль"
    assert saved[0]["caption_en"] == "Duel"

    assert [item["url"] for item in _public(db_client)["screenshots"]] == [
        second["url"],
        first["url"],
    ]
    # Both files are still referenced, so neither is deleted.
    for item in saved:
        assert (landing_dir / _stored_name(item["url"])).is_file()


def test_patch_deletes_the_files_it_drops(db_client, admin_user, landing_dir):
    kept = _upload(db_client, admin_user, filename="kept.png").json()["screenshots"][0]
    dropped = _upload(
        db_client, admin_user, filename="dropped.png"
    ).json()["screenshots"][1]

    response = db_client.patch(
        SHOTS,
        json={"screenshots": [kept]},
        headers=admin_user["headers"],
    )

    assert response.status_code == 200, response.text
    assert [item["url"] for item in response.json()["screenshots"]] == [kept["url"]]
    assert (landing_dir / _stored_name(kept["url"])).is_file()
    assert not (landing_dir / _stored_name(dropped["url"])).exists()


def test_patch_never_deletes_the_hero_background(
    db_client, admin_user, landing_dir
):
    landing_dir.mkdir(parents=True, exist_ok=True)
    hero = landing_dir / "hero-test.png"
    hero.write_bytes(PNG_BYTES)

    response = db_client.put(
        ADMIN,
        json={
            "blocks": {},
            "hero_background": {
                "type": "image",
                "url": "/media/landing/hero-test.png",
                "poster_url": None,
            },
        },
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text

    _upload(db_client, admin_user)
    cleared = db_client.patch(
        SHOTS, json={"screenshots": []}, headers=admin_user["headers"]
    )
    assert cleared.status_code == 200, cleared.text
    assert cleared.json()["screenshots"] == []
    assert hero.is_file()


def test_delete_removes_one_screenshot_and_its_file(
    db_client, admin_user, landing_dir
):
    first = _upload(db_client, admin_user, filename="first.png").json()["screenshots"][0]
    second = _upload(db_client, admin_user, filename="second.png").json()["screenshots"][1]

    response = db_client.delete(
        SHOTS,
        params={"url": first["url"]},
        headers=admin_user["headers"],
    )

    assert response.status_code == 200, response.text
    assert [item["url"] for item in response.json()["screenshots"]] == [second["url"]]
    assert not (landing_dir / _stored_name(first["url"])).exists()
    assert (landing_dir / _stored_name(second["url"])).is_file()


def test_delete_of_an_unknown_screenshot_is_404(db_client, admin_user, landing_dir):
    response = db_client.delete(
        SHOTS,
        params={"url": "/media/landing/shot-missing.png"},
        headers=admin_user["headers"],
    )

    assert response.status_code == 404, response.text


def test_gallery_schema_rejects_paths_outside_the_upload_dir(
    db_client, admin_user, landing_dir
):
    for url in (
        "/media/artwork/hero.png",
        "/media/landing/../secret.png",
        "/media/landing/sub/shot.png",
        "https://example.com/shot.png",
    ):
        response = db_client.patch(
            SHOTS,
            json={"screenshots": [{"url": url, "caption_ru": "", "caption_en": ""}]},
            headers=admin_user["headers"],
        )
        assert response.status_code == 422, f"{url}: {response.text}"


def test_gallery_schema_rejects_more_than_the_cap(db_client, admin_user):
    payload = [
        {"url": f"/media/landing/shot-{index}.png", "caption_ru": "", "caption_en": ""}
        for index in range(MAX_SCREENSHOTS + 1)
    ]
    response = db_client.patch(
        SHOTS, json={"screenshots": payload}, headers=admin_user["headers"]
    )

    assert response.status_code == 422, response.text


def test_put_keeps_the_gallery_when_screenshots_are_omitted(
    db_client, admin_user, landing_dir
):
    url = _upload(db_client, admin_user).json()["screenshots"][0]["url"]

    response = db_client.put(
        ADMIN,
        json={"blocks": {"cta": {"visible": False}}},
        headers=admin_user["headers"],
    )

    assert response.status_code == 200, response.text
    assert [item["url"] for item in response.json()["screenshots"]] == [url]


def test_put_can_replace_the_gallery(db_client, admin_user, landing_dir):
    replaced = _upload(
        db_client, admin_user, filename="replaced.png"
    ).json()["screenshots"][0]

    response = db_client.put(
        ADMIN,
        json={"blocks": {}, "screenshots": []},
        headers=admin_user["headers"],
    )

    assert response.status_code == 200, response.text
    assert response.json()["screenshots"] == []
    assert not (landing_dir / _stored_name(replaced["url"])).exists()


# ---------------------------------------------------------------------------
# Trailer links
# ---------------------------------------------------------------------------
@pytest.mark.parametrize(
    ("url", "video_id"),
    [
        ("https://www.youtube.com/watch?v=FhE07g-8Jm0", "FhE07g-8Jm0"),
        ("https://youtu.be/HJaP-HcZO5o", "HJaP-HcZO5o"),
        ("https://youtube.com/shorts/HJaP-HcZO5o?si=abc", "HJaP-HcZO5o"),
        ("https://www.youtube.com/embed/FhE07g-8Jm0", "FhE07g-8Jm0"),
        ("FhE07g-8Jm0", "FhE07g-8Jm0"),
    ],
)
def test_trailer_links_are_normalized(db_client, admin_user, url, video_id):
    response = db_client.put(
        ADMIN,
        json={"blocks": {"trailer": {"visible": True, "ru": {"url": url}, "en": {"url": ""}}}},
        headers=admin_user["headers"],
    )

    assert response.status_code == 200, response.text
    trailer = response.json()["blocks"]["trailer"]
    assert trailer["ru"]["url"] == f"https://www.youtube.com/watch?v={video_id}"
    assert trailer["en"]["url"] == ""


@pytest.mark.parametrize(
    "url",
    [
        "https://vimeo.com/123456",
        "javascript:alert(1)",
        "https://www.youtube.com/watch?v=short",
        "https://youtube.com.evil.example/watch?v=FhE07g-8Jm0",
    ],
)
def test_non_youtube_trailer_links_are_rejected(db_client, admin_user, url):
    response = db_client.put(
        ADMIN,
        json={"blocks": {"trailer": {"visible": True, "ru": {"url": url}}}},
        headers=admin_user["headers"],
    )

    assert response.status_code == 422, response.text
    assert "trailer" in response.text.lower()


# ---------------------------------------------------------------------------
# Template and browser dictionary
# ---------------------------------------------------------------------------
STATIC_DIR = Path(__file__).resolve().parents[1] / "app" / "static"
TEMPLATE = STATIC_DIR / "landing" / "index.html"
LANDING_JS = STATIC_DIR / "app" / "landing.js"
ADMIN_SITE = STATIC_DIR / "admin" / "admin-site.js"


def test_template_has_the_new_sections_in_order():
    template = TEMPLATE.read_text(encoding="utf-8")

    positions = [
        template.index('class="hero"'),
        template.index('class="facts"'),
        template.index('id="modes"'),
        template.index('id="screens"'),
        template.index('id="about"'),
        template.index('id="how"'),
        template.index('id="features"'),
        template.index('id="faq"'),
        template.index('id="trailer"'),
        template.index('id="requirements"'),
        template.index('id="support-project"'),
        template.index('class="cta"'),
    ]
    assert positions == sorted(positions)

    # The gallery is hidden until the API reports screenshots: no JavaScript
    # means no broken empty section.
    screens = template[template.index('id="screens"') :]
    assert "hidden" in screens[: screens.index(">")]
    assert 'id="screensGallery"' in template

    # Facade + strict embed host in the controller.
    assert "data-trailer" in template
    assert "youtube-nocookie.com/embed/" in LANDING_JS.read_text(encoding="utf-8")
    assert "i.ytimg.com/vi/" in template


def test_language_variants_embed_their_own_trailer(client):
    ru = client.get("/").text
    en = client.get("/en").text

    assert 'data-video-id="FhE07g-8Jm0"' in ru
    assert 'data-video-id="HJaP-HcZO5o"' in en
    assert 'href="#modes"' in ru and 'href="#screens"' in ru
    assert 'href="#modes"' in en and 'href="#screens"' in en
    assert "Как это выглядит в игре" in ru
    assert "What it looks like in the game" in en


def test_trailer_degrades_gracefully_without_youtube(client):
    """Blocked YouTube assets must not leave a black rectangle."""
    template = TEMPLATE.read_text(encoding="utf-8")
    script = LANDING_JS.read_text(encoding="utf-8")

    # Poster quality chain, then the image is hidden and the gradient shows.
    assert "TRAILER_POSTERS" in script
    for quality in ("maxresdefault", "sddefault", "hqdefault"):
        assert quality in script, quality
    assert "poster.hidden = true" in script
    assert "linear-gradient(160deg, var(--surface), var(--surface-2))" in template

    # A plain link under the player works without the facade click.
    assert "data-trailer-open" in template
    assert 'data-i18n="landing.trailer.open"' in template
    assert "{{ landing.trailer.open }}" in template
    assert "Открыть на YouTube" in client.get("/").text
    assert "Watch on YouTube" in client.get("/en").text


def test_admin_editor_exposes_the_new_blocks_and_gallery():
    script = ADMIN_SITE.read_text(encoding="utf-8")

    for key, title in (
        ("modes", "Режимы игры"),
        ("screens", "Скриншоты"),
        ("trailer", "Трейлер"),
    ):
        assert f'key: "{key}"' in script, key
        assert title in script, key

    for key in ("landing.modes.title", "landing.screens.title", "landing.trailer.title"):
        assert f'r("{key}")' in script, key
    assert "TRAILER_URLS" in script
    assert "/admin/site/landing/screenshots" in script
