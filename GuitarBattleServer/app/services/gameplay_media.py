"""Storage and validation of gameplay visuals (characters and stages).

The admin panel uploads images; this module is the single place that validates
the bytes and writes them to disk. It never trusts the client ``content_type``
and never uses the client file name as a path: files are stored under a
generated UUID name inside the configured directory.

Layout inside ``settings.gameplay_media_dir``::

    characters/<uuid>.png   -> /media/characters/<uuid>.png
    stages/<uuid>.png|jpg   -> /media/stages/<uuid>.png|jpg

A UI-level spritesheet must be a transparent PNG with eight poses in a 4x2
grid, so its width must be exactly twice its height (and both divisible by the
grid). Stage backgrounds accept PNG or JPEG and any size.
"""
from __future__ import annotations

import uuid
from pathlib import Path

from app.config import settings

CHARACTERS_URL_PREFIX = "/media/characters/"
STAGES_URL_PREFIX = "/media/stages/"
CHARACTERS_DIRNAME = "characters"
STAGES_DIRNAME = "stages"

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
JPEG_SIGNATURE = b"\xff\xd8\xff"


class GameplayMediaError(Exception):
    """Invalid upload. ``status_code`` is what the API returns verbatim."""

    def __init__(self, message: str, status_code: int = 422) -> None:
        super().__init__(message)
        self.message = message
        self.status_code = status_code


def characters_dir() -> Path:
    return settings.gameplay_media_dir / CHARACTERS_DIRNAME


def stages_dir() -> Path:
    return settings.gameplay_media_dir / STAGES_DIRNAME


def character_sheet_url(sheet_key: str) -> str:
    """Origin-relative public URL of a stored spritesheet."""
    return f"{CHARACTERS_URL_PREFIX}{sheet_key}"


def stage_background_url(background_key: str) -> str:
    """Origin-relative public URL of a stored stage background."""
    return f"{STAGES_URL_PREFIX}{background_key}"


def _png_size(data: bytes) -> tuple[int, int] | None:
    """Read width/height from the IHDR chunk of a PNG payload."""
    # 8-byte signature + 4-byte length + ``IHDR`` + width + height.
    if len(data) < 24 or data[12:16] != b"IHDR":
        return None
    width = int.from_bytes(data[16:20], "big")
    height = int.from_bytes(data[20:24], "big")
    if width <= 0 or height <= 0:
        return None
    return width, height


def _is_jpeg(data: bytes) -> bool:
    return data.startswith(JPEG_SIGNATURE)


def validate_spritesheet(data: bytes) -> str:
    """Validate a character spritesheet; return its content type."""
    if not data.startswith(PNG_SIGNATURE):
        raise GameplayMediaError(
            "Character spritesheet must be a PNG file", status_code=415
        )
    size = _png_size(data)
    if size is None:
        raise GameplayMediaError("PNG file is malformed: no valid IHDR header")
    width, height = size
    if width % 4 != 0 or height % 2 != 0 or width // 4 != height // 2:
        raise GameplayMediaError(
            "Spritesheet must contain 8 poses in a 4x2 grid: "
            "its width must be exactly twice its height"
        )
    return "image/png"


def validate_background(data: bytes) -> tuple[str, str]:
    """Validate a stage background; return ``(extension, content type)``."""
    if data.startswith(PNG_SIGNATURE):
        if _png_size(data) is None:
            raise GameplayMediaError("PNG file is malformed: no valid IHDR header")
        return ".png", "image/png"
    if _is_jpeg(data):
        return ".jpg", "image/jpeg"
    raise GameplayMediaError(
        "Stage background must be a PNG or JPEG image", status_code=415
    )


def _safe_path(directory: Path, stored_name: str) -> Path:
    root = directory.resolve()
    target = (root / stored_name).resolve()
    if not target.is_relative_to(root):
        raise GameplayMediaError("Invalid storage path", status_code=400)
    return target


def store_image(directory: Path, data: bytes, suffix: str) -> str:
    """Write ``data`` under a generated name; return that name."""
    stored_name = f"{uuid.uuid4().hex}{suffix}"
    target = _safe_path(directory, stored_name)
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as destination:
            destination.write(data)
    except OSError as exc:
        raise GameplayMediaError(
            "Could not store the uploaded image", status_code=500
        ) from exc
    return stored_name


def delete_image(directory: Path, stored_name: str | None) -> None:
    """Remove a stored image; a missing file never raises."""
    if not stored_name:
        return
    try:
        _safe_path(directory, stored_name).unlink(missing_ok=True)
    except (OSError, GameplayMediaError):
        pass
