"""Yandex.Metrika counter on the public pages.

The counter (id ``113842975``) is part of the marketing site and the player-facing
web apps, so it must be shipped exactly once per public page and never to the
internal admin panel. The pages are plain static files served by FastAPI, so the
checks read the files directly and also confirm that the routes return them as
UTF-8 HTML with the counter inside.
"""
from __future__ import annotations

from pathlib import Path

import pytest

STATIC_DIR = Path(__file__).resolve().parents[1] / "app" / "static"

COUNTER_ID = "113842975"
TAG_SCRIPT = "https://mc.yandex.ru/metrika/tag.js?id=113842975"
WATCH_PIXEL = "https://mc.yandex.ru/watch/113842975"
INIT_PARAMS = (
    "ssr:true, webvisor:true, clickmap:true, ecommerce:\"dataLayer\", "
    "referrer: document.referrer, url: location.href, "
    "accurateTrackBounce:true, trackLinks:true"
)

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
def test_public_page_contains_counter_once(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])

    # One marker pair and one tracker URL: the snippet was not pasted twice.
    assert html.count("<!-- Yandex.Metrika counter -->") == 1
    assert html.count("<!-- /Yandex.Metrika counter -->") == 1
    assert html.count(f"id={COUNTER_ID}") == 1
    # id in the tag URL, in the init call and in the noscript pixel.
    assert html.count(COUNTER_ID) == 3
    assert TAG_SCRIPT in html
    assert WATCH_PIXEL in html
    assert f"ym({COUNTER_ID}, 'init', {{{INIT_PARAMS}}})" in html


@pytest.mark.parametrize("name", sorted(PUBLIC_PAGES))
def test_counter_placement_keeps_markup_valid(name: str) -> None:
    html = _html(PUBLIC_PAGES[name])
    head_end = html.index("</head>")
    body_start = html.index("<body")
    noscript = html.index(f"<noscript><div><img src=\"{WATCH_PIXEL}\"")

    # The script stays in <head>, the pixel opens <body>, both before content.
    assert html.index("<!-- Yandex.Metrika counter -->") < head_end
    assert head_end < body_start < noscript
    assert noscript < html.index("</noscript>", noscript) < html.index("</body>")
    assert html.rstrip().endswith("</html>")


@pytest.mark.parametrize("name", sorted(PUBLIC_PAGES))
def test_public_page_keeps_utf8_declaration(name: str) -> None:
    assert '<meta charset="utf-8">' in _html(PUBLIC_PAGES[name])


def test_admin_page_has_no_counter() -> None:
    html = _html(ADMIN_PAGE)
    assert COUNTER_ID not in html
    assert "mc.yandex.ru" not in html
    assert "Yandex.Metrika" not in html


@pytest.mark.parametrize("name", sorted(PUBLIC_ROUTES))
def test_route_serves_counter_as_html(client, name: str) -> None:
    response = client.get(PUBLIC_ROUTES[name])

    assert response.status_code == 200
    content_type = response.headers["content-type"]
    assert content_type.startswith("text/html")
    assert "charset=utf-8" in content_type.lower()
    assert response.text.count(f"id={COUNTER_ID}") == 1
    assert TAG_SCRIPT in response.text


def test_admin_route_has_no_counter(client) -> None:
    response = client.get("/admin/")

    assert response.status_code == 200
    assert COUNTER_ID not in response.text


@pytest.mark.parametrize("name", sorted(PUBLIC_ROUTES))
def test_public_route_csp_allows_metrika(client, name: str) -> None:
    """No CSP, or a CSP that explicitly permits mc.yandex.ru."""
    response = client.get(PUBLIC_ROUTES[name])
    csp = response.headers.get("content-security-policy")
    if csp is None:
        return  # No CSP at all: the counter cannot be blocked.

    lowered = csp.lower()
    assert "script-src" in lowered and "img-src" in lowered and "connect-src" in lowered
    assert "https://mc.yandex.ru" in lowered
