"""Registration, login and profile contract tests."""
import uuid

import pytest

from tests import helpers

pytestmark = pytest.mark.db


def test_register_returns_public_profile(client, db_client):
    username = helpers.unique("user").upper()
    email = f"{helpers.unique('mail')}@Example.COM"
    response = db_client.post(
        f"{helpers.API}/auth/register",
        json={
            "username": username,
            "email": email,
            "password": helpers.DEFAULT_PASSWORD,
        },
    )
    assert response.status_code == 201, response.text
    body = response.json()
    assert body["username"] == username.lower()
    assert body["email"] == email.lower()
    assert body["display_name"] == username.lower()
    assert body["email_verified"] is False
    assert body["avatar_key"] is None
    # Password material must never leak through the API.
    assert "password" not in body
    assert "password_hash" not in body
    assert uuid.UUID(body["id"])


def test_duplicate_username_is_rejected(db_client):
    account = helpers.register_user(db_client)
    response = db_client.post(
        f"{helpers.API}/auth/register",
        json={
            "username": account["username"],
            "email": f"{helpers.unique('other')}@example.com",
            "password": helpers.DEFAULT_PASSWORD,
        },
    )
    assert response.status_code == 409
    assert "already registered" in response.json()["detail"]


def test_duplicate_email_is_rejected_case_insensitively(db_client):
    account = helpers.register_user(db_client)
    response = db_client.post(
        f"{helpers.API}/auth/register",
        json={
            "username": helpers.unique("other"),
            "email": account["email"].upper(),
            "password": helpers.DEFAULT_PASSWORD,
        },
    )
    assert response.status_code == 409


@pytest.mark.parametrize(
    "overrides",
    [
        {"username": "ab"},
        {"username": "bad name"},
        {"username": "bad$name"},
        {"email": "not-an-email"},
        {"password": "short"},
        {"display_name": "x" * 65},
    ],
)
def test_register_validates_payload(db_client, overrides):
    payload = {
        "username": helpers.unique("user"),
        "email": f"{helpers.unique('mail')}@example.com",
        "password": helpers.DEFAULT_PASSWORD,
    }
    payload.update(overrides)
    response = db_client.post(f"{helpers.API}/auth/register", json=payload)
    assert response.status_code == 422, response.text


@pytest.mark.parametrize("use_email", [False, True])
def test_login_accepts_username_or_email(db_client, use_email):
    account = helpers.register_user(db_client)
    identifier = account["email"] if use_email else account["username"]
    response = db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": identifier, "password": account["password"]},
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["token_type"] == "bearer"
    assert body["access_token"]
    assert body["expires_in"] == 60 * 60  # conftest sets ACCESS_TOKEN_EXPIRE_MINUTES=60


@pytest.mark.parametrize("password", ["wrong-password", "GuitarBattleTest12", ""])
def test_login_rejects_bad_password(db_client, password):
    account = helpers.register_user(db_client)
    response = db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": account["username"], "password": password},
    )
    assert response.status_code in {401, 422}
    if response.status_code == 401:
        assert response.headers.get("www-authenticate") == "Bearer"


def test_login_rejects_unknown_user(db_client):
    response = db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": helpers.unique("ghost"), "password": helpers.DEFAULT_PASSWORD},
    )
    assert response.status_code == 401
    assert "Incorrect" in response.json()["detail"]


def test_me_requires_valid_token(db_client, user_account):
    missing = db_client.get(f"{helpers.API}/auth/me")
    assert missing.status_code == 401

    malformed = db_client.get(
        f"{helpers.API}/auth/me", headers={"Authorization": "Bearer not-a-jwt"}
    )
    assert malformed.status_code == 401

    ok = db_client.get(f"{helpers.API}/auth/me", headers=user_account["headers"])
    assert ok.status_code == 200
    body = ok.json()
    assert body["id"] == user_account["id"]
    assert body["username"] == user_account["username"]
    assert body["email"] == user_account["email"]


def test_patch_me_updates_display_name(db_client, user_account):
    response = db_client.patch(
        f"{helpers.API}/auth/me",
        json={"display_name": "  New Display Name  "},
        headers=user_account["headers"],
    )
    assert response.status_code == 200, response.text
    assert response.json()["display_name"] == "New Display Name"


@pytest.mark.parametrize("display_name", ["", "   ", "x" * 65])
def test_patch_me_rejects_invalid_display_name(db_client, user_account, display_name):
    response = db_client.patch(
        f"{helpers.API}/auth/me",
        json={"display_name": display_name},
        headers=user_account["headers"],
    )
    assert response.status_code == 422


def test_inactive_user_cannot_use_token_or_login(db_client, db_session, user_account):
    from app.models.user import User

    db_session.execute(
        User.__table__.update()
        .where(User.id == uuid.UUID(user_account["id"]))
        .values(is_active=False)
    )
    db_session.commit()

    me = db_client.get(f"{helpers.API}/auth/me", headers=user_account["headers"])
    assert me.status_code == 401

    login = db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": user_account["username"], "password": user_account["password"]},
    )
    assert login.status_code == 401
