"""Persistent sessions: rotating refresh tokens, cookie flags and logout.

The desktop client keeps using ``access_token`` and may send the refresh token
in the JSON body; the web app relies on the HttpOnly ``ff_refresh`` cookie.
"""
import uuid

import pytest
from sqlalchemy import select

from app.models.refresh_session import RefreshSession
from app.services.refresh_tokens import hash_refresh_token
from tests import helpers

pytestmark = pytest.mark.db


def _login(db_client, account, **extra):
    payload = {"identifier": account["username"], "password": account["password"]}
    payload.update(extra)
    response = db_client.post(f"{helpers.API}/auth/login", json=payload)
    assert response.status_code == 200, response.text
    return response


def _cookie_header(response) -> str:
    return response.headers.get("set-cookie", "")


def test_login_returns_refresh_token_and_sets_cookie(db_client):
    account = helpers.register_user(db_client)
    db_client.cookies.clear()

    response = _login(db_client, account)
    body = response.json()

    # Existing fields are unchanged, the new ones are additive.
    assert body["token_type"] == "bearer"
    assert body["access_token"]
    assert body["expires_in"] == 60 * 60
    assert body["refresh_token"]
    assert body["refresh_expires_in"] == 30 * 24 * 60 * 60

    cookie = _cookie_header(response)
    assert "ff_refresh=" in cookie
    assert "HttpOnly" in cookie
    assert "SameSite=lax" in cookie
    assert "Path=/api/v1/auth" in cookie
    assert "Max-Age=2592000" in cookie
    # The test request is plain HTTP, so the cookie is not marked Secure.
    assert "Secure" not in cookie


def test_refresh_cookie_is_secure_behind_https_proxy(db_client):
    account = helpers.register_user(db_client)
    db_client.cookies.clear()
    response = db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": account["username"], "password": account["password"]},
        headers={"X-Forwarded-Proto": "https"},
    )
    assert response.status_code == 200, response.text
    cookie = _cookie_header(response)
    assert "Secure" in cookie
    assert "HttpOnly" in cookie
    assert "SameSite=lax" in cookie


def test_refresh_tokens_are_stored_only_as_hashes(db_client, db_session):
    account = helpers.register_user(db_client)
    db_client.cookies.clear()
    login = _login(db_client, account).json()

    row = db_session.scalar(
        select(RefreshSession).where(
            RefreshSession.token_hash == hash_refresh_token(login["refresh_token"])
        )
    )
    assert row is not None
    assert row.token_hash != login["refresh_token"]
    assert row.token_hash == hash_refresh_token(login["refresh_token"])
    assert len(row.token_hash) == 64
    assert row.revoked_at is None
    assert row.expires_at is not None
    assert row.replaced_by_id is None


def test_refresh_rotates_the_session_and_returns_new_tokens(db_client, db_session):
    account = helpers.register_user(db_client)
    db_client.cookies.clear()
    first = _login(db_client, account).json()

    refreshed = db_client.post(f"{helpers.API}/auth/refresh")
    assert refreshed.status_code == 200, refreshed.text
    second = refreshed.json()
    assert second["refresh_token"] != first["refresh_token"]
    assert second["access_token"]
    assert "ff_refresh=" in _cookie_header(refreshed)

    # The new access token works and the rotated row points at its successor.
    me = db_client.get(
        f"{helpers.API}/auth/me", headers=helpers.auth_headers(second["access_token"])
    )
    assert me.status_code == 200

    old_row = db_session.scalar(
        select(RefreshSession).where(
            RefreshSession.token_hash == hash_refresh_token(first["refresh_token"])
        )
    )
    new_row = db_session.scalar(
        select(RefreshSession).where(
            RefreshSession.token_hash == hash_refresh_token(second["refresh_token"])
        )
    )
    assert old_row.revoked_at is not None
    assert old_row.replaced_by_id == new_row.id
    assert new_row.revoked_at is None


def test_refresh_accepts_body_token_without_cookie(db_client):
    account = helpers.register_user(db_client)
    db_client.cookies.clear()
    login = _login(db_client, account).json()

    # The desktop client has no cookie jar.
    db_client.cookies.clear()
    refreshed = db_client.post(
        f"{helpers.API}/auth/refresh",
        json={"refresh_token": login["refresh_token"]},
    )
    assert refreshed.status_code == 200, refreshed.text
    assert refreshed.json()["refresh_token"] != login["refresh_token"]


def test_reused_refresh_token_revokes_the_whole_chain(
    db_client, db_session, monkeypatch
):
    from app.config import settings

    # Disable the "two tabs racing" grace window: reuse is treated as theft.
    monkeypatch.setattr(settings, "refresh_reuse_grace_seconds", 0)

    account = helpers.register_user(db_client)
    db_client.cookies.clear()
    first = _login(db_client, account).json()
    second = db_client.post(f"{helpers.API}/auth/refresh").json()

    # Present the already rotated token again.
    db_client.cookies.clear()
    reused = db_client.post(
        f"{helpers.API}/auth/refresh",
        json={"refresh_token": first["refresh_token"]},
    )
    assert reused.status_code == 401
    assert reused.json()["detail"] == "Invalid or expired refresh token"

    # The successor is revoked as well: the whole chain is dead.
    db_client.cookies.clear()
    after = db_client.post(
        f"{helpers.API}/auth/refresh",
        json={"refresh_token": second["refresh_token"]},
    )
    assert after.status_code == 401

    db_session.expire_all()
    chain = db_session.scalars(
        select(RefreshSession).where(
            RefreshSession.token_hash.in_(
                [
                    hash_refresh_token(first["refresh_token"]),
                    hash_refresh_token(second["refresh_token"]),
                ]
            )
        )
    ).all()
    assert len(chain) == 2
    assert all(row.revoked_at is not None for row in chain)


def test_immediate_reuse_within_grace_keeps_the_new_session(db_client):
    """Two tabs refreshing at once must not log the user out."""
    account = helpers.register_user(db_client)
    db_client.cookies.clear()
    first = _login(db_client, account).json()
    second = db_client.post(f"{helpers.API}/auth/refresh").json()

    db_client.cookies.clear()
    raced = db_client.post(
        f"{helpers.API}/auth/refresh",
        json={"refresh_token": first["refresh_token"]},
    )
    assert raced.status_code == 401

    surface = db_client.post(
        f"{helpers.API}/auth/refresh",
        json={"refresh_token": second["refresh_token"]},
    )
    assert surface.status_code == 200, surface.text


def test_expired_refresh_token_is_rejected_and_revoked(db_client, db_session):
    from datetime import datetime, timedelta, timezone

    account = helpers.register_user(db_client)
    db_client.cookies.clear()
    login = _login(db_client, account).json()

    row = db_session.scalar(
        select(RefreshSession).where(
            RefreshSession.token_hash == hash_refresh_token(login["refresh_token"])
        )
    )
    row.expires_at = datetime.now(timezone.utc) - timedelta(minutes=1)
    db_session.commit()

    db_client.cookies.clear()
    response = db_client.post(
        f"{helpers.API}/auth/refresh",
        json={"refresh_token": login["refresh_token"]},
    )
    assert response.status_code == 401
    assert response.json()["detail"] == "Invalid or expired refresh token"

    db_session.expire_all()
    assert db_session.get(RefreshSession, row.id).revoked_at is not None


@pytest.mark.parametrize("token", [None, "", "not-a-real-token"])
def test_refresh_without_a_valid_token_is_401(db_client, token):
    db_client.cookies.clear()
    payload = {} if token is None else {"refresh_token": token}
    response = db_client.post(f"{helpers.API}/auth/refresh", json=payload)
    assert response.status_code == 401
    # Generic message: no state of the session leaks to the caller.
    assert response.json()["detail"] == "Invalid or expired refresh token"
    # A failed refresh also removes the unusable cookie.
    cookie = _cookie_header(response)
    assert "ff_refresh=" in cookie
    assert "Max-Age=0" in cookie


def test_refresh_without_any_body_is_401(db_client):
    db_client.cookies.clear()
    response = db_client.post(f"{helpers.API}/auth/refresh")
    assert response.status_code == 401


def test_logout_revokes_the_session_clears_cookie_and_is_idempotent(db_client, db_session):
    account = helpers.register_user(db_client)
    db_client.cookies.clear()
    login = _login(db_client, account).json()

    response = db_client.post(f"{helpers.API}/auth/logout")
    assert response.status_code == 204
    cookie = _cookie_header(response)
    assert "ff_refresh=" in cookie
    assert "Max-Age=0" in cookie

    db_client.cookies.clear()
    reused = db_client.post(
        f"{helpers.API}/auth/refresh",
        json={"refresh_token": login["refresh_token"]},
    )
    assert reused.status_code == 401

    # Logging out again (or without any token) stays a 204.
    assert db_client.post(f"{helpers.API}/auth/logout").status_code == 204

    row = db_session.scalar(
        select(RefreshSession).where(
            RefreshSession.token_hash == hash_refresh_token(login["refresh_token"])
        )
    )
    assert row.revoked_at is not None


def test_logout_accepts_body_token_for_desktop_client(db_client):
    account = helpers.register_user(db_client)
    db_client.cookies.clear()
    login = _login(db_client, account).json()

    db_client.cookies.clear()
    response = db_client.post(
        f"{helpers.API}/auth/logout", json={"refresh_token": login["refresh_token"]}
    )
    assert response.status_code == 204

    db_client.cookies.clear()
    assert (
        db_client.post(
            f"{helpers.API}/auth/refresh",
            json={"refresh_token": login["refresh_token"]},
        ).status_code
        == 401
    )


def test_logout_without_token_still_clears_the_cookie(db_client):
    db_client.cookies.clear()
    response = db_client.post(f"{helpers.API}/auth/logout")
    assert response.status_code == 204
    assert "ff_refresh=" in _cookie_header(response)


def test_password_change_revokes_refresh_sessions(db_client):
    account = helpers.register_user(db_client)
    db_client.cookies.clear()
    login = _login(db_client, account).json()

    changed = db_client.post(
        f"{helpers.API}/auth/change-password",
        json={
            "current_password": account["password"],
            "new_password": "ChangedGuitarBattle123",
        },
        headers=account["headers"],
    )
    assert changed.status_code == 204, changed.text

    db_client.cookies.clear()
    refused = db_client.post(
        f"{helpers.API}/auth/refresh",
        json={"refresh_token": login["refresh_token"]},
    )
    assert refused.status_code == 401


def test_legacy_account_has_no_consent_timestamp(db_client, db_session, user_account):
    from app.models.user import User

    db_session.execute(
        User.__table__.update()
        .where(User.id == uuid.UUID(user_account["id"]))
        .values(privacy_consent_at=None)
    )
    db_session.commit()

    me = db_client.get(f"{helpers.API}/auth/me", headers=user_account["headers"])
    assert me.status_code == 200
    assert me.json()["privacy_consent_at"] is None
