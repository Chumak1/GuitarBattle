"""Transactional email delivery for account verification and password recovery.

Production uses SMTP (Yandex Cloud Postbox in the reference deployment). In
development (when SMTP is not configured) the message is written to the
application log so the flow can be tested locally. Test runs keep messages in
an in-memory outbox that the pytest helpers can inspect.

Codes are never logged in production: an unconfigured production deployment
raises ``EmailDeliveryError`` instead of silently creating accounts that cannot
be verified.
"""
from __future__ import annotations

import html
import logging
import smtplib
import ssl
from dataclasses import dataclass
from email.message import EmailMessage
from email.utils import formataddr, formatdate, make_msgid

from app.config import settings

logger = logging.getLogger(__name__)


class EmailDeliveryError(RuntimeError):
    """Raised when an email could not be handed to the SMTP server."""


@dataclass(frozen=True)
class SentEmail:
    to: str
    subject: str
    text: str
    html: str | None


_TEST_OUTBOX: list[SentEmail] = []


def get_test_outbox() -> tuple[SentEmail, ...]:
    """Return messages captured during the current test run."""
    return tuple(_TEST_OUTBOX)


def clear_test_outbox() -> None:
    """Reset the test-only outbox between tests."""
    _TEST_OUTBOX.clear()


def _send_via_smtp(message: EmailMessage) -> None:
    host = settings.smtp_host
    if not host:
        raise EmailDeliveryError("SMTP is not configured")
    if settings.smtp_username and not settings.smtp_password:
        # Postbox always requires SMTP AUTH. Fail before connecting so a
        # half-configured deployment cannot accidentally send unauthenticated.
        raise EmailDeliveryError("SMTP password is not configured")

    context = ssl.create_default_context()

    try:
        if settings.smtp_security == "ssl":
            client: smtplib.SMTP = smtplib.SMTP_SSL(
                host,
                settings.smtp_port,
                timeout=settings.smtp_timeout_seconds,
                context=context,
            )
        else:
            client = smtplib.SMTP(
                host,
                settings.smtp_port,
                timeout=settings.smtp_timeout_seconds,
            )

        with client as server:
            server.ehlo()
            if settings.smtp_security == "starttls":
                server.starttls(context=context)
                server.ehlo()
            if settings.smtp_username:
                server.login(settings.smtp_username, settings.smtp_password or "")
            server.send_message(message)
    except smtplib.SMTPAuthenticationError as exc:
        # Keep the numeric code for diagnostics; never include credentials.
        raise EmailDeliveryError(
            f"SMTP authentication failed ({exc.smtp_code})"
        ) from exc
    except (OSError, smtplib.SMTPException) as exc:
        raise EmailDeliveryError("SMTP delivery failed") from exc


def send_email(*, to: str, subject: str, text: str, html_body: str | None = None) -> None:
    if settings.app_env == "test":
        _TEST_OUTBOX.append(SentEmail(to=to, subject=subject, text=text, html=html_body))
        return

    if not settings.smtp_host:
        if settings.app_env == "development":
            # Local development fallback. Never enable this for production:
            # production is rejected above so a code can never leak into logs.
            logger.warning(
                "SMTP is not configured; development email to %s\nSubject: %s\n%s",
                to,
                subject,
                text,
            )
            return
        raise EmailDeliveryError("SMTP is not configured")

    from_email = str(settings.smtp_from_email)
    message = EmailMessage()
    message["Subject"] = subject
    message["From"] = formataddr((settings.smtp_from_name, from_email))
    message["To"] = to
    message["Date"] = formatdate(localtime=False)
    message["Message-ID"] = make_msgid(domain=from_email.rsplit("@", 1)[-1])
    message.set_content(text)
    if html_body:
        message.add_alternative(html_body, subtype="html")

    _send_via_smtp(message)


def _code_email_html(*, title: str, greeting: str, code: str, expiry_minutes: int,
                     explanation: str) -> str:
    safe_title = html.escape(title)
    safe_greeting = html.escape(greeting)
    safe_code = html.escape(code)
    safe_explanation = html.escape(explanation)
    return f"""\
<!doctype html>
<html lang="ru">
  <body style="margin:0;padding:0;background:#070b10;color:#e9eff6;font-family:Segoe UI,Arial,sans-serif;">
    <table role="presentation" width="100%" cellspacing="0" cellpadding="0" style="background:#070b10;padding:32px 16px;">
      <tr>
        <td align="center">
          <table role="presentation" width="520" cellspacing="0" cellpadding="0" style="max-width:520px;background:#101821;border:1px solid #24313d;border-radius:18px;padding:32px;">
            <tr>
              <td style="font-size:11px;letter-spacing:2px;text-transform:uppercase;color:#f3c451;font-weight:700;">GuitarBattle</td>
            </tr>
            <tr>
              <td style="padding-top:18px;font-size:26px;line-height:1.2;font-weight:700;color:#e9eff6;">{safe_title}</td>
            </tr>
            <tr>
              <td style="padding-top:12px;font-size:15px;line-height:1.6;color:#8d9cab;">{safe_greeting}</td>
            </tr>
            <tr>
              <td align="center" style="padding:26px 0;">
                <div style="display:inline-block;background:#141e29;border:1px solid #f3c451;border-radius:14px;padding:16px 28px;font-size:34px;letter-spacing:8px;font-weight:700;color:#f3c451;">{safe_code}</div>
              </td>
            </tr>
            <tr>
              <td style="font-size:14px;line-height:1.6;color:#e9eff6;">{safe_explanation}</td>
            </tr>
            <tr>
              <td style="padding-top:18px;font-size:13px;line-height:1.5;color:#8d9cab;">Код действует {expiry_minutes} минут. Если вы не запрашивали это действие, просто проигнорируйте письмо.</td>
            </tr>
          </table>
        </td>
      </tr>
    </table>
  </body>
</html>
"""


def send_verification_email(*, to: str, username: str, code: str) -> None:
    minutes = settings.email_code_expire_minutes
    subject = "GuitarBattle: подтверждение почты"
    text = (
        f"Здравствуйте, {username}!\n\n"
        f"Код подтверждения GuitarBattle: {code}\n\n"
        f"Введите его в приложении. Код действует {minutes} минут.\n"
        "Если вы не регистрировались в GuitarBattle, проигнорируйте это письмо.\n"
    )
    html_body = _code_email_html(
        title="Подтвердите почту",
        greeting=f"Здравствуйте, {username}!",
        code=code,
        expiry_minutes=minutes,
        explanation="Введите этот код в приложении, чтобы завершить регистрацию и включить онлайн-результаты.",
    )
    send_email(to=to, subject=subject, text=text, html_body=html_body)


def send_password_reset_email(*, to: str, username: str, code: str) -> None:
    minutes = settings.email_code_expire_minutes
    subject = "GuitarBattle: восстановление пароля"
    text = (
        f"Здравствуйте, {username}!\n\n"
        f"Код для восстановления пароля GuitarBattle: {code}\n\n"
        f"Введите его в приложении и задайте новый пароль. Код действует {minutes} минут.\n"
        "Если вы не запрашивали восстановление, просто проигнорируйте это письмо.\n"
    )
    html_body = _code_email_html(
        title="Восстановление пароля",
        greeting=f"Здравствуйте, {username}!",
        code=code,
        expiry_minutes=minutes,
        explanation="Введите этот код в приложении и задайте новый пароль. Старые сеансы будут завершены.",
    )
    send_email(to=to, subject=subject, text=text, html_body=html_body)
