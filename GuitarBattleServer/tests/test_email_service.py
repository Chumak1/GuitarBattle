"""Unit tests for the transactional email service.

These tests never open a network connection. They replace ``smtplib.SMTP`` /
``smtplib.SMTP_SSL`` with a small fake and inspect the message that would be
handed to Yandex Cloud Postbox.
"""
from __future__ import annotations

import logging
import smtplib
from email.message import EmailMessage

import pytest
from pydantic import ValidationError

from app.config import Settings, settings
from app.services import email_service
from app.services.email_service import EmailDeliveryError


class FakeSMTP:
    instances: list["FakeSMTP"] = []

    def __init__(self, host: str, port: int, timeout: float | None = None,
                 context: object | None = None, **kwargs: object) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.context = context
        self.starttls_called = False
        self.tls_context: object | None = None
        self.login_args: tuple[str, str] | None = None
        self.message: EmailMessage | None = None
        self.ehlo_calls = 0
        FakeSMTP.instances.append(self)

    def __enter__(self) -> "FakeSMTP":
        return self

    def __exit__(self, *args: object) -> bool:
        return False

    def ehlo(self) -> tuple[int, bytes]:
        self.ehlo_calls += 1
        return 250, b"OK"

    def starttls(self, context: object | None = None) -> tuple[int, bytes]:
        self.starttls_called = True
        self.tls_context = context
        return 220, b"Ready to start TLS"

    def login(self, username: str, password: str) -> tuple[int, bytes]:
        self.login_args = (username, password)
        return 235, b"Authentication successful"

    def send_message(self, message: EmailMessage) -> dict[str, tuple[int, bytes]]:
        self.message = message
        return {}


@pytest.fixture(autouse=True)
def _reset_fake_smtp() -> None:
    FakeSMTP.instances.clear()
    yield
    FakeSMTP.instances.clear()


def _configure(monkeypatch: pytest.MonkeyPatch, **overrides: object) -> None:
    values: dict[str, object] = {
        "app_env": "production",
        "smtp_host": "postbox.cloud.yandex.net",
        "smtp_port": 587,
        "smtp_username": "api-key-id",
        "smtp_password": "api-key-secret",
        "smtp_security": "starttls",
        "smtp_from_email": "noreply@fretfight.online",
        "smtp_from_name": "GuitarBattle",
        "smtp_timeout_seconds": 10,
    }
    values.update(overrides)
    for key, value in values.items():
        monkeypatch.setattr(settings, key, value)


def test_invalid_smtp_from_email_is_rejected() -> None:
    with pytest.raises(ValidationError):
        Settings(
            _env_file=None,
            postgres_password="test",
            secret_key="test",
            smtp_from_email="not-an-email",
        )


def test_starttls_login_and_message_headers(monkeypatch: pytest.MonkeyPatch) -> None:
    _configure(monkeypatch)
    monkeypatch.setattr(email_service.smtplib, "SMTP", FakeSMTP)

    email_service.send_email(
        to="player@example.com",
        subject="GuitarBattle: test",
        text="Plain body",
        html_body="<p>HTML body</p>",
    )

    assert len(FakeSMTP.instances) == 1
    smtp = FakeSMTP.instances[0]
    assert (smtp.host, smtp.port) == ("postbox.cloud.yandex.net", 587)
    assert smtp.starttls_called is True
    assert smtp.login_args == ("api-key-id", "api-key-secret")

    message = smtp.message
    assert message is not None
    assert message["From"] == "GuitarBattle <noreply@fretfight.online>"
    assert message["To"] == "player@example.com"
    assert message["Date"]
    assert message["Message-ID"].endswith("@fretfight.online>")
    assert "Plain body" in message.get_body(preferencelist=("plain",)).get_content()
    assert "HTML body" in message.get_body(preferencelist=("html",)).get_content()


def test_smtps_uses_implicit_tls(monkeypatch: pytest.MonkeyPatch) -> None:
    _configure(monkeypatch, smtp_port=465, smtp_security="ssl")
    monkeypatch.setattr(email_service.smtplib, "SMTP_SSL", FakeSMTP)

    email_service.send_email(to="player@example.com", subject="Test", text="Body")

    smtp = FakeSMTP.instances[0]
    assert smtp.port == 465
    assert smtp.context is not None
    assert smtp.starttls_called is False
    assert smtp.login_args == ("api-key-id", "api-key-secret")


def test_missing_password_fails_before_connecting(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _configure(monkeypatch, smtp_password="")
    monkeypatch.setattr(email_service.smtplib, "SMTP", FakeSMTP)

    with pytest.raises(EmailDeliveryError, match="password"):
        email_service.send_email(
            to="player@example.com",
            subject="Test",
            text="Body",
        )

    assert FakeSMTP.instances == []


def test_authentication_error_is_wrapped_without_secret(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class RejectingSMTP(FakeSMTP):
        def login(self, username: str, password: str) -> tuple[int, bytes]:
            raise smtplib.SMTPAuthenticationError(535, b"Authentication failed")

    _configure(monkeypatch)
    monkeypatch.setattr(email_service.smtplib, "SMTP", RejectingSMTP)

    with pytest.raises(EmailDeliveryError) as excinfo:
        email_service.send_email(
            to="player@example.com",
            subject="Test",
            text="Body",
        )

    assert "535" in str(excinfo.value)
    assert "api-key-secret" not in str(excinfo.value)


def test_production_without_smtp_raises(monkeypatch: pytest.MonkeyPatch) -> None:
    _configure(monkeypatch, smtp_host=None)

    with pytest.raises(EmailDeliveryError, match="not configured"):
        email_service.send_email(
            to="player@example.com",
            subject="Test",
            text="Body",
        )


def test_development_without_smtp_logs_message(
    monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
) -> None:
    _configure(monkeypatch, app_env="development", smtp_host=None)

    with caplog.at_level(logging.WARNING, logger=email_service.__name__):
        email_service.send_email(
            to="dev@example.com",
            subject="Development test",
            text="Verification code: 123456",
        )

    assert "123456" in caplog.text


def test_test_environment_uses_in_memory_outbox(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _configure(monkeypatch, app_env="test", smtp_host=None)

    email_service.send_email(
        to="test@example.com",
        subject="Test",
        text="Body",
        html_body="<p>Body</p>",
    )

    outbox = email_service.get_test_outbox()
    assert len(outbox) == 1
    assert outbox[0].to == "test@example.com"
    assert outbox[0].subject == "Test"
