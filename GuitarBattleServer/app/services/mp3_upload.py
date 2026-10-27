"""Shared validation for uploaded MP3 files.

Both song previews and the global background music accept the same format and
size limit, so the rules live here instead of being duplicated in the two admin
routers. The helpers are transport-agnostic and return booleans; the endpoints
translate a failure into 415/422/413 with a human-readable ``detail``.
"""
from __future__ import annotations

from pathlib import Path

MP3_CONTENT_TYPE = "audio/mpeg"


def has_mp3_extension(filename: str | None) -> bool:
    """Only ``.mp3`` is accepted; the extension is the first cheap filter."""
    return Path(filename or "").suffix.lower() == ".mp3"


def looks_like_mp3(data: bytes) -> bool:
    """Accept the common MP3 forms: an ``ID3`` tag or an MPEG frame sync.

    The upload ``content_type`` is never trusted, and this is deliberately a
    signature check only: it proves the bytes start like MP3 audio without
    pretending to fully decode the file.
    """
    if len(data) < 4:
        return False
    has_id3 = data.startswith(b"ID3")
    has_frame_sync = data[0] == 0xFF and (data[1] & 0xE0) == 0xE0
    return has_id3 or has_frame_sync
