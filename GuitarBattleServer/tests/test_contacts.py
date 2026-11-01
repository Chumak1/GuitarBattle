"""Contacts block on the public pages.

The author's contact details are a static block (``section.contacts``) shipped
with the landing page and the three public web-app pages. It must appear
exactly once per page, never in the admin panel, and its labels must be
translated by both i18n dictionaries. The pages are plain static files served
by FastAPI, so the checks read the files directly and also confirm that the
HTML routes return the block.

The values themselves are managed in the admin panel and served by
``GET /api/v1/site/contacts``; the static markup is the no-JavaScript
fallback and contains four disabled social placeholders.
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest

from tests import helpers

STATIC_DIR = Path(__file__).resolve().parents[1] / "app" / "static"

PUBLIC_PAGES = {
    "landing": STATIC_DIR / "landing" / "index.html",
    "auth": STATIC_DIR / "app" / "auth.html",
    "account": STATIC_DIR / "app" / "account.html",
    "policy": STATIC_DIR / "app" / "policy.html",
}

PUBLIC_ROUTES = {
    "landing": "/",
    "auth": "/login",
    "account": "/account",
    "policy": "/policy",
}

ADMIN_PAGE = STATIC_DIR / "admin" / "index.html"
ADMIN_SCRIPT = STATIC_DIR / "admin" / "admin-site.js"

I18N_SOURCES = {
    "app": STATIC_DIR / "app" / "i18n.js",
    "landing": STATIC_DIR / "app" / "landing-i18n.js",
}

STYLES = {
    "app": STATIC_DIR / "app" / "styles.css",
    "landing": STATIC_DIR / "landing" / "index.html",
}

EMAIL = "chumakevgeny@gmail.com"
MAILTO = f"mailto:{EMAIL}"
TELEGRAM_URL = "https://t.me/chumakevgene"
TELEGRAM_HANDLE = "@chumakevgene"

CONTACT_KEYS = (
    "contacts.title",
    "contacts.name",
    "contacts.emailLabel",
    "contacts.telegramLabel",
)

MARKUP_KEYS = CONTACT_KEYS + ("contacts.socialsLabel",)

CSS_CLASSES = (
    ".contacts",
    ".contacts__eyebrow",
    ".contacts__title",
    ".contacts__links",
    ".contacts__link",
    ".contacts__label",
    ".contacts__value",
    ".contacts__socials",
    ".contacts__social",
)

SOCIAL_NETWORKS = ("youtube", "instagram", "twitter", "vk")

DATA_HOOKS = (
    "data-contacts-name",
    "data-contacts-email",
    "data-contacts-telegram",
    "data-contacts-socials",
)

CYRILLIC_RE = re.compile(r"[А-Яа-яЁё]")

DEFAULT_CONTACTS = {
    "name": {
        "ru": "Чумак Евгений Александрович",
        "en": "Evgeniy Aleksandrovich Chumak",
    },
    "email": EMAIL,
    "telegram": TELEGRAM_HANDLE,
    "telegram_url": TELEGRAM_URL,
}


def _html(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _telegram_anchor(html: str) -> str:
    match = re.search(r"<a\b[^>]*href=\"" + re.escape(TELEGRAM_URL) + r"\"[^>]*>", html)
    assert match is not None, "Telegram link is missing"
    return match.group(0)


def _dict_parts(source: str) -> dict[str, str]:
    """Split a dictionary module into its ``ru`` and ``en`` halves."""
    matches = [
        match
        for match in re.finditer(r"\n[ \t]+en:[ \t]*\{[ \t]*\n", source)
    ]
    assert matches, "en dictionary not found"
    start = matches[-1].start()
    return {"ru": source[:start], "en": source[start:]}


def _entry(part: str, key: str) -> str | None:
    match = re.search(r"'" + re.escape(key) + r"':\s*'([^']*)'", part)
    return match.group(1) if match is not None else None


# ---------------------------------------------------------------------------
# Static markup
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("name", sorted(PUBLIC_PAGES))
def test_page_has_one_contacts_block(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])

    assert html.count('class="contacts"') == 1
    assert MAILTO in html
    assert EMAIL in html
    assert TELEGRAM_URL in html
    assert TELEGRAM_HANDLE in html

    anchor = _telegram_anchor(html)
    assert 'target="_blank"' in anchor
    assert 'rel="noopener noreferrer"' in anchor


@pytest.mark.parametrize("name", sorted(PUBLIC_PAGES))
def test_contacts_block_has_js_hooks(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])

    for hook in DATA_HOOKS:
        assert hook in html, f"{hook} missing in {name}"
    assert "/app/contacts.js" in html, f"contacts.js not loaded in {name}"


@pytest.mark.parametrize("name", sorted(PUBLIC_PAGES))
def test_page_ships_four_social_placeholders(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])

    assert html.count('class="contacts__social is-placeholder"') == 4
    positions = []
    for network in SOCIAL_NETWORKS:
        marker = f'data-social="{network}"'
        assert marker in html, f"{network} placeholder missing in {name}"
        positions.append(html.index(marker))
    # Icons keep a fixed order: YouTube, Instagram, Twitter/X, VK.
    assert positions == sorted(positions)

    assert 'title="Скоро"' in html or 'data-i18n-title="contacts.soon"' in html
    assert 'aria-disabled="true"' in html
    assert 'tabindex="-1"' in html


@pytest.mark.parametrize("name", sorted(PUBLIC_PAGES))
def test_contacts_block_is_bound_to_i18n(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])

    for key in MARKUP_KEYS:
        assert f'data-i18n="{key}"' in html or f'data-i18n-aria-label="{key}"' in html


@pytest.mark.parametrize("name", sorted(I18N_SOURCES))
def test_i18n_dictionaries_have_contacts_keys(name: str) -> None:
    parts = _dict_parts(_html(I18N_SOURCES[name]))

    for lang in ("ru", "en"):
        for key in CONTACT_KEYS:
            assert _entry(parts[lang], key) is not None, f"{key} missing in {name}/{lang}"


@pytest.mark.parametrize("name", sorted(I18N_SOURCES))
def test_social_translations(name: str) -> None:
    parts = _dict_parts(_html(I18N_SOURCES[name]))

    assert _entry(parts["ru"], "contacts.soon") == "Скоро"
    assert _entry(parts["en"], "contacts.soon") == "Coming soon"
    assert _entry(parts["ru"], "contacts.socialsLabel") == "Мы в соцсетях"
    assert _entry(parts["en"], "contacts.socialsLabel") == "Follow us"


@pytest.mark.parametrize("name", sorted(I18N_SOURCES))
def test_english_contacts_name_has_no_cyrillic(name: str) -> None:
    parts = _dict_parts(_html(I18N_SOURCES[name]))

    english_name = _entry(parts["en"], "contacts.name")
    assert english_name == "Evgeniy Aleksandrovich Chumak"
    assert CYRILLIC_RE.search(english_name) is None

    # The untranslated values stay identical in both languages.
    assert _entry(parts["ru"], "contacts.emailLabel") == "Email"
    assert _entry(parts["en"], "contacts.emailLabel") == "Email"
    assert _entry(parts["ru"], "contacts.telegramLabel") == "Telegram"
    assert _entry(parts["en"], "contacts.telegramLabel") == "Telegram"


@pytest.mark.parametrize("name", sorted(STYLES))
def test_styles_declare_contacts_classes(name: str) -> None:
    css = _html(STYLES[name])

    for selector in CSS_CLASSES:
        assert selector + " {" in css, f"{selector} missing in {name} styles"
    assert ".contacts__social.is-placeholder {" in css


def test_admin_page_has_no_contacts_block() -> None:
    html = _html(ADMIN_PAGE)

    assert 'class="contacts' not in html
    assert MAILTO not in html
    assert TELEGRAM_URL not in html
    assert "data-contacts-socials" not in html
    assert "/app/contacts.js" not in html


def test_admin_has_contacts_section() -> None:
    html = _html(ADMIN_PAGE)
    script = _html(ADMIN_SCRIPT)

    assert 'data-admin-section="contacts"' in html
    assert 'data-admin-panel="contacts"' in html
    assert 'id="sectionContacts"' in html
    assert 'id="contactsNameRu"' in html
    assert 'id="contactsEmail"' in html
    assert 'id="contactsYoutube"' in html
    assert "/admin/site/contacts" in script


@pytest.mark.parametrize("name", sorted(PUBLIC_ROUTES))
def test_route_serves_contacts_block(client, name: str) -> None:
    response = client.get(PUBLIC_ROUTES[name])

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("text/html")
    assert response.text.count('class="contacts"') == 1
    assert MAILTO in response.text
    assert TELEGRAM_URL in response.text
    assert response.text.count('class="contacts__social is-placeholder"') == 4
    assert "/app/contacts.js" in response.text


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------
def test_public_contacts_defaults(db_client) -> None:
    response = db_client.get(f"{helpers.API}/site/contacts")

    assert response.status_code == 200, response.text
    body = response.json()
    assert body["name"] == DEFAULT_CONTACTS["name"]
    assert body["email"] == DEFAULT_CONTACTS["email"]
    assert body["telegram"] == DEFAULT_CONTACTS["telegram"]
    assert body["telegram_url"] == DEFAULT_CONTACTS["telegram_url"]
    assert body["socials"] == {network: None for network in SOCIAL_NETWORKS}

    # Only the documented fields are exposed.
    assert set(body) == {"name", "email", "telegram", "telegram_url", "socials"}
