"""IP-based country and language detection for the public site.

The database is the free DB-IP Lite country MMDB (same record shape as
MaxMind GeoLite2 country: ``{"country": {"iso_code": "RU"}}``). It is optional:
if the file is missing or unreadable every lookup returns ``None`` instead of
raising, so the landing page keeps working without GeoIP data.
"""
from __future__ import annotations

import ipaddress
import logging
import threading
from pathlib import Path
from typing import Any

import maxminddb

from app.config import settings

logger = logging.getLogger(__name__)

# Countries where the client defaults to Russian. The list is deliberately
# limited to the CIS plus the post-Soviet neighbours with a large Russian-
# speaking audience; everything else gets English.
CIS_COUNTRIES = frozenset(
    {"RU", "BY", "KZ", "KG", "UZ", "TJ", "TM", "AZ", "AM", "MD"}
)
DEFAULT_LANGUAGE = "en"

_lock = threading.Lock()
_reader: Any | None = None
_reader_key: tuple[str, int, int] | None = None
_failed_key: tuple[str, int, int] | None = None


def _signature(path: Path) -> tuple[str, int, int] | None:
    try:
        stat = path.stat()
    except OSError:
        return None
    if not path.is_file():
        return None
    return (str(path), stat.st_mtime_ns, stat.st_size)


def _close(reader: Any | None) -> None:
    if reader is None:
        return
    try:
        reader.close()
    except Exception:  # pragma: no cover - defensive
        pass


def _get_reader() -> Any | None:
    """Return a cached MMDB reader, reopening it when the file changes."""
    global _reader, _reader_key, _failed_key

    path = Path(settings.geoip_db_path)
    key = _signature(path)
    if key is None:
        # Missing database: nothing cached, callers get None.
        return None

    with _lock:
        if _reader is not None and _reader_key == key:
            return _reader
        if _failed_key == key:
            return None
        if _reader is not None:
            _close(_reader)
            _reader = None
            _reader_key = None
        try:
            reader = maxminddb.open_database(str(path))
        except Exception:
            # Never let a broken/partial download take the API down.
            _failed_key = key
            logger.warning("GeoIP database could not be opened at %s", path)
            return None
        _reader = reader
        _reader_key = key
        _failed_key = None
        return reader


IPAddress = ipaddress.IPv4Address | ipaddress.IPv6Address


def _parse_ip(value: str | None) -> IPAddress | None:
    if not value:
        return None
    cleaned = value.strip()
    if not cleaned:
        return None
    # Strip an optional port ("1.2.3.4:5678", "[::1]:443") and brackets.
    if cleaned.startswith("["):
        end = cleaned.find("]")
        if end != -1:
            cleaned = cleaned[1:end]
    elif cleaned.count(":") == 1 and "." in cleaned:
        cleaned = cleaned.split(":", 1)[0]
    try:
        return ipaddress.ip_address(cleaned)
    except ValueError:
        return None


def _is_lookup_candidate(address: IPAddress) -> bool:
    return not (
        address.is_private
        or address.is_loopback
        or address.is_link_local
        or address.is_multicast
        or address.is_reserved
        or address.is_unspecified
        or not address.is_global
    )


def country_for_ip(ip: str | None) -> str | None:
    """Return an ISO-3166-1 alpha-2 country code for a public IP, else None."""
    address = _parse_ip(ip)
    if address is None or not _is_lookup_candidate(address):
        return None

    reader = _get_reader()
    if reader is None:
        return None
    try:
        record = reader.get(str(address))
    except Exception:
        return None
    if not isinstance(record, dict):
        return None

    for key in ("country", "registered_country"):
        country = record.get(key)
        if isinstance(country, dict):
            code = country.get("iso_code")
            if isinstance(code, str) and code.strip():
                return code.strip().upper()
    return None


def language_for_country(code: str | None) -> str | None:
    """Map a country code to the landing page language, None when unknown."""
    if not code or not isinstance(code, str):
        return None
    normalized = code.strip().upper()
    if not normalized:
        return None
    return "ru" if normalized in CIS_COUNTRIES else DEFAULT_LANGUAGE
