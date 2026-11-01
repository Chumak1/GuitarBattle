"""Normalize the free-form OS name reported by the desktop client.

The JUCE client sends ``juce::SystemStats::getOperatingSystemName()`` verbatim
(``Windows 11``, ``Windows 10``, ``Mac OSX 26.1``, ``Linux``, …), so the raw
strings are too detailed for a table cell and differ between releases. The
admin panel groups them into four families and keeps the original string for
tooltips, so nothing reported by a client is ever lost.
"""
from __future__ import annotations

WINDOWS = "Windows"
MACOS = "macOS"
LINUX = "Linux"
UNKNOWN = "Unknown"

# Display order used by the admin panel when two families were seen at the same
# moment; the API itself sorts by recency.
PLATFORM_FAMILIES: tuple[str, ...] = (WINDOWS, MACOS, LINUX, UNKNOWN)

# Substrings are matched against the lower-cased raw name. ``osx``/``os x`` are
# checked before ``windows`` only for clarity: "Mac OSX 26.1" contains neither
# "windows" nor "win", so the order does not change the result.
_MACOS_MARKERS = ("mac", "darwin", "osx", "os x")
_LINUX_MARKERS = ("linux", "ubuntu", "debian", "fedora", "mint", "opensuse", "centos")


def normalize_platform(raw: str | None) -> str:
    """Map a raw client OS name to a display family.

    Unknown and empty values become ``Unknown`` instead of being dropped: a
    session without a recognizable OS name still proves the user was online.
    """
    if raw is None:
        return UNKNOWN

    value = raw.strip().lower()
    if not value:
        return UNKNOWN

    # JUCE reports "Windows 11", "Windows 10", and older builds "Win32".
    if "windows" in value or value.startswith("win"):
        return WINDOWS
    if any(marker in value for marker in _MACOS_MARKERS):
        return MACOS
    if any(marker in value for marker in _LINUX_MARKERS):
        return LINUX
    return UNKNOWN
