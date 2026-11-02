"""Platform rules for the uploaded desktop installers.

A ``GameBuild`` row is bound to exactly one platform, and the accepted file
extensions depend on it: Windows ships ``.exe``/``.msi`` installers (or a
``.zip``), macOS ships a universal ``.dmg`` (or a ``.zip``) that runs on both
Apple Silicon and Intel Macs. Exactly one build is published per platform.

The module has no application imports so it can be used by the models, the
public endpoints and the admin router without a cycle.
"""
from __future__ import annotations

from typing import Final

#: Canonical platform identifiers; they are also the ``?platform=`` values.
WINDOWS: Final = "windows"
MACOS: Final = "macos"

GAME_BUILD_PLATFORMS: Final[tuple[str, ...]] = (WINDOWS, MACOS)
DEFAULT_GAME_BUILD_PLATFORM: Final = WINDOWS

#: Human-readable name of each platform (admin UI, error messages).
PLATFORM_LABELS: Final[dict[str, str]] = {
    WINDOWS: "Windows",
    MACOS: "macOS",
}

#: Allowed extension -> stored content type, per platform. The extension is
#: authoritative; ``UploadFile.content_type`` is never trusted.
GAME_BUILD_TYPES: Final[dict[str, dict[str, str]]] = {
    WINDOWS: {
        ".exe": "application/octet-stream",
        ".zip": "application/zip",
        ".msi": "application/octet-stream",
    },
    MACOS: {
        ".dmg": "application/x-apple-diskimage",
        ".zip": "application/zip",
    },
}


def normalize_platform(value: str | None) -> str:
    """Return a canonical platform id, or ``""`` for an unknown value."""
    candidate = (value or "").strip().lower()
    return candidate if candidate in GAME_BUILD_TYPES else ""


def platform_label(platform: str) -> str:
    return PLATFORM_LABELS.get(platform, platform)


def platform_check_sql(column: str = "platform") -> str:
    """``CHECK`` expression pinning the column to the known platforms."""
    values = ", ".join(f"'{platform}'" for platform in GAME_BUILD_PLATFORMS)
    return f"{column} IN ({values})"


def allowed_extensions(platform: str) -> tuple[str, ...]:
    return tuple(GAME_BUILD_TYPES.get(platform, {}))


def allowed_extensions_text(platform: str) -> str:
    """``.dmg or .zip`` — used in the 415 detail and the admin hint."""
    extensions = allowed_extensions(platform)
    if not extensions:
        return ""
    if len(extensions) == 1:
        return extensions[0]
    return ", ".join(extensions[:-1]) + " or " + extensions[-1]


def content_type_for(platform: str, suffix: str) -> str | None:
    """Stored content type for ``platform`` + ``suffix``, or ``None`` if the
    pair is not allowed."""
    return GAME_BUILD_TYPES.get(platform, {}).get(suffix.lower())
