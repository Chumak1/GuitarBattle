"""Issue and verify one-time email codes.

The raw six-digit code only exists long enough to be sent. The database stores
an HMAC bound to the user id and purpose, so a leaked database dump cannot be
used to verify an account without also knowing ``SECRET_KEY``.
"""
from __future__ import annotations

import hashlib
import hmac
import secrets
from datetime import datetime, timedelta, timezone

from sqlalchemy import delete, select
from sqlalchemy.orm import Session

from app.config import settings
from app.models.email_auth_code import EmailAuthCode
from app.models.user import User

PURPOSE_EMAIL_VERIFY = "email_verify"
PURPOSE_PASSWORD_RESET = "password_reset"


def generate_code() -> str:
    return f"{secrets.randbelow(1_000_000):06d}"


def hash_code(user_id: object, purpose: str, code: str) -> str:
    material = f"{purpose}:{user_id}:{code}".encode("utf-8")
    return hmac.new(
        settings.secret_key.encode("utf-8"), material, hashlib.sha256
    ).hexdigest()


def _as_utc(value: datetime) -> datetime:
    return value if value.tzinfo is not None else value.replace(tzinfo=timezone.utc)


def get_active_code(
    db: Session, user_id: object, purpose: str, *, lock: bool = False
) -> EmailAuthCode | None:
    statement = (
        select(EmailAuthCode)
        .where(
            EmailAuthCode.user_id == user_id,
            EmailAuthCode.purpose == purpose,
            EmailAuthCode.consumed_at.is_(None),
        )
        .order_by(EmailAuthCode.created_at.desc())
        .limit(1)
    )
    if lock:
        statement = statement.with_for_update()
    return db.scalar(statement)


def issue_code(
    db: Session,
    user: User,
    purpose: str,
    *,
    force: bool = False,
) -> str | None:
    """Create a new code and return it, or ``None`` during the resend cooldown.

    Any older unconsumed code for the same purpose is invalidated. The caller
    must commit the transaction after the email has been sent.
    """
    now = datetime.now(timezone.utc)
    latest = get_active_code(db, user.id, purpose)
    if latest is not None and not force:
        sent_at = _as_utc(latest.sent_at)
        if sent_at > now - timedelta(seconds=settings.email_code_resend_seconds):
            return None

    db.execute(
        delete(EmailAuthCode).where(
            EmailAuthCode.user_id == user.id,
            EmailAuthCode.purpose == purpose,
            EmailAuthCode.consumed_at.is_(None),
        )
    )

    code = generate_code()
    record = EmailAuthCode(
        user_id=user.id,
        purpose=purpose,
        code_hash=hash_code(user.id, purpose, code),
        expires_at=now + timedelta(minutes=settings.email_code_expire_minutes),
        sent_at=now,
        attempts=0,
    )
    db.add(record)
    db.flush()
    return code


def verify_code(db: Session, user: User, purpose: str, code: str) -> bool:
    """Check a code and mark it consumed on success.

    The caller owns the transaction: commit on both success and failure so the
    attempt counter is persisted before an HTTP error is returned.
    """
    record = get_active_code(db, user.id, purpose, lock=True)
    if record is None:
        return False

    now = datetime.now(timezone.utc)
    if _as_utc(record.expires_at) <= now:
        return False
    if record.attempts >= settings.email_code_max_attempts:
        return False

    record.attempts += 1
    if not hmac.compare_digest(
        record.code_hash, hash_code(user.id, purpose, code)
    ):
        return False

    record.consumed_at = now
    return True


def purge_codes(db: Session, user_id: object, purpose: str) -> None:
    db.execute(
        delete(EmailAuthCode).where(
            EmailAuthCode.user_id == user_id,
            EmailAuthCode.purpose == purpose,
            EmailAuthCode.consumed_at.is_(None),
        )
    )
