"""Security headers on the public site.

TLS terminates at Caddy (``deploy/Caddyfile``), and the HSTS/nosniff/referrer
headers are sent by the application itself: the API is the single place that
knows whether the request actually arrived over TLS (``X-Forwarded-Proto`` from
Caddy) and it behaves the same under a bare ``uvicorn`` in development. Putting
them in Caddy as well used to duplicate ``X-Content-Type-Options`` for proxied
responses, so the file has them only on the ``www`` redirect host.
"""
from __future__ import annotations

import pytest

BASELINE_HEADERS = {
    "x-content-type-options": "nosniff",
    "referrer-policy": "strict-origin-when-cross-origin",
}
HSTS = "max-age=86400"

# The landing page, the player web apps, the admin console and the API.
PUBLIC_PATHS = [
    "/",
    "/en",
    "/login",
    "/register",
    "/policy",
    "/account",
    "/admin/",
    "/health/live",
]


@pytest.mark.parametrize("path", PUBLIC_PATHS)
def test_baseline_headers_on_every_page(client, path):
    response = client.get(path)

    assert response.status_code == 200
    for name, value in BASELINE_HEADERS.items():
        assert response.headers[name] == value


@pytest.mark.parametrize("path", PUBLIC_PATHS)
def test_hsts_is_not_sent_over_plain_http(client, path):
    """Browsers ignore HSTS on HTTP; a development server must not claim it."""
    response = client.get(path)

    assert "strict-transport-security" not in response.headers


def test_hsts_is_sent_behind_the_tls_proxy(client):
    response = client.get("/health/live", headers={"X-Forwarded-Proto": "https"})

    assert response.headers["strict-transport-security"] == HSTS
    # One day only: no includeSubDomains, no preload (preload is irreversible).
    assert "includeSubdomains" not in response.headers["strict-transport-security"]
    assert "preload" not in response.headers["strict-transport-security"]


def test_hsts_uses_the_first_forwarded_proto(client):
    """Caddy appends the original scheme first; a later "http" must not win."""
    response = client.get(
        "/health/live", headers={"X-Forwarded-Proto": "https, http"}
    )
    assert response.headers["strict-transport-security"] == HSTS

    response = client.get(
        "/health/live", headers={"X-Forwarded-Proto": "http, https"}
    )
    assert "strict-transport-security" not in response.headers


@pytest.mark.parametrize("path", ["/admin", "/admin/", "/support", "/donate"])
def test_internal_pages_keep_the_strict_csp(client, path):
    response = client.get(path)

    csp = response.headers["content-security-policy"]
    assert "script-src 'self'" in csp
    assert "'unsafe-inline'" not in csp.split("script-src", 1)[1].split(";", 1)[0]


@pytest.mark.parametrize("path", ["/", "/en", "/login", "/account", "/policy"])
def test_public_pages_have_no_csp_that_blocks_metrika(client, path):
    """The marketing pages run Yandex.Metrika inline, so they stay unrestricted."""
    assert "content-security-policy" not in client.get(path).headers


@pytest.mark.db
def test_json_api_responses_carry_the_same_headers(db_client):
    response = db_client.get("/api/v1/songs", params={"limit": 1})

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("application/json")
    for name, value in BASELINE_HEADERS.items():
        assert response.headers[name] == value
