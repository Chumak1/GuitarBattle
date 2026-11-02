"""Rules for the uploaded desktop builds: platform and artifact kind.

A ``GameBuild`` row is bound to exactly one platform and one artifact:

* ``installer`` — what a **new** user installs from the site: a Windows
  ``.exe``/``.msi`` (a legacy ``.zip`` is still accepted) or a universal macOS
  ``.dmg`` (legacy ``.zip`` too). This is what the public download button and
  ``GET /downloads/game`` serve.
* ``portable`` — the archive the **installed** client unpacks to update itself
  without an installer: always a ``.zip`` on both platforms. It is served by
  ``GET /downloads/game/update`` and advertised as ``updates`` on ``/landing``.

Exactly one build is published per ``(platform, artifact)`` pair, so publishing
a portable ZIP never unpublishes the installer of the same platform and vice
versa.

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

#: Canonical artifact identifiers; they are also the ``?artifact=`` values.
INSTALLER: Final = "installer"
PORTABLE: Final = "portable"

BUILD_ARTIFACTS: Final[tuple[str, ...]] = (INSTALLER, PORTABLE)
DEFAULT_BUILD_ARTIFACT: Final = INSTALLER

#: Human-readable name of each platform (admin UI, error messages).
PLATFORM_LABELS: Final[dict[str, str]] = {
    WINDOWS: "Windows",
    MACOS: "macOS",
}

#: Human-readable name of each artifact in the 415 detail.
ARTIFACT_LABELS: Final[dict[str, str]] = {
    INSTALLER: "installer",
    PORTABLE: "portable ZIP",
}

#: Allowed extension -> stored content type, per platform and artifact. The
#: extension is authoritative; ``UploadFile.content_type`` is never trusted.
GAME_BUILD_TYPES: Final[dict[str, dict[str, dict[str, str]]]] = {
    WINDOWS: {
        INSTALLER: {
            ".exe": "application/octet-stream",
            ".msi": "application/octet-stream",
            # Legacy: installers uploaded before the .exe/.msi rule existed.
            ".zip": "application/zip",
        },
        PORTABLE: {
            ".zip": "application/zip",
        },
    },
    MACOS: {
        INSTALLER: {
            ".dmg": "application/x-apple-diskimage",
            # Legacy: the universal build used to ship as a ZIP.
            ".zip": "application/zip",
        },
        PORTABLE: {
            ".zip": "application/zip",
        },
    },
}


def normalize_platform(value: str | None) -> str:
    """Return a canonical platform id, or ``""`` for an unknown value."""
    candidate = (value or "").strip().lower()
    return candidate if candidate in GAME_BUILD_PLATFORMS else ""


def normalize_artifact(value: str | None) -> str:
    """Return a canonical artifact id, or ``""`` for an unknown value."""
    candidate = (value or "").strip().lower()
    return candidate if candidate in BUILD_ARTIFACTS else ""


def platform_label(platform: str) -> str:
    return PLATFORM_LABELS.get(platform, platform)


def artifact_label(artifact: str) -> str:
    return ARTIFACT_LABELS.get(artifact, artifact)


def platform_check_sql(column: str = "platform") -> str:
    """``CHECK`` expression pinning the column to the known platforms."""
    values = ", ".join(f"'{platform}'" for platform in GAME_BUILD_PLATFORMS)
    return f"{column} IN ({values})"


def artifact_check_sql(column: str = "artifact") -> str:
    """``CHECK`` expression pinning the column to the known artifacts."""
    values = ", ".join(f"'{artifact}'" for artifact in BUILD_ARTIFACTS)
    return f"{column} IN ({values})"


def allowed_extensions(
    platform: str, artifact: str = DEFAULT_BUILD_ARTIFACT
) -> tuple[str, ...]:
    return tuple(GAME_BUILD_TYPES.get(platform, {}).get(artifact, {}))


def allowed_extensions_text(
    platform: str, artifact: str = DEFAULT_BUILD_ARTIFACT
) -> str:
    """``.dmg or .zip`` — used in the 415 detail and the admin hint."""
    extensions = allowed_extensions(platform, artifact)
    if not extensions:
        return ""
    if len(extensions) == 1:
        return extensions[0]
    return ", ".join(extensions[:-1]) + " or " + extensions[-1]


def requirements_text(
    platform: str, artifact: str = DEFAULT_BUILD_ARTIFACT
) -> str:
    """One sentence naming the rejected pair, e.g. ``Windows portable ZIP must
    be .zip``."""
    return (
        f"{platform_label(platform)} {artifact_label(artifact)} must be "
        f"{allowed_extensions_text(platform, artifact)}"
    )


def content_type_for(
    platform: str,
    suffix: str,
    artifact: str = DEFAULT_BUILD_ARTIFACT,
) -> str | None:
    """Stored content type for ``platform`` + ``artifact`` + ``suffix``, or
    ``None`` if the triple is not allowed."""
    return GAME_BUILD_TYPES.get(platform, {}).get(artifact, {}).get(suffix.lower())
