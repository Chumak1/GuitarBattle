"""Contacts block on the public pages.

The author's contact details are a static block (``section.contacts``) shipped
with the landing page and the three public web-app pages. It must appear
exactly once per page, never in the admin panel, and its labels must be
translated by both i18n dictionaries. The pages are plain static files served
by FastAPI, so the checks read the files directly and also confirm that the
HTML routes return the block.
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest

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

CSS_CLASSES = (
    ".contacts",
    ".contacts__eyebrow",
    ".contacts__title",
    ".contacts__links",
    ".contacts__link",
    ".contacts__label",
    ".contacts__value",
)

CYRILLIC_RE = re.compile(r"[А-Яа-яЁё]")


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
def test_contacts_block_is_bound_to_i18n(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])

    for key in CONTACT_KEYS:
        assert f'data-i18n="{key}"' in html


@pytest.mark.parametrize("name", sorted(I18N_SOURCES))
def test_i18n_dictionaries_have_contacts_keys(name: str) -> None:
    parts = _dict_parts(_html(I18N_SOURCES[name]))

    for lang in ("ru", "en"):
        for key in CONTACT_KEYS:
            assert _entry(parts[lang], key) is not None, f"{key} missing in {name}/{lang}"


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


def test_admin_page_has_no_contacts_block() -> None:
    html = _html(ADMIN_PAGE)

    assert 'class="contacts' not in html
    assert MAILTO not in html
    assert TELEGRAM_URL not in html


@pytest.mark.parametrize("name", sorted(PUBLIC_ROUTES))
def test_route_serves_contacts_block(client, name: str) -> None:
    response = client.get(PUBLIC_ROUTES[name])

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("text/html")
    assert response.text.count('class="contacts"') == 1
    assert MAILTO in response.text
    assert TELEGRAM_URL in response.text
