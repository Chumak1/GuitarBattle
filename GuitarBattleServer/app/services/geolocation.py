"""Country lookup for a public IP address, without an API key.

The provider is a free HTTPS service (``https://ipwho.is`` by default). The
lookup is strictly best effort: registration and sign-in must never wait for
it or fail because of it, so every transport error, timeout and unexpected
payload collapses to ``None``.

``apply_country`` is meant to run as a FastAPI ``BackgroundTasks`` job: it
opens its own ``SessionLocal`` (never the request-scoped session) and Starlette
runs a synchronous background task in a worker thread, so the blocking
``urllib`` call cannot stall the event loop.

Successful answers are cached in-process for a day to keep the provider's free
quota for real sign-ups. Failures are *not* cached: ``country_checked_at``
stays ``NULL`` and the next sign-in retries. Full IP addresses are never
logged.
"""
from __future__ import annotations

import json
import logging
import threading
import time
import urllib.request
from datetime import datetime, timezone
from typing import Any
from urllib.parse import quote
from uuid import UUID

from sqlalchemy import select

from app.config import settings
from app.database import SessionLocal
from app.models.user import User
from app.services.client_ip import parse_public_ip

logger = logging.getLogger(__name__)

SUCCESS_TTL_SECONDS = 24 * 60 * 60
MAX_CACHE_ENTRIES = 1024
MAX_RESPONSE_BYTES = 64 * 1024
USER_AGENT = "GuitarBattle/1.0 (+https://fretfight.online)"

# ip -> (expires_at_monotonic, (code, name)). Only successful answers are kept.
_cache: dict[str, tuple[float, tuple[str, str | None]]] = {}
_cache_lock = threading.Lock()


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Treat a redirect as an error instead of following it."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D102
        return None


_opener = urllib.request.build_opener(_NoRedirect)


def _fetch_json(url: str) -> Any:
    """One GET request with a hard timeout; raises on any transport error."""
    request = urllib.request.Request(
        url,
        headers={"Accept": "application/json", "User-Agent": USER_AGENT},
    )
    with _opener.open(request, timeout=settings.geoip_timeout_seconds) as response:
        raw = response.read(MAX_RESPONSE_BYTES)
    return json.loads(raw)


def _parse(payload: Any) -> tuple[str, str | None] | None:
    """Read ``success``/``country``/``country_code`` from a provider answer."""
    if not isinstance(payload, dict) or payload.get("success") is not True:
        return None
    code = payload.get("country_code")
    if not isinstance(code, str):
        return None
    code = code.strip().upper()
    if len(code) != 2 or not code.isascii() or not code.isalpha():
        return None
    name = payload.get("country")
    if isinstance(name, str):
        name = name.strip()[:64] or None
    else:
        name = None
    return code, name


def _lookup(ip: str) -> tuple[str, str | None] | None:
    base = (settings.geoip_url or "").strip().rstrip("/")
    if not base.lower().startswith(("http://", "https://")):
        logger.warning("GEOIP_URL is not an absolute http(s) URL; lookup skipped")
        return None
    try:
        payload = _fetch_json(f"{base}/{quote(ip, safe=':')}")
    except Exception:
        # Timeout, DNS failure, non-2xx (including a redirect), broken JSON:
        # all of it just means "unknown country".
        logger.debug("Geo-IP lookup failed")
        return None
    return _parse(payload)


def _cache_get(ip: str) -> tuple[bool, tuple[str, str | None] | None]:
    now = time.monotonic()
    with _cache_lock:
        entry = _cache.get(ip)
        if entry is None:
            return False, None
        expires_at, value = entry
        if expires_at <= now:
            _cache.pop(ip, None)
            return False, None
        return True, value


def _cache_put(ip: str, value: tuple[str, str | None]) -> None:
    now = time.monotonic()
    with _cache_lock:
        if len(_cache) >= MAX_CACHE_ENTRIES:
            for key, (expires_at, _value) in list(_cache.items()):
                if expires_at <= now:
                    _cache.pop(key, None)
            if len(_cache) >= MAX_CACHE_ENTRIES:
                # Bounded memory matters more than a warm cache.
                _cache.clear()
        _cache[ip] = (now + SUCCESS_TTL_SECONDS, value)


def clear_cache() -> None:
    """Drop every cached answer (tests, and after a provider change)."""
    with _cache_lock:
        _cache.clear()


def resolve_country(ip: str | None) -> tuple[str, str | None] | None:
    """Return ``(ISO 3166-1 alpha-2, English name)`` for a public IP, else None."""
    if not settings.geoip_enabled:
        return None
    normalized = parse_public_ip(ip)
    if normalized is None:
        return None
    found, cached = _cache_get(normalized)
    if found:
        return cached
    result = _lookup(normalized)
    if result is not None:
        _cache_put(normalized, result)
    return result


def country_flag(code: str | None) -> str | None:
    """Regional-indicator emoji (``DE`` -> 🇩🇪) for the admin table."""
    if not code or not isinstance(code, str):
        return None
    normalized = code.strip().upper()
    if len(normalized) != 2 or not normalized.isascii() or not normalized.isalpha():
        return None
    return "".join(chr(0x1F1E6 + ord(char) - ord("A")) for char in normalized)


def _country_missing(user_id: UUID) -> bool:
    """Whether the account still has no country; a read failure means "no"."""
    db = SessionLocal()
    try:
        return db.scalar(select(User.country_code).where(User.id == user_id)) is None
    except Exception:
        logger.warning("Could not read the stored country for user %s", user_id)
        return False
    finally:
        db.close()


def apply_country(
    user_id: UUID, ip: str | None, *, only_if_missing: bool = True
) -> bool:
    """Resolve the country of ``ip`` and store it on the account.

    ``only_if_missing`` is used on sign-in: an account that already has a
    country is never looked up again, so the provider's free quota is spent
    only on new or still-unknown players. A failed lookup stores nothing and
    leaves ``country_checked_at`` NULL, which makes the next sign-in retry.

    Returns ``True`` when the account row was updated. Never raises.
    """
    if not settings.geoip_enabled or not ip:
        return False
    if only_if_missing and not _country_missing(user_id):
        return False

    country = resolve_country(ip)
    if country is None:
        return False
    code, name = country

    db = SessionLocal()
    try:
        user = db.get(User, user_id)
        if user is None:
            return False
        if only_if_missing and user.country_code:
            # Another sign-in resolved it while this lookup was in flight.
            return False
        user.country_code = code
        user.country_name = name
        user.country_checked_at = datetime.now(timezone.utc)
        db.commit()
        return True
    except Exception:
        db.rollback()
        logger.warning("Could not store the country for user %s", user_id)
        return False
    finally:
        db.close()
