"""Password hashing shared by account auth and duel lobby passwords.

Argon2 (``pwdlib`` recommended settings) is self-describing: a hash produced by
this module verifies with the hasher configured in ``app.api.v1.auth`` and vice
versa, so the two modules stay independent without duplicating secrets.
"""
from __future__ import annotations

from pwdlib import PasswordHash

_hasher = PasswordHash.recommended()


def hash_password(plaintext: str) -> str:
    return _hasher.hash(plaintext)


def verify_password(plaintext: str, stored_hash: str | None) -> bool:
    """Constant-time-ish check that never raises on a malformed hash."""
    if not stored_hash:
        return False
    try:
        return bool(_hasher.verify(plaintext, stored_hash))
    except Exception:  # pragma: no cover - defensive against corrupt rows
        return False
