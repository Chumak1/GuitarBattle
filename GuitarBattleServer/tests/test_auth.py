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
            "privacy_consent": True,
        },
    )
    assert response.status_code == 201, response.text
    body = response.json()
    assert body["username"] == username.lower()
    assert body["email"] == email.lower()
    assert body["display_name"] == username.lower()
    assert body["email_verified"] is False
    assert body["avatar_key"] is None
    # Consent is recorded exactly once, at registration.
    assert body["privacy_consent_at"]
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
            "privacy_consent": True,
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
            "privacy_consent": True,
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
        "privacy_consent": True,
    }
    payload.update(overrides)
    response = db_client.post(f"{helpers.API}/auth/register", json=payload)
    assert response.status_code == 422, response.text


@pytest.mark.parametrize("consent", [None, False])
def test_register_requires_privacy_consent(db_client, consent):
    username = helpers.unique("noconsent")
    payload = {
        "username": username,
        "email": f"{username}@example.com",
        "password": helpers.DEFAULT_PASSWORD,
    }
    if consent is not None:
        payload["privacy_consent"] = consent
    response = db_client.post(f"{helpers.API}/auth/register", json=payload)
    assert response.status_code == 422, response.text
    assert "consent" in response.json()["detail"].lower()

    # Nothing was created.
    login = db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": username, "password": helpers.DEFAULT_PASSWORD},
    )
    assert login.status_code == 401


def test_login_ignores_privacy_consent_and_keeps_consent_timestamp(db_client, db_session):
    from sqlalchemy import select

    from app.models.user import User

    account = helpers.register_user(db_client)
    before = db_session.scalar(
        select(User.privacy_consent_at).where(User.id == uuid.UUID(account["id"]))
    )
    assert before is not None

    response = db_client.post(
        f"{helpers.API}/auth/login",
        json={
            "identifier": account["username"],
            "password": account["password"],
            "privacy_consent": True,
        },
    )
    assert response.status_code == 200, response.text
    db_session.expire_all()
    after = db_session.scalar(
        select(User.privacy_consent_at).where(User.id == uuid.UUID(account["id"]))
    )
    assert after == before

    me = db_client.get(f"{helpers.API}/auth/me", headers=account["headers"])
    assert me.status_code == 200
    assert me.json()["privacy_consent_at"] is not None


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


def test_registration_requires_email_confirmation_before_login(db_client):
    username = helpers.unique("unverified")
    email = f"{username}@example.com"
    response = db_client.post(
        f"{helpers.API}/auth/register",
        json={
            "username": username,
            "email": email,
            "password": helpers.DEFAULT_PASSWORD,
            "privacy_consent": True,
        },
    )
    assert response.status_code == 201, response.text
    assert response.json()["email_verified"] is False

    blocked = db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": username, "password": helpers.DEFAULT_PASSWORD},
    )
    assert blocked.status_code == 403
    assert "not verified" in blocked.json()["detail"].lower()

    verify = db_client.post(
        f"{helpers.API}/auth/verify-email",
        json={"email": email, "code": helpers.verification_code(email)},
    )
    assert verify.status_code == 200, verify.text
    token = verify.json()["access_token"]

    profile = db_client.get(
        f"{helpers.API}/auth/me", headers=helpers.auth_headers(token)
    )
    assert profile.status_code == 200
    assert profile.json()["email_verified"] is True

    signed_in = db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": email, "password": helpers.DEFAULT_PASSWORD},
    )
    assert signed_in.status_code == 200

    repeated = db_client.post(
        f"{helpers.API}/auth/verify-email",
        json={"email": email, "code": helpers.verification_code(email)},
    )
    assert repeated.status_code == 400


def test_verification_code_attempts_are_limited_and_resend_recovers(
    db_client, monkeypatch
):
    from app.config import settings

    monkeypatch.setattr(settings, "email_code_resend_seconds", 0)
    username = helpers.unique("attempts")
    email = f"{username}@example.com"
    response = db_client.post(
        f"{helpers.API}/auth/register",
        json={
            "username": username,
            "email": email,
            "password": helpers.DEFAULT_PASSWORD,
            "privacy_consent": True,
        },
    )
    assert response.status_code == 201, response.text

    correct_code = helpers.verification_code(email)
    wrong_code = "000000" if correct_code != "000000" else "111111"
    for _ in range(settings.email_code_max_attempts):
        rejected = db_client.post(
            f"{helpers.API}/auth/verify-email",
            json={"email": email, "code": wrong_code},
        )
        assert rejected.status_code == 400

    exhausted = db_client.post(
        f"{helpers.API}/auth/verify-email",
        json={"email": email, "code": correct_code},
    )
    assert exhausted.status_code == 400

    resent = db_client.post(
        f"{helpers.API}/auth/resend-verification", json={"email": email}
    )
    assert resent.status_code == 202, resent.text
    fresh_code = helpers.verification_code(email)
    verify = db_client.post(
        f"{helpers.API}/auth/verify-email",
        json={"email": email, "code": fresh_code},
    )
    assert verify.status_code == 200, verify.text


def test_forgot_password_resets_password_and_invalidates_old_tokens(db_client):
    account = helpers.register_user(db_client)
    old_token = account["token"]
    forgot = db_client.post(
        f"{helpers.API}/auth/forgot-password", json={"email": account["email"]}
    )
    assert forgot.status_code == 202, forgot.text

    reset_code = helpers.password_reset_code(account["email"])
    new_password = "NewGuitarBattlePassword123"
    reset = db_client.post(
        f"{helpers.API}/auth/reset-password",
        json={
            "email": account["email"],
            "code": reset_code,
            "new_password": new_password,
        },
    )
    assert reset.status_code == 200, reset.text
    new_token = reset.json()["access_token"]

    assert db_client.get(
        f"{helpers.API}/auth/me", headers=helpers.auth_headers(old_token)
    ).status_code == 401
    assert db_client.get(
        f"{helpers.API}/auth/me", headers=helpers.auth_headers(new_token)
    ).status_code == 200

    old_login = db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": account["username"], "password": account["password"]},
    )
    assert old_login.status_code == 401

    new_login = db_client.post(
        f"{helpers.API}/auth/login",
        json={"identifier": account["username"], "password": new_password},
    )
    assert new_login.status_code == 200


def test_password_reset_rejects_bad_code_and_hides_unknown_email(db_client):
    from app.services.email_service import get_test_outbox

    account = helpers.register_user(db_client)
    forgot = db_client.post(
        f"{helpers.API}/auth/forgot-password", json={"email": account["email"]}
    )
    assert forgot.status_code == 202
    correct_code = helpers.password_reset_code(account["email"])
    wrong_code = "000000" if correct_code != "000000" else "111111"

    rejected = db_client.post(
        f"{helpers.API}/auth/reset-password",
        json={
            "email": account["email"],
            "code": wrong_code,
            "new_password": "NewGuitarBattlePassword123",
        },
    )
    assert rejected.status_code == 400

    before = len(get_test_outbox())
    unknown = f"{helpers.unique('ghost')}@example.com"
    response = db_client.post(
        f"{helpers.API}/auth/forgot-password", json={"email": unknown}
    )
    assert response.status_code == 202
    assert len(get_test_outbox()) == before

    resend = db_client.post(
        f"{helpers.API}/auth/resend-verification", json={"email": unknown}
    )
    assert resend.status_code == 202
    assert len(get_test_outbox()) == before


def test_registration_rolls_back_when_verification_email_fails(
    db_client, db_session, monkeypatch
):
    from sqlalchemy import select

    from app.api.v1 import auth as auth_module
    from app.models.user import User
    from app.services.email_service import EmailDeliveryError

    def fail_delivery(**_kwargs):
        raise EmailDeliveryError("SMTP unavailable")

    monkeypatch.setattr(auth_module, "send_verification_email", fail_delivery)
    username = helpers.unique("nomail")
    email = f"{username}@example.com"
    response = db_client.post(
        f"{helpers.API}/auth/register",
        json={
            "username": username,
            "email": email,
            "password": helpers.DEFAULT_PASSWORD,
            "privacy_consent": True,
        },
    )
    assert response.status_code == 503

    user = db_session.scalar(select(User).where(User.email == email))
    assert user is None


def test_change_password_invalidates_existing_token(db_client, user_account):
    response = db_client.post(
        f"{helpers.API}/auth/change-password",
        json={
            "current_password": user_account["password"],
            "new_password": "ChangedGuitarBattle123",
        },
        headers=user_account["headers"],
    )
    assert response.status_code == 204, response.text

    assert db_client.get(
        f"{helpers.API}/auth/me", headers=user_account["headers"]
    ).status_code == 401


def test_change_password_invalidates_pending_reset_code(db_client, user_account):
    forgot = db_client.post(
        f"{helpers.API}/auth/forgot-password",
        json={"email": user_account["email"]},
    )
    assert forgot.status_code == 202, forgot.text
    reset_code = helpers.password_reset_code(user_account["email"])

    changed = db_client.post(
        f"{helpers.API}/auth/change-password",
        json={
            "current_password": user_account["password"],
            "new_password": "ChangedGuitarBattle123",
        },
        headers=user_account["headers"],
    )
    assert changed.status_code == 204, changed.text

    # The code requested before the password change must be dead.
    reused = db_client.post(
        f"{helpers.API}/auth/reset-password",
        json={
            "email": user_account["email"],
            "code": reset_code,
            "new_password": "AnotherGuitarBattle123",
        },
    )
    assert reused.status_code == 400


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
