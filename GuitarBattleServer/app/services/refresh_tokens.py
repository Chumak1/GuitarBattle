"""Rotating refresh tokens.

Design notes
------------
* Only a sha256 hex digest of the raw token is stored. The raw token exists in
  the HttpOnly cookie (web) or in the login response (desktop client) and is
  never logged.
* Every refresh rotates: the presented row is revoked, ``replaced_by_id`` points
  at the new row, and the new raw token is returned.
* Presenting an already revoked token means either a race between two tabs or a
  stolen token. A short grace window treats the first case as a plain 401;
  later reuse revokes every session in the same rotation chain, so the thief and
  the victim are both logged out.
"""
from __future__ import annotations

import hashlib
import secrets
from datetime import datetime, timedelta, timezone

from sqlalchemy import select, text
from sqlalchemy.orm import Session

from app.config import settings
from app.models.refresh_session import RefreshSession
from app.models.user import User

# token_urlsafe(48) yields 64 characters of URL-safe randomness.
TOKEN_BYTES = 48
USER_AGENT_MAX_LENGTH = 512


def hash_refresh_token(raw_token: str) -> str:
    """Return the value stored in ``refresh_sessions.token_hash``."""
    return hashlib.sha256(raw_token.encode("utf-8")).hexdigest()


def refresh_token_expires_in() -> int:
    return settings.refresh_token_expire_days * 24 * 60 * 60


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _as_utc(value: datetime) -> datetime:
    return value if value.tzinfo is not None else value.replace(tzinfo=timezone.utc)


def _clean_user_agent(user_agent: str | None) -> str | None:
    if not user_agent:
        return None
    cleaned = user_agent.strip()
    return cleaned[:USER_AGENT_MAX_LENGTH] or None


def issue_refresh_session(
    db: Session,
    user: User,
    *,
    user_agent: str | None = None,
    now: datetime | None = None,
) -> tuple[str, RefreshSession]:
    """Create a refresh session and return ``(raw_token, row)``.

    The caller owns the transaction and must commit after the raw token has been
    handed to the client (cookie or JSON body).
    """
    now = now or _now()
    raw_token = secrets.token_urlsafe(TOKEN_BYTES)
    session = RefreshSession(
        user_id=user.id,
        token_hash=hash_refresh_token(raw_token),
        expires_at=now + timedelta(seconds=refresh_token_expires_in()),
        user_agent=_clean_user_agent(user_agent),
    )
    db.add(session)
    db.flush()
    return raw_token, session


def find_refresh_session(
    db: Session, raw_token: str, *, for_update: bool = False
) -> RefreshSession | None:
    statement = (
        select(RefreshSession)
        .where(RefreshSession.token_hash == hash_refresh_token(raw_token))
        .limit(1)
    )
    if for_update:
        statement = statement.with_for_update()
    return db.scalar(statement)


def is_expired(session: RefreshSession, now: datetime | None = None) -> bool:
    return _as_utc(session.expires_at) <= (now or _now())


def rotating_reuse_is_race(
    session: RefreshSession, now: datetime | None = None
) -> bool:
    """True when a revoked token was rotated a moment ago (two tabs racing).

    Only a token that was *replaced* counts: a session revoked by logout is not
    a race and a second use of it is treated as suspicious like any other reuse.
    """
    if session.revoked_at is None or session.replaced_by_id is None:
        return False
    grace = timedelta(seconds=settings.refresh_reuse_grace_seconds)
    return _as_utc(session.revoked_at) + grace > (now or _now())


def revoke_session(
    session: RefreshSession,
    *,
    now: datetime | None = None,
    replaced_by: RefreshSession | None = None,
) -> None:
    """Revoke one session; keep the original revocation time if already set."""
    if session.revoked_at is None:
        session.revoked_at = now or _now()
    if replaced_by is not None and session.replaced_by_id is None:
        session.replaced_by_id = replaced_by.id


def revoke_chain(db: Session, session: RefreshSession, *, now: datetime | None = None) -> None:
    """Revoke the whole rotation chain (older and newer tokens) of a session."""
    db.execute(
        text(
            """
            WITH RECURSIVE forward AS (
                SELECT id, replaced_by_id
                FROM refresh_sessions
                WHERE id = :session_id
                UNION
                SELECT successor.id, successor.replaced_by_id
                FROM refresh_sessions successor
                JOIN forward current ON successor.id = current.replaced_by_id
            ),
            backward AS (
                SELECT id, replaced_by_id
                FROM refresh_sessions
                WHERE id = :session_id
                UNION
                SELECT predecessor.id, predecessor.replaced_by_id
                FROM refresh_sessions predecessor
                JOIN backward current ON predecessor.replaced_by_id = current.id
            )
            UPDATE refresh_sessions
            SET revoked_at = :now
            WHERE revoked_at IS NULL
              AND id IN (SELECT id FROM forward UNION SELECT id FROM backward)
            """
        ),
        {"session_id": session.id, "now": now or _now()},
    )


def revoke_user_sessions(db: Session, user_id: object, *, now: datetime | None = None) -> int:
    """Revoke every active refresh session of a user (password change/reset)."""
    result = db.execute(
        text(
            """
            UPDATE refresh_sessions
            SET revoked_at = :now
            WHERE user_id = :user_id AND revoked_at IS NULL
            """
        ),
        {"user_id": user_id, "now": now or _now()},
    )
    return result.rowcount or 0
