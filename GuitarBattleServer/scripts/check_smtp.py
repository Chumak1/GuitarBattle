#!/usr/bin/env python3
"""Check SMTP connectivity and credentials for the GuitarBattle server.

The script reads the same SMTP_* variables as the API (environment first,
then GuitarBattleServer/.env). It never prints SMTP_PASSWORD.

Examples:
  python scripts/check_smtp.py
  python scripts/check_smtp.py --send --to you@example.com

For Yandex Cloud Postbox:
  SMTP_HOST=postbox.cloud.yandex.net
  SMTP_PORT=587
  SMTP_SECURITY=starttls
  SMTP_USERNAME=<API key ID>
  SMTP_PASSWORD=<API key secret>
  SMTP_FROM_EMAIL=noreply@fretfight.online

See docs/YANDEX_POSTBOX.md for the full setup and DNS records.
"""
from __future__ import annotations

import argparse
import smtplib
import ssl
import sys
from email.message import EmailMessage
from email.utils import formataddr, formatdate, make_msgid
from pathlib import Path

SERVER_ROOT = Path(__file__).resolve().parents[1]
if str(SERVER_ROOT) not in sys.path:
    sys.path.insert(0, str(SERVER_ROOT))

from app.config import Settings  # noqa: E402


def _load_settings() -> Settings:
    # The API settings object also requires database credentials. They are
    # irrelevant for an SMTP probe, so provide harmless values when the local
    # .env only contains SMTP variables.
    return Settings(
        _env_file=SERVER_ROOT / ".env",
        postgres_password="not-used-by-check-smtp",
        secret_key="not-used-by-check-smtp",
    )


def _decode(value: object) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)


def _print_response(label: str, code: int, message: object) -> None:
    text = " ".join(_decode(message).split())
    print(f"{label}: {code} {text}".rstrip())


def _make_client(settings: Settings) -> tuple[smtplib.SMTP, ssl.SSLContext]:
    context = ssl.create_default_context()
    if settings.smtp_security == "ssl":
        client: smtplib.SMTP = smtplib.SMTP_SSL(
            settings.smtp_host,
            settings.smtp_port,
            timeout=settings.smtp_timeout_seconds,
            context=context,
        )
    else:
        client = smtplib.SMTP(
            settings.smtp_host,
            settings.smtp_port,
            timeout=settings.smtp_timeout_seconds,
        )
    return client, context


def _build_message(settings: Settings, recipient: str) -> EmailMessage:
    from_email = str(settings.smtp_from_email)
    message = EmailMessage()
    message["Subject"] = "GuitarBattle Postbox test"
    message["From"] = formataddr((settings.smtp_from_name, from_email))
    message["To"] = recipient
    message["Date"] = formatdate(localtime=False)
    message["Message-ID"] = make_msgid(domain=from_email.rsplit("@", 1)[-1])
    message.set_content(
        "This is a test message from the GuitarBattle server SMTP check.\n"
        "If you received it, Postbox accepted the message for delivery.\n"
    )
    return message


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--send",
        action="store_true",
        help="actually send a test message (requires --to)",
    )
    parser.add_argument(
        "--to",
        metavar="ADDRESS",
        help="recipient for the test message",
    )
    parser.add_argument(
        "--allow-insecure",
        action="store_true",
        help="allow SMTP AUTH over an unencrypted connection (local relay only)",
    )
    args = parser.parse_args()

    if args.send and not args.to:
        parser.error("--send requires --to")

    settings = _load_settings()

    print(f"SMTP host: {settings.smtp_host or '<empty>'}")
    print(f"SMTP port: {settings.smtp_port}")
    print(f"SMTP security: {settings.smtp_security}")
    print(f"SMTP from: {settings.smtp_from_email}")
    print(f"SMTP username set: {bool(settings.smtp_username)}")
    print(f"SMTP password set: {bool(settings.smtp_password)}")

    if not settings.smtp_host:
        print("SMTP_HOST is empty; nothing to check.", file=sys.stderr)
        return 2
    if settings.smtp_username and not settings.smtp_password:
        print("SMTP_USERNAME is set but SMTP_PASSWORD is empty.", file=sys.stderr)
        return 2
    if (
        settings.smtp_security == "none"
        and settings.smtp_username
        and not args.allow_insecure
    ):
        print(
            "Refusing to send SMTP credentials over an unencrypted connection. "
            "Use SMTP_SECURITY=starttls or ssl, or pass --allow-insecure for a "
            "local relay.",
            file=sys.stderr,
        )
        return 2

    try:
        client, context = _make_client(settings)
        with client as server:
            _print_response("EHLO", *server.ehlo())
            if settings.smtp_security == "starttls":
                _print_response("STARTTLS", *server.starttls(context=context))
                _print_response("EHLO", *server.ehlo())
            if settings.smtp_username:
                code, message = server.login(
                    settings.smtp_username, settings.smtp_password or ""
                )
                _print_response("AUTH", code, message)
            else:
                print("AUTH: skipped (SMTP_USERNAME is empty)")

            if args.send:
                refused = server.send_message(_build_message(settings, args.to))
                if refused:
                    print(
                        f"SEND: some recipients were refused: {list(refused)}",
                        file=sys.stderr,
                    )
                    return 3
                print("SEND: accepted by the SMTP server")
            else:
                print("No message sent (use --send --to <address> for a real test).")
    except smtplib.SMTPAuthenticationError as exc:
        print(
            f"AUTH failed: {exc.smtp_code} {_decode(exc.smtp_error)!r}",
            file=sys.stderr,
        )
        return 3
    except (OSError, smtplib.SMTPException) as exc:
        print(f"SMTP error: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 3

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
