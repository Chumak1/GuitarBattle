"""Tests for the public feedback endpoint.

The SMTP transport is replaced with an in-memory recorder, so no real mail is
ever sent. Only ``_send_via_smtp`` is mocked: the endpoint, the request schema,
base64 decoding, filename sanitising and MIME construction all run for real.
"""
from __future__ import annotations

import base64
from email.message import EmailMessage

import pytest

from app.config import settings
from app.schemas.feedback import ATTACHMENT_MAX_BYTES, DEFAULT_ATTACHMENT_NAME
from app.services import email_service
from app.services.email_service import EmailDeliveryError

FEEDBACK = "/api/v1/feedback"
RECIPIENT = "chumakevgeny@gmail.com"
FIVE_MIB = 5 * 1024 * 1024


@pytest.fixture()
def smtp(monkeypatch: pytest.MonkeyPatch) -> list[EmailMessage]:
    """Configure a fake, production-like SMTP transport and capture messages."""
    sent: list[EmailMessage] = []
    monkeypatch.setattr(settings, "app_env", "production")
    monkeypatch.setattr(settings, "smtp_host", "postbox.cloud.yandex.net")
    monkeypatch.setattr(settings, "smtp_username", "api-key-id")
    monkeypatch.setattr(settings, "smtp_password", "api-key-secret")
    monkeypatch.setattr(settings, "smtp_from_email", "noreply@fretfight.online")
    monkeypatch.setattr(settings, "feedback_email_to", RECIPIENT)
    monkeypatch.setattr(email_service, "_send_via_smtp", sent.append)
    return sent


def _plain_text(message: EmailMessage) -> str:
    body = message.get_body(preferencelist=("plain",))
    assert body is not None
    return body.get_content()


def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


# Reproduces juce::MemoryBlock::toBase64Encoding(): JUCE's own alphabet (index 0
# is '.', then A-Z, a-z, 0-9, '+'), no '=' padding, "<decimal size>." prefix.
# Clients up to 1.4.1 sent attachments in this layout.
_JUCE_ALPHABET = b".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"


def _legacy_juce_b64(data: bytes) -> str:
    chars: list[str] = []
    accumulator = 0
    bits = 0
    for byte in data:
        accumulator = (accumulator << 8) | byte
        bits += 8
        while bits >= 6:
            bits -= 6
            chars.append(chr(_JUCE_ALPHABET[(accumulator >> bits) & 0x3F]))
    if bits:
        chars.append(chr(_JUCE_ALPHABET[(accumulator << (6 - bits)) & 0x3F]))
    return f"{len(data)}." + "".join(chars)


def test_feedback_sends_message_with_attachment(client, smtp):
    attachment = b"first line\nsecond line\n"
    response = client.post(
        FEEDBACK,
        json={
            "message": "The metronome drifts on 7/8 songs.",
            "username": "player_1",
            "app_version": "1.2.3",
            # An old client may still send the retired email field; it must not
            # become a Reply-To address for an anonymous request.
            "email": "attacker@example.com",
            "attachment_name": "../../logs/guitar.txt",
            "attachment_data": _b64(attachment),
        },
    )

    assert response.status_code == 200, response.text
    assert response.json() == {"ok": True}

    assert len(smtp) == 1
    message = smtp[0]
    assert message["To"] == RECIPIENT
    assert message["Subject"] == "FretFight feedback from player_1"
    assert message["Reply-To"] is None

    body = _plain_text(message)
    assert "The metronome drifts on 7/8 songs." in body
    assert "Username: player_1" in body
    assert "Email: (not provided)" in body
    assert "App version: 1.2.3" in body
    assert "attacker@example.com" not in body

    attachments = list(message.iter_attachments())
    assert len(attachments) == 1
    part = attachments[0]
    # Directory components are stripped, only the bare filename is kept.
    assert part.get_filename() == "guitar.txt"
    assert part.get_content_type() == "text/plain"
    assert part.get_payload(decode=True) == attachment


def test_feedback_attachment_without_name_gets_default_name(client, smtp):
    response = client.post(
        FEEDBACK,
        json={"message": "Screenshot attached", "attachment_data": _b64(b"\x89PNG")},
    )

    assert response.status_code == 200, response.text
    part = list(smtp[0].iter_attachments())[0]
    assert part.get_filename() == DEFAULT_ATTACHMENT_NAME
    assert part.get_content_type() == "application/octet-stream"


def test_feedback_without_attachment_still_works(client, smtp):
    response = client.post(FEEDBACK, json={"message": "Sounds great!"})

    assert response.status_code == 200, response.text
    message = smtp[0]
    assert message["Subject"] == "FretFight feedback"
    assert list(message.iter_attachments()) == []
    body = _plain_text(message)
    assert "Sounds great!" in body
    assert "Username: (not provided)" in body
    assert "Email: (not provided)" in body
    assert "App version: (not provided)" in body


def test_feedback_rejects_invalid_base64(client, smtp):
    for data in ("not base64!!!", "@@@@", "a"):
        response = client.post(
            FEEDBACK,
            json={"message": "broken attachment", "attachment_data": data},
        )
        assert response.status_code == 422, (data, response.text)

    assert smtp == []


def test_feedback_accepts_legacy_juce_base64_attachment(client, smtp):
    """Clients <= 1.4.1 send ``"<size>.<JUCE base64>"``; it must round-trip."""
    attachment = bytes(range(256)) * 4 + b"\x00juce\xff\x00"
    legacy = _legacy_juce_b64(attachment)
    # Sanity: the legacy payload is not plain base64.
    assert legacy.split(".", 1)[1] != _b64(attachment)

    response = client.post(
        FEEDBACK,
        json={
            "message": "Legacy client attachment",
            "app_version": "1.4.1",
            "attachment_name": "legacy.bin",
            "attachment_data": legacy,
        },
    )

    assert response.status_code == 200, response.text
    assert response.json() == {"ok": True}
    part = list(smtp[0].iter_attachments())[0]
    assert part.get_filename() == "legacy.bin"
    assert part.get_payload(decode=True) == attachment


def test_feedback_accepts_legacy_size_prefix_with_plain_base64(client, smtp):
    """A "<size>." prefix on RFC 4648 data is accepted and the size is ignored."""
    # base64 contains '/' in the first blob and '=' padding in the second, which
    # is how a prefixed plain-base64 payload is recognised.
    attachments = (
        b"\xff\xd8\xff\xe0\x00\x10JFIF" * 3,
        b"hi",
    )
    sent = 0
    for attachment in attachments:
        encoded = _b64(attachment)
        assert "/" in encoded or "=" in encoded

        for declared in (str(len(attachment)), "1", "999999"):
            response = client.post(
                FEEDBACK,
                json={
                    "message": "Prefixed plain base64",
                    "attachment_data": f"{declared}.{encoded}",
                },
            )
            assert response.status_code == 200, (declared, response.text)
            part = list(smtp[sent].iter_attachments())[0]
            assert part.get_payload(decode=True) == attachment
            sent += 1


def test_feedback_accepts_plain_base64_attachment(client, smtp):
    """Clients 1.4.2+ send plain RFC 4648 base64 without any prefix."""
    attachment = b"\x89PNG\r\n\x1a\n" + bytes(range(64))
    response = client.post(
        FEEDBACK,
        json={
            "message": "Plain base64",
            "attachment_name": "shot.png",
            "attachment_data": _b64(attachment),
        },
    )

    assert response.status_code == 200, response.text
    part = list(smtp[0].iter_attachments())[0]
    assert part.get_payload(decode=True) == attachment


def test_feedback_rejects_non_numeric_legacy_prefix(client, smtp):
    """Only a decimal size prefix is JUCE; anything else must stay a 422."""
    for data in (
        f"abc.{_b64(b'payload')}",
        f"1a2.{_b64(b'payload')}",
        f"-10.{_b64(b'payload')}",
        f" {_b64(b'payload')}.1",
    ):
        response = client.post(
            FEEDBACK,
            json={"message": "bad prefix", "attachment_data": data},
        )
        assert response.status_code == 422, (data, response.text)
        assert response.json() == {"detail": "attachment_data must be valid base64"}
        # The error body must not echo the payload.
        assert "payload" not in response.text

    assert smtp == []


def test_feedback_rejects_legacy_payload_with_garbage(client, smtp):
    response = client.post(
        FEEDBACK,
        json={"message": "garbage", "attachment_data": "1000.not base64!!!"},
    )

    assert response.status_code == 422, response.text
    assert response.json() == {"detail": "attachment_data must be valid base64"}
    assert smtp == []


def test_feedback_rejects_legacy_attachment_over_five_megabytes(client, smtp):
    """The 5 MiB cap applies after decoding, legacy prefix or not."""
    size = ATTACHMENT_MAX_BYTES + 1
    # All-zero bytes encode to all '.' characters in JUCE's alphabet, so the
    # payload can be built without a slow per-byte loop.
    legacy = f"{size}.{'.' * ((size * 8 + 5) // 6)}"
    response = client.post(
        FEEDBACK,
        json={"message": "Too big", "attachment_data": legacy},
    )

    assert response.status_code == 422, response.text
    assert response.json() == {
        "detail": "attachment_data must decode to at most 5 MB "
        f"({ATTACHMENT_MAX_BYTES} bytes)"
    }
    assert smtp == []


def test_feedback_rejects_attachment_over_five_megabytes(client, smtp):
    """Decoded size limit is 5 MiB (5 * 1024 * 1024 bytes); over it is 422."""
    oversized = b"\0" * (ATTACHMENT_MAX_BYTES + 1)
    response = client.post(
        FEEDBACK,
        json={
            "message": "Too big",
            "attachment_name": "huge.bin",
            "attachment_data": _b64(oversized),
        },
    )

    assert response.status_code == 422, response.text
    assert "5 MB" in response.text
    assert smtp == []


def test_feedback_attachment_at_exactly_five_megabytes_is_accepted(client, smtp):
    exact = b"\0" * FIVE_MIB
    response = client.post(
        FEEDBACK,
        json={
            "message": "Exactly at the limit",
            "attachment_name": "limit.bin",
            "attachment_data": _b64(exact),
        },
    )

    assert response.status_code == 200, response.text
    part = list(smtp[0].iter_attachments())[0]
    assert part.get_payload(decode=True) == exact


def test_feedback_message_is_required_and_bounded(client, smtp):
    for payload in (
        {"message": ""},
        {"message": "   \n\t "},
        {"message": "x" * 4001},
        {},
    ):
        response = client.post(FEEDBACK, json=payload)
        assert response.status_code == 422, (payload, response.text)

    assert smtp == []


def test_feedback_authenticated_account_email_is_reply_to(db_client, monkeypatch):
    """A valid bearer token supplies Reply-To; the request cannot override it."""
    from tests import helpers

    user = helpers.register_user(db_client)

    sent: list[EmailMessage] = []
    monkeypatch.setattr(settings, "app_env", "production")
    monkeypatch.setattr(settings, "smtp_host", "postbox.cloud.yandex.net")
    monkeypatch.setattr(settings, "smtp_username", "api-key-id")
    monkeypatch.setattr(settings, "smtp_password", "api-key-secret")
    monkeypatch.setattr(settings, "smtp_from_email", "noreply@fretfight.online")
    monkeypatch.setattr(settings, "feedback_email_to", RECIPIENT)
    monkeypatch.setattr(email_service, "_send_via_smtp", sent.append)

    response = db_client.post(
        FEEDBACK,
        json={"message": "Signed-in feedback"},
        headers=user["headers"],
    )

    assert response.status_code == 200, response.text
    message = sent[0]
    assert message["Reply-To"] == user["email"]
    # The username is filled in from the account when the client omits it.
    assert message["Subject"] == f"FretFight feedback from {user['username']}"
    assert f"Email: {user['email']}" in _plain_text(message)


def test_feedback_treats_broken_token_as_anonymous(db_client, smtp):
    """A stale token must not turn the public feedback form into a 401."""
    response = db_client.post(
        FEEDBACK,
        json={"message": "My session expired but the form still works."},
        headers={"Authorization": "Bearer not-a-real-jwt"},
    )

    assert response.status_code == 200, response.text
    assert response.json() == {"ok": True}
    assert len(smtp) == 1
    assert smtp[0]["Reply-To"] is None


def test_feedback_without_smtp_reports_unconfigured(client, monkeypatch):
    sent: list[EmailMessage] = []
    monkeypatch.setattr(settings, "app_env", "production")
    monkeypatch.setattr(settings, "smtp_host", None)
    monkeypatch.setattr(email_service, "_send_via_smtp", sent.append)

    response = client.post(
        FEEDBACK,
        json={"message": "Anybody there?", "attachment_data": _b64(b"data")},
    )

    assert response.status_code == 503
    assert response.json() == {"ok": False, "detail": "mail is not configured"}
    assert sent == []


def test_feedback_delivery_failure_is_reported_without_body(
    client, smtp, monkeypatch
):
    def fail(_message: EmailMessage) -> None:
        raise EmailDeliveryError("SMTP delivery failed")

    monkeypatch.setattr(email_service, "_send_via_smtp", fail)

    response = client.post(
        FEEDBACK,
        json={
            "message": "secret feedback body",
            "username": "player_1",
            "attachment_name": "secret.bin",
            "attachment_data": _b64(b"secret bytes"),
        },
    )

    assert response.status_code == 503
    assert response.json() == {"ok": False, "detail": "mail delivery failed"}
