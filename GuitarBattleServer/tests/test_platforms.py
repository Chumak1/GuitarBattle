"""Unit tests for the OS-name normalization used by the admin users table.

The desktop client sends ``juce::SystemStats::getOperatingSystemName()``
verbatim, so the same family arrives under many strings and differs between
releases. These tests pin the mapping without touching the database.
"""
import pytest

from app.services.platforms import normalize_platform


@pytest.mark.parametrize(
    ("raw", "family"),
    [
        ("Windows 11", "Windows"),
        ("Windows 10", "Windows"),
        ("Windows 10 Pro for Workstations", "Windows"),
        ("win32", "Windows"),
        ("  Windows 11  ", "Windows"),
        ("Mac OSX 26.1", "macOS"),
        ("macOS 14.5", "macOS"),
        ("Darwin 23.6.0", "macOS"),
        ("Linux", "Linux"),
        ("Ubuntu 24.04", "Linux"),
        ("Debian 12", "Linux"),
        ("Fedora Linux 41", "Linux"),
        (None, "Unknown"),
        ("", "Unknown"),
        ("   ", "Unknown"),
        ("FreeBSD 14", "Unknown"),
    ],
)
def test_normalize_platform(raw, family):
    assert normalize_platform(raw) == family
