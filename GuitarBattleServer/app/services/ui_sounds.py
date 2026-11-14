"""Storage, validation, manifest and settings helpers for UI sound events.

The event catalog lives in ``ui_sound_events`` (seeded by migration
``0032_ui_sounds``); this module is the only place that touches the uploaded
bytes. Files are stored under ``settings.gameplay_media_dir/ui_sounds`` on the
existing ``gameplay_media_data`` volume, but they are **not** mounted as a
static directory: the public download route resolves the row, checks ``active``
and applies the cache policy, so an inactive event can never be fetched by
guessing a file name.

Upload rules (the client ``content_type`` is never trusted):

* extension must be ``.mp3``, ``.wav`` or ``.ogg``;
* the bytes must actually be that format: ``RIFF``/``WAVE`` header, ``OggS``
  page header, or an MP3 ``ID3`` tag / MPEG frame sync;
* the payload is limited by ``settings.max_ui_sound_bytes`` (2 MiB by default).
"""
from __future__ import annotations

import math
import uuid
from pathlib import Path
from typing import Any

from sqlalchemy import select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.orm import Session

from app.config import settings
from app.models.ui_sound import UiSoundEvent, UserUiSoundSettings

UI_SOUNDS_DIRNAME = "ui_sounds"

#: Allowed extensions and the content type served for them.
UI_SOUND_FORMATS: dict[str, str] = {
    ".mp3": "audio/mpeg",
    ".wav": "audio/wav",
    ".ogg": "audio/ogg",
}

DEFAULT_SETTINGS: dict[str, Any] = {
    "enabled": True,
    "volume": 0.8,
    "navigation": True,
    "action": True,
    "system": True,
}

SETTING_FIELDS = tuple(DEFAULT_SETTINGS)


class UiSoundError(Exception):
    """A rejection with an HTTP status; the endpoint returns ``detail`` as is."""

    def __init__(self, message: str, status_code: int = 400) -> None:
        super().__init__(message)
        self.message = message
        self.status_code = status_code


def ui_sounds_dir() -> Path:
    return settings.gameplay_media_dir / UI_SOUNDS_DIRNAME


def safe_path(stored_name: str) -> Path | None:
    """Resolve a stored name inside the UI sounds directory, never outside it."""
    root = ui_sounds_dir().resolve()
    target = (root / stored_name).resolve()
    if not target.is_relative_to(root):
        return None
    return target


def stored_file(stored_name: str | None) -> Path | None:
    """The existing file for ``stored_name`` or ``None``."""
    if not stored_name:
        return None
    target = safe_path(stored_name)
    if target is None or not target.is_file():
        return None
    return target


def detect_format(data: bytes) -> str | None:
    """Sniff the container: ``'mp3'``, ``'wav'``, ``'ogg'`` or ``None``."""
    if len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WAVE":
        return "wav"
    if data.startswith(b"OggS"):
        return "ogg"
    if len(data) >= 4:
        has_id3 = data.startswith(b"ID3")
        has_frame_sync = data[0] == 0xFF and (data[1] & 0xE0) == 0xE0
        if has_id3 or has_frame_sync:
            return "mp3"
    return None


def validate_upload(filename: str | None, data: bytes) -> tuple[str, str]:
    """Validate an upload; return ``(suffix, content_type)``.

    Raises :class:`UiSoundError` with 400 for an invalid extension/body and 413
    for an oversized payload. The size limit is enforced by the caller before
    reading more than one byte past it, so this only reports a clear message.
    """
    suffix = Path(filename or "").suffix.lower()
    if suffix not in UI_SOUND_FORMATS:
        raise UiSoundError(
            "UI sound must be an .mp3, .wav or .ogg file", status_code=400
        )
    if not data:
        raise UiSoundError("Uploaded file is empty", status_code=400)
    if len(data) > settings.max_ui_sound_bytes:
        raise UiSoundError(
            "UI sound exceeds the configured upload limit "
            f"({settings.max_ui_sound_bytes // 1024} KiB)",
            status_code=413,
        )

    detected = detect_format(data)
    expected = suffix.lstrip(".")
    if detected is None:
        raise UiSoundError(
            "File does not appear to contain audio in the declared format",
            status_code=400,
        )
    if detected != expected:
        raise UiSoundError(
            f"File has a .{expected} extension but its content is {detected}",
            status_code=400,
        )
    return suffix, UI_SOUND_FORMATS[suffix]


def store_file(key: str, data: bytes, suffix: str) -> str:
    """Write ``data`` under a fresh generated name; return that name.

    The key is part of the name only for readability; the randomly generated
    part guarantees that a replacement never overwrites a file another request
    may still stream.
    """
    directory = ui_sounds_dir()
    prefix = key.replace(".", "-")[:48]
    stored_name = f"{prefix}-{uuid.uuid4().hex}{suffix}"
    target = safe_path(stored_name)
    if target is None:  # pragma: no cover - generated names cannot escape
        raise UiSoundError("Invalid storage path", status_code=400)
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as destination:
            destination.write(data)
    except OSError as exc:
        raise UiSoundError(
            "Could not store the UI sound file", status_code=500
        ) from exc
    return stored_name


def delete_file(stored_name: str | None) -> None:
    """Best-effort unlink; a missing file must never break an API response."""
    if not stored_name:
        return
    target = safe_path(stored_name)
    if target is None:
        return
    try:
        target.unlink(missing_ok=True)
    except OSError:
        pass


def get_event(db: Session, key: str) -> UiSoundEvent | None:
    return db.scalar(select(UiSoundEvent).where(UiSoundEvent.key == key))


def all_events(db: Session) -> list[UiSoundEvent]:
    return list(
        db.scalars(
            select(UiSoundEvent).order_by(UiSoundEvent.id.asc())
        ).all()
    )


def clamp_unit(value: float, *, field: str = "value") -> float:
    """Clamp ``value`` to ``[0, 1]``; reject NaN/infinity with a 422."""
    try:
        number = float(value)
    except (TypeError, ValueError) as exc:
        raise UiSoundError(f"{field} must be a number", status_code=422) from exc
    if not math.isfinite(number):
        raise UiSoundError(f"{field} must be a finite number", status_code=422)
    return max(0.0, min(1.0, number))


# ---------------------------------------------------------------------------
# Per-account settings
# ---------------------------------------------------------------------------
def settings_row(db: Session, user_id: Any) -> UserUiSoundSettings | None:
    return db.get(UserUiSoundSettings, user_id)


def settings_payload(db: Session, user_id: Any) -> dict[str, Any]:
    """The effective settings; a missing row is "all defaults"."""
    row = settings_row(db, user_id)
    if row is None:
        return {**DEFAULT_SETTINGS, "updated_at": None}
    return {
        "enabled": bool(row.enabled),
        "volume": float(row.volume),
        "navigation": bool(row.navigation),
        "action": bool(row.action),
        "system": bool(row.system),
        "updated_at": row.updated_at,
    }


def ensure_settings_row(db: Session, user_id: Any) -> UserUiSoundSettings:
    """Return the row, creating the default one on first use.

    ``ON CONFLICT DO NOTHING`` makes two concurrent first requests safe: the
    loser reads the row the winner inserted. The caller decides whether the
    transaction is committed (a plain read does not need to persist defaults).
    """
    row = settings_row(db, user_id)
    if row is not None:
        return row

    db.execute(
        pg_insert(UserUiSoundSettings.__table__)
        .values(user_id=user_id)
        .on_conflict_do_nothing(index_elements=["user_id"])
    )
    db.flush()
    db.expire_all()
    row = settings_row(db, user_id)
    if row is None:  # pragma: no cover - impossible after the upsert
        raise UiSoundError(
            "Could not load UI sound settings", status_code=500
        )
    return row


def update_settings(
    db: Session, user_id: Any, changes: dict[str, Any]
) -> dict[str, Any]:
    """Apply a partial update and return the effective settings."""
    row = ensure_settings_row(db, user_id)
    for field, value in changes.items():
        if field not in DEFAULT_SETTINGS or value is None:
            continue
        if field == "volume":
            value = clamp_unit(value, field="volume")
        setattr(row, field, value)
    db.commit()
    db.refresh(row)
    return settings_payload(db, user_id)
