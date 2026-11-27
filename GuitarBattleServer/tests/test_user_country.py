"""Country of registration: IP extraction, geo-IP lookup and the admin API.

The suite has three layers:

* pure unit tests for :mod:`app.services.client_ip` and
  :mod:`app.services.geolocation` (no database, no network);
* database tests that register and sign in over HTTP and then read the stored
  columns, plus the admin statistics contract (marked ``db``);
* static tests that pin the admin table column, the country filter and the
  cache-busting versions.
"""
from __future__ import annotations

import uuid
from pathlib import Path

import pytest
from sqlalchemy import select, update
from starlette.requests import Request

from app.config import settings
from app.models.user import User
from app.services import geolocation
from app.services.client_ip import client_ip
from tests import helpers

ADMIN_STATIC = Path(__file__).resolve().parents[1] / "app" / "static" / "admin"

GERMANY = {"success": True, "country": "Germany", "country_code": "DE"}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _request(
    headers: dict[str, str] | None = None,
    client: tuple[str, int] | None = ("testclient", 50000),
) -> Request:
    raw_headers = [
        (name.lower().encode("latin-1"), value.encode("latin-1"))
        for name, value in (headers or {}).items()
    ]
    scope = {
        "type": "http",
        "http_version": "1.1",
        "method": "GET",
        "scheme": "http",
        "path": "/",
        "raw_path": b"/",
        "query_string": b"",
        "root_path": "",
        "headers": raw_headers,
        "client": client,
        "server": ("testserver", 80),
    }
    return Request(scope)


@pytest.fixture(autouse=True)
def _clear_geo_cache(monkeypatch):
    """The TTL cache is process-wide; every test starts cold.

    The defaults are pinned too, so a developer's ``.env`` cannot decide
    whether these tests talk to the network.
    """
    monkeypatch.setattr(settings, "geoip_enabled", True)
    monkeypatch.setattr(settings, "geoip_url", "https://ipwho.is")
    monkeypatch.setattr(settings, "geoip_timeout_seconds", 2.0)
    geolocation.clear_cache()
    yield
    geolocation.clear_cache()


def _register(db_client, *, ip: str | None = None, **overrides):
    """Register an account with an explicit ``X-Forwarded-For`` header."""
    username = overrides.pop("username", None) or helpers.unique("geo")
    payload = {
        "username": username,
        "email": f"{username}@example.com",
        "password": helpers.DEFAULT_PASSWORD,
        "privacy_consent": True,
        **overrides,
    }
    headers = {"X-Forwarded-For": ip} if ip else {}
    response = db_client.post(f"{helpers.API}/auth/register", json=payload, headers=headers)
    assert response.status_code == 201, response.text
    return username, payload["email"]


def _verify(db_client, email: str) -> None:
    response = db_client.post(
        f"{helpers.API}/auth/verify-email",
        json={"email": email, "code": helpers.verification_code(email)},
    )
    assert response.status_code == 200, response.text


def _login(db_client, identifier: str, *, ip: str | None = None):
    headers = {"X-Forwarded-For": ip} if ip else {}
    return db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": identifier, "password": helpers.DEFAULT_PASSWORD},
        headers=headers,
    )


# ---------------------------------------------------------------------------
# client_ip
# ---------------------------------------------------------------------------
def test_client_ip_uses_the_first_forwarded_address():
    request = _request({"X-Forwarded-For": "8.8.8.8, 10.0.0.1, 127.0.0.1"})
    assert client_ip(request) == "8.8.8.8"


def test_client_ip_normalizes_ipv6():
    request = _request(
        {"X-Forwarded-For": "2A00:1450:4001:082B:0000:0000:0000:200E"}
    )
    assert client_ip(request) == "2a00:1450:4001:82b::200e"


def test_client_ip_strips_the_port():
    assert client_ip(_request({"X-Forwarded-For": "8.8.8.8:5678"})) == "8.8.8.8"
    assert (
        client_ip(_request({"X-Forwarded-For": "[2001:4860:4860::8888]:443"}))
        == "2001:4860:4860::8888"
    )
    assert client_ip(_request({"X-Forwarded-For": "[::1]"})) is None


@pytest.mark.parametrize(
    "value",
    [
        "192.168.1.10",  # private
        "10.1.2.3",  # private
        "172.16.0.1",  # private
        "127.0.0.1",  # loopback
        "::1",  # loopback
        "fe80::1",  # link-local
        "0.0.0.0",  # unspecified
        "224.0.0.1",  # multicast
        "203.0.113.7",  # documentation range, not routable
        "not-an-ip",
        "1.2.3",
        "8.8.8.8:port",
        "",
        "   ",
    ],
)
def test_client_ip_rejects_unusable_addresses(value):
    assert client_ip(_request({"X-Forwarded-For": value})) is None


def test_client_ip_falls_back_to_the_socket_peer():
    assert client_ip(_request(client=("8.8.4.4", 4321))) == "8.8.4.4"
    # Starlette's TestClient peer is the literal "testclient", not an address.
    assert client_ip(_request()) is None
    assert client_ip(_request(client=None)) is None


# ---------------------------------------------------------------------------
# Geolocation
# ---------------------------------------------------------------------------
def test_resolve_country_success(monkeypatch):
    calls: list[str] = []

    def fake_fetch(url: str):
        calls.append(url)
        return dict(GERMANY)

    monkeypatch.setattr(geolocation, "_fetch_json", fake_fetch)
    assert geolocation.resolve_country("8.8.8.8") == ("DE", "Germany")
    assert len(calls) == 1
    assert calls[0] == f"{settings.geoip_url.rstrip('/')}/8.8.8.8"


def test_resolve_country_normalizes_the_code(monkeypatch):
    monkeypatch.setattr(
        geolocation,
        "_fetch_json",
        lambda url: {"success": True, "country": " Germany ", "country_code": " de "},
    )
    assert geolocation.resolve_country("8.8.8.8") == ("DE", "Germany")


def test_resolve_country_ignores_unsuccessful_payloads(monkeypatch):
    monkeypatch.setattr(
        geolocation,
        "_fetch_json",
        lambda url: {"success": False, "message": "reserved range"},
    )
    assert geolocation.resolve_country("8.8.8.8") is None


@pytest.mark.parametrize(
    "error", [TimeoutError("timed out"), OSError("dns failure"), ValueError("bad json")]
)
def test_resolve_country_swallows_transport_errors(monkeypatch, error):
    def boom(url: str):
        raise error

    monkeypatch.setattr(geolocation, "_fetch_json", boom)
    assert geolocation.resolve_country("8.8.8.8") is None


@pytest.mark.parametrize(
    "payload",
    [
        {"success": True, "country": "Germany", "country_code": ""},
        {"success": True, "country": "Germany", "country_code": None},
        {"success": True, "country": "Germany", "country_code": "DEU"},
        {"success": True, "country": "Germany", "country_code": 42},
        {"success": True, "country": "Germany"},
        {"country": "Germany", "country_code": "DE"},
        ["DE"],
        None,
    ],
)
def test_resolve_country_rejects_incomplete_payloads(monkeypatch, payload):
    monkeypatch.setattr(geolocation, "_fetch_json", lambda url: payload)
    assert geolocation.resolve_country("8.8.8.8") is None


def test_resolve_country_caches_successes(monkeypatch):
    calls: list[str] = []

    def fake_fetch(url: str):
        calls.append(url)
        return dict(GERMANY)

    monkeypatch.setattr(geolocation, "_fetch_json", fake_fetch)
    assert geolocation.resolve_country("8.8.8.8") == ("DE", "Germany")
    assert geolocation.resolve_country("8.8.8.8") == ("DE", "Germany")
    assert len(calls) == 1


def test_resolve_country_does_not_cache_failures(monkeypatch):
    # A failed attempt must stay retryable: ``country_checked_at`` is NULL and
    # the next sign-in asks the provider again.
    calls: list[str] = []

    def fake_fetch(url: str):
        calls.append(url)
        return {"success": False}

    monkeypatch.setattr(geolocation, "_fetch_json", fake_fetch)
    assert geolocation.resolve_country("8.8.8.8") is None
    assert geolocation.resolve_country("8.8.8.8") is None
    assert len(calls) == 2


def test_resolve_country_skips_network_when_disabled(monkeypatch):
    calls: list[str] = []
    monkeypatch.setattr(settings, "geoip_enabled", False)
    monkeypatch.setattr(
        geolocation, "_fetch_json", lambda url: calls.append(url) or dict(GERMANY)
    )
    assert geolocation.resolve_country("8.8.8.8") is None
    assert calls == []


def test_resolve_country_never_looks_up_a_private_address(monkeypatch):
    def fail(url: str):  # pragma: no cover - must not be called
        raise AssertionError(f"unexpected lookup for {url}")

    monkeypatch.setattr(geolocation, "_fetch_json", fail)
    assert geolocation.resolve_country("192.168.1.5") is None
    assert geolocation.resolve_country("127.0.0.1") is None
    assert geolocation.resolve_country(None) is None


def test_country_flag():
    assert geolocation.country_flag("DE") == "🇩🇪"
    assert geolocation.country_flag("ru") == "🇷🇺"
    assert geolocation.country_flag(None) is None
    assert geolocation.country_flag("DEU") is None
    assert geolocation.country_flag("ДЕ") is None


# ---------------------------------------------------------------------------
# Registration and sign-in write the columns
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_registration_stores_ip_and_country(db_client, db_session, monkeypatch):
    monkeypatch.setattr(geolocation, "_fetch_json", lambda url: dict(GERMANY))

    username, _email = _register(db_client, ip="8.8.8.8")

    db_session.expire_all()
    user = db_session.scalar(select(User).where(User.username == username))
    assert user is not None
    assert user.registration_ip == "8.8.8.8"
    assert user.last_login_ip is None
    assert user.country_code == "DE"
    assert user.country_name == "Germany"
    assert user.country_checked_at is not None


@pytest.mark.db
def test_registration_without_a_usable_ip_stores_nothing(db_client, db_session, monkeypatch):
    def fail(url: str):  # pragma: no cover - must not be called
        raise AssertionError(f"unexpected lookup for {url}")

    monkeypatch.setattr(geolocation, "_fetch_json", fail)

    username, _email = _register(db_client)

    db_session.expire_all()
    user = db_session.scalar(select(User).where(User.username == username))
    assert user.registration_ip is None
    assert user.country_code is None
    assert user.country_checked_at is None


@pytest.mark.db
def test_login_updates_last_login_ip(db_client, db_session, monkeypatch):
    monkeypatch.setattr(geolocation, "_fetch_json", lambda url: dict(GERMANY))
    username, email = _register(db_client)
    _verify(db_client, email)

    response = _login(db_client, username, ip="1.1.1.1")
    assert response.status_code == 200, response.text

    db_session.expire_all()
    user = db_session.scalar(select(User).where(User.username == username))
    assert user.last_login_ip == "1.1.1.1"
    # The registration address is never overwritten by a later sign-in.
    assert user.registration_ip is None


@pytest.mark.db
def test_login_keeps_a_stored_public_ip_when_the_peer_is_private(
    db_client, db_session, monkeypatch
):
    monkeypatch.setattr(geolocation, "_fetch_json", lambda url: dict(GERMANY))
    username, email = _register(db_client, ip="8.8.8.8")
    _verify(db_client, email)

    response = _login(db_client, username, ip="192.168.1.7")
    assert response.status_code == 200, response.text

    db_session.expire_all()
    user = db_session.scalar(select(User).where(User.username == username))
    assert user.registration_ip == "8.8.8.8"
    assert user.last_login_ip is None


@pytest.mark.db
def test_failed_lookup_leaves_the_retry_open_for_the_next_login(
    db_client, db_session, monkeypatch
):
    monkeypatch.setattr(geolocation, "_fetch_json", lambda url: {"success": False})
    username, email = _register(db_client, ip="8.8.8.8")

    db_session.expire_all()
    user = db_session.scalar(select(User).where(User.username == username))
    assert user.registration_ip == "8.8.8.8"
    assert user.country_code is None
    # NULL means "not resolved yet", so the next sign-in tries again.
    assert user.country_checked_at is None

    _verify(db_client, email)
    monkeypatch.setattr(geolocation, "_fetch_json", lambda url: dict(GERMANY))
    response = _login(db_client, username, ip="8.8.8.8")
    assert response.status_code == 200, response.text

    db_session.expire_all()
    user = db_session.scalar(select(User).where(User.username == username))
    assert user.country_code == "DE"
    assert user.country_name == "Germany"
    assert user.country_checked_at is not None


@pytest.mark.db
def test_known_country_is_not_resolved_again(db_client, db_session, monkeypatch):
    calls: list[str] = []

    def fake_fetch(url: str):
        calls.append(url)
        return dict(GERMANY)

    monkeypatch.setattr(geolocation, "_fetch_json", fake_fetch)
    username, email = _register(db_client, ip="8.8.8.8")
    assert len(calls) == 1

    _verify(db_client, email)
    response = _login(db_client, username, ip="8.8.8.8")
    assert response.status_code == 200, response.text
    # The account already has a country: no second provider call.
    assert len(calls) == 1


@pytest.mark.db
def test_geoip_can_be_disabled_without_losing_the_ip(db_client, db_session, monkeypatch):
    monkeypatch.setattr(settings, "geoip_enabled", False)

    def fail(url: str):  # pragma: no cover - must not be called
        raise AssertionError(f"unexpected lookup for {url}")

    monkeypatch.setattr(geolocation, "_fetch_json", fail)
    username, _email = _register(db_client, ip="8.8.8.8")

    db_session.expire_all()
    user = db_session.scalar(select(User).where(User.username == username))
    assert user.registration_ip == "8.8.8.8"
    assert user.country_code is None
    assert user.country_checked_at is None


# ---------------------------------------------------------------------------
# Admin API
# ---------------------------------------------------------------------------
def _set_country(db_session, user_id: str, code: str, name: str) -> None:
    db_session.execute(
        update(User)
        .where(User.id == uuid.UUID(user_id))
        .values(country_code=code, country_name=name)
    )


@pytest.mark.db
def test_admin_users_expose_country_and_ips(db_client, db_session, admin_user):
    player = helpers.register_user(db_client)
    db_session.execute(
        update(User)
        .where(User.id == uuid.UUID(player["id"]))
        .values(
            country_code="DE",
            country_name="Germany",
            registration_ip="8.8.8.8",
            last_login_ip="1.1.1.1",
        )
    )
    db_session.commit()

    response = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"search": player["username"]},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    row = response.json()["items"][0]
    assert row["country_code"] == "DE"
    assert row["country_name"] == "Germany"
    assert row["country_flag"] == "🇩🇪"
    assert row["registration_ip"] == "8.8.8.8"
    assert row["last_login_ip"] == "1.1.1.1"


@pytest.mark.db
def test_admin_users_without_a_country_return_nulls(db_client, admin_user):
    player = helpers.register_user(db_client)

    response = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"search": player["username"]},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    row = response.json()["items"][0]
    # Additive fields: always present, NULL until a country is resolved.
    assert row["country_code"] is None
    assert row["country_name"] is None
    assert row["country_flag"] is None
    assert row["registration_ip"] is None
    assert row["last_login_ip"] is None


@pytest.mark.db
def test_admin_users_filter_by_country(db_client, db_session, admin_user):
    german = helpers.register_user(db_client)
    russian = helpers.register_user(db_client)
    _set_country(db_session, german["id"], "DE", "Germany")
    _set_country(db_session, russian["id"], "RU", "Russia")
    db_session.commit()

    response = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"country": "DE"},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    payload = response.json()
    assert payload["total"] == 1
    assert [item["user_id"] for item in payload["items"]] == [german["id"]]

    # Lower case is accepted and normalised; the stored code is upper case.
    lowered = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"country": "de"},
        headers=admin_user["headers"],
    )
    assert lowered.status_code == 200, lowered.text
    assert lowered.json()["total"] == 1

    # The filter combines with the other parameters instead of replacing them.
    combined = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"country": "RU", "search": russian["username"], "limit": 10},
        headers=admin_user["headers"],
    )
    assert combined.status_code == 200, combined.text
    assert [item["user_id"] for item in combined.json()["items"]] == [russian["id"]]

    empty = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"country": "FR"},
        headers=admin_user["headers"],
    )
    assert empty.status_code == 200, empty.text
    assert empty.json()["total"] == 0
    assert empty.json()["items"] == []


@pytest.mark.db
@pytest.mark.parametrize("country", ["DEU", "D", "12", "D1"])
def test_admin_users_reject_an_invalid_country(db_client, admin_user, country):
    response = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"country": country},
        headers=admin_user["headers"],
    )
    assert response.status_code == 422, response.text


@pytest.mark.db
def test_overview_countries_are_the_top_ten_by_users(db_client, db_session, admin_user):
    # Two accounts for DE and RU, one for nine other countries: eleven groups,
    # so the tenth row is the last one in the list.
    codes = {
        "DE": ("Germany", 2),
        "RU": ("Russia", 2),
        "AT": ("Austria", 1),
        "BE": ("Belgium", 1),
        "CH": ("Switzerland", 1),
        "DK": ("Denmark", 1),
        "ES": ("Spain", 1),
        "FI": ("Finland", 1),
        "GR": ("Greece", 1),
        "HU": ("Hungary", 1),
        "IE": ("Ireland", 1),
    }
    users = []
    for code, (name, count) in codes.items():
        for index in range(count):
            username = f"ctry_{code.lower()}_{index}"
            users.append(
                User(
                    username=username,
                    email=f"{username}@example.com",
                    password_hash="x",
                    display_name="",
                    country_code=code,
                    country_name=name,
                )
            )
    db_session.add_all(users)
    db_session.commit()

    response = db_client.get(
        f"{helpers.API}/admin/statistics/overview",
        params={"days": 30},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    countries = response.json()["countries"]

    assert len(countries) == 10
    # Most players first, ties broken by the code so paging is deterministic.
    assert [item["code"] for item in countries[:2]] == ["DE", "RU"]
    assert [item["users"] for item in countries[:2]] == [2, 2]
    assert all(item["users"] == 1 for item in countries[2:])
    assert [item["code"] for item in countries[2:]] == [
        "AT",
        "BE",
        "CH",
        "DK",
        "ES",
        "FI",
        "GR",
        "HU",
    ]
    assert "IE" not in [item["code"] for item in countries]
    assert countries[0]["name"] == "Germany"


@pytest.mark.db
def test_overview_countries_ignore_accounts_without_a_country(db_client, admin_user):
    helpers.register_user(db_client)

    response = db_client.get(
        f"{helpers.API}/admin/statistics/overview",
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    assert response.json()["countries"] == []


# ---------------------------------------------------------------------------
# Admin page (static)
# ---------------------------------------------------------------------------
def test_admin_users_table_has_a_country_column():
    html = (ADMIN_STATIC / "index.html").read_text(encoding="utf-8")
    assert "Страна" in html
    # The loading row must span every column of the users table.
    assert 'colspan="16"' in html


def test_admin_statistics_has_a_country_filter():
    html = (ADMIN_STATIC / "index.html").read_text(encoding="utf-8")
    assert 'id="statsCountry"' in html
    assert "Все страны" in html

    script = (ADMIN_STATIC / "admin-statistics.js").read_text(encoding="utf-8")
    assert "statsCountry" in script
    assert "renderCountryOptions" in script
    assert "countryCell" in script
    assert 'params.set("country", state.country)' in script
    assert 'colspan="16"' in script


def test_admin_assets_are_cache_busted():
    html = (ADMIN_STATIC / "index.html").read_text(encoding="utf-8")
    assert "admin-statistics.js?v=20261122-1" in html
    assert "admin.css?v=20261124-1" in html
