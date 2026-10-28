"""Google tag (gtag.js) next to the Yandex.Metrika counter on public pages.

The Google tag (``G-8ZJLDXK867``) ships on the same four public pages as the
Metrika counter: the marketing landing page, the auth screen and the
player-facing account/policy pages. It must live in ``<head>`` and be emitted
*before* the Metrika snippet, because Metrika is initialised with
``ecommerce:"dataLayer"`` and therefore expects ``window.dataLayer`` to exist.

The internal admin panel must stay free of both trackers.
"""
from __future__ import annotations

from pathlib import Path

import pytest

STATIC_DIR = Path(__file__).resolve().parents[1] / "app" / "static"

GTAG_ID = "G-8ZJLDXK867"
GTAG_COMMENT = "<!-- Google tag (gtag.js) -->"
GTAG_ORIGIN = "https://www.googletagmanager.com"
GTAG_LIBRARY = f"{GTAG_ORIGIN}/gtag/js"
GTAG_SCRIPT = f"{GTAG_LIBRARY}?id={GTAG_ID}"
DATA_LAYER_INIT = "window.dataLayer = window.dataLayer || [];"
GTAG_SHIM = "function gtag(){dataLayer.push(arguments);}"
GTAG_JS_CALL = "gtag('js', new Date());"
GTAG_CONFIG_CALL = f"gtag('config', '{GTAG_ID}');"

METRIKA_COMMENT = "<!-- Yandex.Metrika counter -->"
METRIKA_COUNTER_ID = "113842975"
METRIKA_HOST = "mc.yandex.ru"

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


def _html(path: Path) -> str:
    return path.read_text(encoding="utf-8")


@pytest.mark.parametrize("name", sorted(PUBLIC_PAGES))
def test_public_page_contains_google_tag_once(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])

    # One snippet, one loader URL, one config call: never pasted twice.
    assert html.count(GTAG_COMMENT) == 1
    assert html.count(f"{GTAG_LIBRARY}?id={GTAG_ID}") == 1
    assert html.count(GTAG_SCRIPT) == 1
    assert html.count(GTAG_CONFIG_CALL) == 1
    # The measurement id appears exactly in the loader URL and the config call.
    assert html.count(GTAG_ID) == 2
    assert f"gtag/js?id={GTAG_ID}" in html


@pytest.mark.parametrize("name", sorted(PUBLIC_PAGES))
def test_google_tag_initialises_data_layer(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])

    assert DATA_LAYER_INIT in html
    assert GTAG_SHIM in html
    assert GTAG_JS_CALL in html
    assert GTAG_CONFIG_CALL in html


@pytest.mark.parametrize("name", sorted(PUBLIC_PAGES))
def test_google_tag_precedes_metrika_in_head(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])

    gtag = html.index(GTAG_COMMENT)
    metrika = html.index(METRIKA_COMMENT)
    head_end = html.index("</head>")

    # gtag first, then Metrika, and both still inside <head>.
    assert gtag < metrika < head_end
    # The dataLayer is created before Metrika reads ecommerce:"dataLayer".
    assert html.index(DATA_LAYER_INIT) < metrika
    assert html.index(GTAG_CONFIG_CALL) < metrika
    # Nothing was duplicated further down the document.
    assert html.count(GTAG_COMMENT) == 1
    assert html.rindex(GTAG_COMMENT) == gtag


@pytest.mark.parametrize("name", sorted(PUBLIC_PAGES))
def test_public_page_keeps_utf8_declaration(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])
    assert '<meta charset="utf-8">' in html
    assert html.rstrip().endswith("</html>")


def test_admin_page_has_no_analytics() -> None:
    html = _html(ADMIN_PAGE)

    assert GTAG_ID not in html
    assert GTAG_COMMENT not in html
    assert "googletagmanager" not in html.lower()
    assert "google-analytics" not in html.lower()
    assert "dataLayer" not in html
    # And still no Metrika counter either.
    assert METRIKA_COUNTER_ID not in html
    assert METRIKA_HOST not in html
    assert "Yandex.Metrika" not in html


@pytest.mark.parametrize("name", sorted(PUBLIC_ROUTES))
def test_route_serves_google_tag_as_html(client, name: str) -> None:
    response = client.get(PUBLIC_ROUTES[name])

    assert response.status_code == 200
    content_type = response.headers["content-type"]
    assert content_type.startswith("text/html")
    assert "charset=utf-8" in content_type.lower()
    assert response.text.count(GTAG_ID) == 2
    assert GTAG_SCRIPT in response.text
    assert GTAG_CONFIG_CALL in response.text
    assert response.text.index(GTAG_COMMENT) < response.text.index(METRIKA_COMMENT)


def test_admin_route_has_no_google_tag(client) -> None:
    response = client.get("/admin/")

    assert response.status_code == 200
    assert GTAG_ID not in response.text
    assert "googletagmanager" not in response.text.lower()


@pytest.mark.parametrize("name", sorted(PUBLIC_ROUTES))
def test_public_route_csp_allows_google_tag(client, name: str) -> None:
    """No CSP, or a CSP that explicitly permits Google Tag Manager."""
    response = client.get(PUBLIC_ROUTES[name])
    csp = response.headers.get("content-security-policy")
    if csp is None:
        return  # No CSP at all: the tag cannot be blocked.

    lowered = csp.lower()
    assert "script-src" in lowered and "img-src" in lowered and "connect-src" in lowered
    assert GTAG_ORIGIN in lowered
    assert "https://www.google-analytics.com" in lowered
    assert "https://region1.google-analytics.com" in lowered
