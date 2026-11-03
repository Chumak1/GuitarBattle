"""Best-effort client IP extraction for the account audit columns.

The API listens on the loopback interface and is reachable only through Caddy,
so the real peer arrives in ``X-Forwarded-For`` (the same header the language
cookie and ``GET /geo`` already read). Caddy appends the immediate peer, so the
first entry is the original client.

Only a syntactically valid, globally routable address is returned. Private,
loopback, link-local, unspecified and multicast addresses say nothing about a
country and are dropped instead of being written to the database.

Nothing here raises: a malformed header or a missing socket peer returns
``None`` and the caller simply stores no IP.
"""
from __future__ import annotations

import ipaddress

from fastapi import Request

FORWARDED_FOR_HEADER = "x-forwarded-for"


def _is_port_suffix(value: str) -> bool:
    """``":5678"`` is a port; ``":port"`` or a bare tail is not."""
    if not value.startswith(":"):
        return False
    digits = value[1:].strip()
    return digits.isdigit() and 0 < int(digits) <= 65535


def _strip_port(value: str) -> str:
    """Drop an optional port: ``1.2.3.4:5678``, ``[::1]:5678``, ``[::1]``.

    Anything after the host that is not a numeric port makes the whole value
    unusable, so the caller gets ``""`` instead of a silently truncated host.
    """
    cleaned = value.strip()
    if not cleaned:
        return ""
    if cleaned.startswith("["):
        end = cleaned.find("]")
        if end == -1:
            return ""
        host = cleaned[1:end].strip()
        remainder = cleaned[end + 1:].strip()
        if remainder and not _is_port_suffix(remainder):
            return ""
        return host
    # A single colon together with a dot is ``ipv4:port``; two or more colons
    # are an unbracketed IPv6 address, which must stay untouched.
    if cleaned.count(":") == 1 and "." in cleaned:
        host, _, port = cleaned.partition(":")
        return host.strip() if _is_port_suffix(":" + port) else ""
    return cleaned


def parse_public_ip(value: str | None) -> str | None:
    """Normalise ``value`` to a public IP string, or ``None``.

    Returns the compressed textual form (``2a00:1450:4001:82b::200e``) so the
    same address always maps to one cache key and one ``INET`` value.
    """
    if not value or not isinstance(value, str):
        return None
    candidate = _strip_port(value)
    if not candidate:
        return None
    try:
        address = ipaddress.ip_address(candidate)
    except ValueError:
        return None
    # ``is_global`` is True for multicast on Python 3.10, so test it explicitly.
    if address.is_multicast or not address.is_global:
        return None
    return str(address)


def client_ip(request: Request) -> str | None:
    """The public IP of the caller, or ``None`` when it cannot be trusted.

    ``X-Forwarded-For`` wins because it is the only header that carries the
    original peer behind Caddy; without it the socket peer is used, which is
    the client itself on a direct (development) connection.
    """
    forwarded = request.headers.get(FORWARDED_FOR_HEADER)
    if forwarded:
        # First entry only: everything after the comma was appended by a proxy.
        return parse_public_ip(forwarded.split(",", 1)[0])
    if request.client is None:
        return None
    return parse_public_ip(request.client.host)
