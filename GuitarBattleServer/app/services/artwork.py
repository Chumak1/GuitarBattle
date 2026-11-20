"""Artwork slots (HOME hero backgrounds, default avatar) and player avatars.

Shared rules:

* the file type is decided by the signature, never by the client
  ``content_type``: only PNG, JPEG and WebP are accepted;
* the size limit is enforced while streaming, before anything is written;
* the stored name is a fresh uuid, so a replaced image always gets a new URL and
  clients that cache by URL pick it up;
* the previous file is removed only after the new URL is committed.

The metadata of the three administrator slots lives in ``artwork_assets``
(one row per slot); per-player avatars are a column on ``users``.
"""
from __future__ import annotations

from pathlib import Path
from typing import Any, Callable

from sqlalchemy import select
from sqlalchemy.orm import Session

from app.config import settings
from app.models.artwork import ArtworkAsset
from app.schemas.artwork import ArtworkSlotItem, SiteConfigResponse

#: The three administrator-managed slots, in the order the panel shows them.
HERO_BATTLE = "hero-battle"
HERO_DUEL = "hero-duel"
DEFAULT_AVATAR = "default-avatar"
ARTWORK_SLOTS: tuple[str, ...] = (HERO_BATTLE, HERO_DUEL, DEFAULT_AVATAR)

#: Origin-relative URL prefixes. ``/media/artwork`` is mounted by ``app.main``
#: next to ``/media/campaigns``; per-player avatars use their own directory.
ARTWORK_URL_PREFIX = "/media/artwork/"
AVATAR_URL_PREFIX = "/media/avatars/"

#: Formats accepted by every image upload route of this module.
IMAGE_SUFFIXES = frozenset({".png", ".jpg", ".jpeg", ".webp"})

#: extension -> signature check, the same approach as ``IMAGE_SIGNATURES`` in
#: ``app.api.v1.admin_site``. It is kept local because ``site`` -> ``artwork``
#: -> ``admin_site`` -> ``site`` would be an import cycle.
_IMAGE_SIGNATURES: dict[str, Callable[[bytes], bool]] = {
    ".png": lambda head: head.startswith(b"\x89PNG\r\n\x1a\n"),
    ".jpg": lambda head: head.startswith(b"\xff\xd8\xff"),
    ".jpeg": lambda head: head.startswith(b"\xff\xd8\xff"),
    ".webp": lambda head: head[:4] == b"RIFF" and head[8:12] == b"WEBP",
}

#: Human readable slot names used in error details.
SLOT_LABELS = {
    HERO_BATTLE: "Battle background",
    HERO_DUEL: "Duel background",
    DEFAULT_AVATAR: "Default avatar",
}


class ImageUploadError(Exception):
    """A rejected upload; the API layer turns it into an HTTP error."""

    def __init__(self, status_code: int, detail: str) -> None:
        super().__init__(detail)
        self.status_code = status_code
        self.detail = detail


def validate_extension(file_name: str | None, *, label: str) -> tuple[str, Any]:
    """Return ``(suffix, signature_check)`` or raise a 415 ``ImageUploadError``."""
    suffix = Path(file_name or "").suffix.lower()
    matches_signature = _IMAGE_SIGNATURES.get(suffix) if suffix in IMAGE_SUFFIXES else None
    if matches_signature is None:
        raise ImageUploadError(415, f"{label} must be PNG, JPEG or WebP")
    return suffix, matches_signature


def artwork_limit(slot: str) -> int:
    """Hero backgrounds are large; the default avatar follows the avatar limit."""
    if slot == DEFAULT_AVATAR:
        return int(settings.max_avatar_bytes)
    return int(settings.max_artwork_bytes)


def slot_storage_dir(slot: str) -> Path:
    """Directory of one admin slot (all three live under ``media/artwork``)."""
    del slot  # one directory for every administrator slot
    return settings.artwork_media_dir


def url_prefix_for_slot(slot: str) -> str:
    del slot
    return ARTWORK_URL_PREFIX


def asset_path(url: str | None, directory: Path, prefix: str) -> Path | None:
    """Map ``/media/<...>/<name>`` to a file inside @p directory.

    The stored value is never trusted as a filesystem path: anything outside the
    prefix, a nested name or an escaping path maps to ``None``.
    """
    if not url or not url.startswith(prefix):
        return None
    name = url[len(prefix):]
    if not name or "/" in name or "\\" in name or name in {".", ".."}:
        return None
    storage_root = directory.resolve()
    target = (storage_root / name).resolve()
    if not target.is_relative_to(storage_root):
        return None
    return target


def artwork_path(url: str | None) -> Path | None:
    return asset_path(url, settings.artwork_media_dir, ARTWORK_URL_PREFIX)


def avatar_path(url: str | None) -> Path | None:
    return asset_path(url, settings.avatar_media_dir, AVATAR_URL_PREFIX)


def slot_assets(db: Session) -> dict[str, ArtworkAsset]:
    rows = db.scalars(select(ArtworkAsset)).all()
    return {row.slot: row for row in rows}


def serialize_slot(slot: str, asset: ArtworkAsset | None) -> ArtworkSlotItem:
    if asset is None:
        return ArtworkSlotItem(slot=slot)
    return ArtworkSlotItem(
        slot=slot, url=asset.url, updated_at=asset.updated_at
    )


def public_site_config(db: Session) -> SiteConfigResponse:
    """The three public artwork URLs (``null`` when a slot is empty)."""
    assets = slot_assets(db)
    return SiteConfigResponse(
        hero_battle_url=assets[HERO_BATTLE].url if HERO_BATTLE in assets else None,
        hero_duel_url=assets[HERO_DUEL].url if HERO_DUEL in assets else None,
        default_avatar_url=(
            assets[DEFAULT_AVATAR].url if DEFAULT_AVATAR in assets else None
        ),
    )
