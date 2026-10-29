"""Contacts block of the public pages: defaults, validation and normalisation.

The values live in ``landing_settings.content["contacts"]`` (single row id=1),
next to the landing ``blocks`` and ``hero_background``. Adding the key needs no
migration, so the public endpoint and the admin editor share this module: both
read the same defaults and both write the same normalised shape.

Stored values are always produced by :func:`normalize_contacts_update`, but the
reader stays lenient: a hand-edited document or an old row must never break the
public page. Per-field fallback is applied only when the value is missing or
invalid, so an intentionally cleared field (empty string) stays empty.
"""
from __future__ import annotations

import re
from typing import Any, Callable, TypeVar
from urllib.parse import urlsplit, urlunsplit

from pydantic import EmailStr, TypeAdapter, ValidationError

from app.schemas.site import (
    SiteContacts,
    SiteContactsName,
    SiteContactsUpdate,
    SiteSocials,
)

# Shown when the CMS has no value yet, so the pages never look abandoned.
DEFAULT_CONTACT_NAME_RU = "Чумак Евгений Александрович"
DEFAULT_CONTACT_NAME_EN = "Evgeniy Aleksandrovich Chumak"
DEFAULT_CONTACT_EMAIL = "chumakevgeny@gmail.com"
DEFAULT_CONTACT_TELEGRAM = "@chumakevgene"

# Fixed order of the icons on every public page.
SOCIAL_KEYS: tuple[str, ...] = ("youtube", "instagram", "twitter", "vk")

# Hostnames each network may link to. Anything else is rejected, so a typo or
# a malicious paste can never turn the block into an open redirect.
SOCIAL_HOSTS: dict[str, tuple[str, ...]] = {
    "youtube": ("youtube.com", "www.youtube.com", "m.youtube.com", "youtu.be"),
    "instagram": ("instagram.com", "www.instagram.com"),
    "twitter": ("twitter.com", "www.twitter.com", "x.com", "www.x.com"),
    "vk": ("vk.com", "www.vk.com", "m.vk.com"),
}

TELEGRAM_HANDLE_RE = re.compile(r"^[A-Za-z0-9_]{4,32}$")
# ``@handle`` is stripped separately; here we only unwrap links.
TELEGRAM_URL_RE = re.compile(
    r"^(?:https?://)?(?:www\.)?t\.me/([A-Za-z0-9_]+)/?$", re.IGNORECASE
)

_EMAIL_ADAPTER = TypeAdapter(EmailStr)

T = TypeVar("T")


class ContactsValidationError(ValueError):
    """A single field failed validation; ``str()`` names the field."""

    def __init__(self, field: str, message: str) -> None:
        self.field = field
        self.message = message
        super().__init__(f"{field}: {message}")


def _has_forbidden_characters(text: str) -> bool:
    return "<" in text or ">" in text or any(
        ord(char) < 32 or ord(char) == 127 for char in text
    )


def normalize_name(field: str, value: str) -> str:
    """Trim a display name. Empty is allowed: the page falls back to i18n."""
    text = (value or "").strip()
    if len(text) > 120:
        raise ContactsValidationError(field, "must be at most 120 characters")
    if _has_forbidden_characters(text):
        raise ContactsValidationError(
            field, "must not contain control characters or angle brackets"
        )
    return text


def normalize_email(value: str) -> str:
    """Require a real address and lower-case the domain part."""
    text = (value or "").strip()
    if not text:
        raise ContactsValidationError("email", "is required")
    if len(text) > 254:
        raise ContactsValidationError("email", "must be at most 254 characters")
    try:
        parsed = str(_EMAIL_ADAPTER.validate_python(text))
    except ValidationError as exc:
        raise ContactsValidationError("email", "must be a valid email address") from exc
    local, _, domain = parsed.rpartition("@")
    return f"{local}@{domain.lower()}"


def normalize_telegram(value: str) -> str:
    """Accept ``@handle``, ``handle``, ``t.me/handle`` and full t.me links.

    Returns ``"@handle"`` or ``""`` (the row is then hidden on the page).
    """
    text = (value or "").strip()
    if not text:
        return ""
    match = TELEGRAM_URL_RE.match(text)
    handle = match.group(1) if match is not None else text.lstrip("@")
    if not TELEGRAM_HANDLE_RE.match(handle):
        raise ContactsValidationError(
            "telegram",
            "must be @handle, handle or https://t.me/handle "
            "(4-32 letters, digits or underscores)",
        )
    return "@" + handle


def normalize_social(network: str, value: str | None) -> str | None:
    """Return an absolute URL on the network's own domain, or ``None``."""
    if value is None:
        return None
    text = value.strip()
    if not text:
        return None
    if len(text) > 2048:
        raise ContactsValidationError(network, "must be at most 2048 characters")

    try:
        parts = urlsplit(text)
    except ValueError as exc:
        raise ContactsValidationError(network, "must be a valid http(s) URL") from exc

    scheme = parts.scheme.lower()
    if scheme not in ("http", "https"):
        # Catches javascript:, data:, relative paths and scheme-less hosts.
        raise ContactsValidationError(network, "must be an absolute http(s) URL")
    if parts.username is not None or parts.password is not None:
        raise ContactsValidationError(network, "must not contain credentials")

    try:
        host = (parts.hostname or "").lower().rstrip(".")
    except ValueError as exc:
        raise ContactsValidationError(network, "must be a valid http(s) URL") from exc
    allowed = SOCIAL_HOSTS[network]
    if host not in allowed:
        raise ContactsValidationError(
            network, "must link to " + ", ".join(allowed)
        )

    return urlunsplit((scheme, host, parts.path, parts.query, parts.fragment))


def normalize_contacts_update(payload: SiteContactsUpdate) -> SiteContactsUpdate:
    """Validate every field and return the canonical document to store."""
    return SiteContactsUpdate(
        name_ru=normalize_name("name_ru", payload.name_ru),
        name_en=normalize_name("name_en", payload.name_en),
        email=normalize_email(payload.email),
        telegram=normalize_telegram(payload.telegram),
        socials=SiteSocials(
            youtube=normalize_social("youtube", payload.socials.youtube),
            instagram=normalize_social("instagram", payload.socials.instagram),
            twitter=normalize_social("twitter", payload.socials.twitter),
            vk=normalize_social("vk", payload.socials.vk),
        ),
    )


def _as_text(value: Any) -> str | None:
    return value if isinstance(value, str) else None


def _lenient(normalizer: Callable[[str], T], raw: Any, default: T) -> T:
    text = _as_text(raw)
    if text is None:
        return default
    try:
        return normalizer(text)
    except ContactsValidationError:
        return default


def contacts_update_from_content(content: dict[str, Any]) -> SiteContactsUpdate:
    """Read ``content["contacts"]``, filling missing or broken fields."""
    raw = content.get("contacts")
    data = raw if isinstance(raw, dict) else {}
    socials_raw = data.get("socials")
    socials = socials_raw if isinstance(socials_raw, dict) else {}

    return SiteContactsUpdate(
        name_ru=_lenient(
            lambda text: normalize_name("name_ru", text),
            data.get("name_ru"),
            DEFAULT_CONTACT_NAME_RU,
        ),
        name_en=_lenient(
            lambda text: normalize_name("name_en", text),
            data.get("name_en"),
            DEFAULT_CONTACT_NAME_EN,
        ),
        email=_lenient(normalize_email, data.get("email"), DEFAULT_CONTACT_EMAIL),
        telegram=_lenient(
            normalize_telegram, data.get("telegram"), DEFAULT_CONTACT_TELEGRAM
        ),
        socials=SiteSocials(
            **{
                network: _lenient(
                    lambda text, network=network: normalize_social(network, text),
                    socials.get(network),
                    None,
                )
                for network in SOCIAL_KEYS
            }
        ),
    )


def contacts_to_content(values: SiteContactsUpdate) -> dict[str, Any]:
    """JSONB payload for ``content["contacts"]`` (fixed keys only)."""
    return {
        "name_ru": values.name_ru,
        "name_en": values.name_en,
        "email": values.email,
        "telegram": values.telegram,
        "socials": {
            network: getattr(values.socials, network) for network in SOCIAL_KEYS
        },
    }


def telegram_url(handle: str) -> str:
    return f"https://t.me/{handle[1:]}" if handle else ""


def public_contacts(values: SiteContactsUpdate) -> SiteContacts:
    """Public projection: no internal CMS keys, fixed social key order."""
    return SiteContacts(
        name=SiteContactsName(ru=values.name_ru, en=values.name_en),
        email=values.email,
        telegram=values.telegram,
        telegram_url=telegram_url(values.telegram),
        socials={
            network: getattr(values.socials, network) for network in SOCIAL_KEYS
        },
    )
