"""Pydantic schemas for the public feedback endpoint.

The JSON contract is shared with the JUCE client (``POST /api/v1/feedback``):

    {"message": "...", "username": null, "app_version": null,
     "attachment_name": null, "attachment_data": null}

``message`` is the only required field. ``attachment_data`` is base64 of the
raw file bytes and is capped at 5 MiB after decoding. There is no ``email``
field: the reply address comes from the bearer token on the server (see the
endpoint), never from the request body.

Two encodings are accepted for ``attachment_data``:

* plain RFC 4648 base64 (JUCE clients 1.4.2+ send this), and
* the legacy ``MemoryBlock::toBase64Encoding()`` layout sent by clients up to
  1.4.1: ``"<decimal byte size>.<data>"``, e.g. ``"1000./9j/4AA..."``.

The legacy payload is decoded correctly *including* JUCE's non-standard base64
alphabet (see ``_JUCE_BASE64_TO_RFC``): simply stripping the prefix and calling
``base64.b64decode`` would accept the request but silently corrupt the file.
The declared size is treated as optional metadata and is never enforced.
"""
from __future__ import annotations

import base64
import binascii
import re

from pydantic import BaseModel, ConfigDict, Field, field_validator

FEEDBACK_MESSAGE_MAX_LENGTH = 4000
ATTACHMENT_MAX_BYTES = 5 * 1024 * 1024
DEFAULT_ATTACHMENT_NAME = "attachment.bin"
ATTACHMENT_NAME_MAX_LENGTH = 120

# Characters that are unsafe in a filename on Windows, POSIX or in a MIME
# header. Path separators are removed together with the directories.
_UNSAFE_FILENAME_CHARS = set('<>:"/\\|?*\r\n\t')

# Cheap upper bound on the encoded length before allocating a decoder buffer:
# 4 chars per 3 decoded bytes (RFC 4648 and JUCE agree on this ratio).
_MAX_ENCODED_LENGTH = ((ATTACHMENT_MAX_BYTES + 2) // 3) * 4
_OVERSIZE_MESSAGE = (
    "attachment_data must decode to at most 5 MB "
    f"({ATTACHMENT_MAX_BYTES} bytes)"
)
_INVALID_BASE64_MESSAGE = "attachment_data must be valid base64"
_EMPTY_ATTACHMENT_MESSAGE = "attachment_data must not be empty"

# ``MemoryBlock::toBase64Encoding()`` writes the decimal byte count, a '.', then
# the data. Plain RFC 4648 never contains a '.', so this cannot misfire on a
# base64 string sent by a newer client.
_LEGACY_PREFIX = re.compile(r"\d+\.")

# JUCE uses its own 64-character table (juce_core/memory/juce_MemoryBlock.cpp):
# index 0 is '.', then A-Z, a-z, 0-9 and '+'. It never emits '=' padding or
# '/'. Mapping it onto the RFC 4648 alphabet lets us reuse the C-accelerated
# ``base64.b64decode`` instead of a per-character Python loop over megabytes.
_JUCE_BASE64_TO_RFC = str.maketrans(
    ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
)


def sanitize_attachment_name(value: str | None) -> str | None:
    """Return a bare, printable filename (no directories), or ``None``."""
    if value is None:
        return None
    # Keep only the last path component, for both separators.
    name = value.replace("\\", "/").rsplit("/", 1)[-1]
    name = "".join(
        char
        for char in name
        if char.isprintable() and char not in _UNSAFE_FILENAME_CHARS
    )
    name = name.strip().strip(".")
    return name[:ATTACHMENT_NAME_MAX_LENGTH] or None


def _single_line(value: str | None, *, limit: int) -> str | None:
    """Trim an optional metadata field, drop control characters and truncate.

    ``username`` ends up in the email subject, so a newline would allow header
    injection (``EmailMessage`` rejects it, but the request should be cleaned up
    instead of failing with a 500). Metadata never rejects a request: only
    ``message`` and ``attachment_data`` are validated.
    """
    if value is None:
        return None
    cleaned = " ".join(value.split())
    return cleaned[:limit] or None


class FeedbackRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    message: str = Field(min_length=1, max_length=FEEDBACK_MESSAGE_MAX_LENGTH)
    username: str | None = None
    app_version: str | None = None
    attachment_name: str | None = None
    attachment_data: str | None = None

    @field_validator("message")
    @classmethod
    def _require_visible_message(cls, value: str) -> str:
        # ``min_length`` counts whitespace, so "   " would otherwise be accepted.
        if not value.strip():
            raise ValueError("message must not be empty")
        if len(value) > FEEDBACK_MESSAGE_MAX_LENGTH:
            raise ValueError(
                f"message must be at most {FEEDBACK_MESSAGE_MAX_LENGTH} characters"
            )
        return value

    @field_validator("username")
    @classmethod
    def _clean_username(cls, value: str | None) -> str | None:
        return _single_line(value, limit=64)

    @field_validator("app_version")
    @classmethod
    def _clean_app_version(cls, value: str | None) -> str | None:
        return _single_line(value, limit=64)

    @field_validator("attachment_name")
    @classmethod
    def _clean_attachment_name(cls, value: str | None) -> str | None:
        return sanitize_attachment_name(value)


def decode_attachment_data(value: str | None) -> bytes | None:
    """Decode the base64 attachment, enforcing the decoded-size cap.

    Accepts plain RFC 4648 base64 and the legacy JUCE
    ``"<decimal size>.<data>"`` layout (clients up to 1.4.1). The declared size
    is metadata only: it is not compared with the decoded length, so a stale or
    wrong number cannot reject an otherwise valid attachment.

    Decoding lives here rather than in a Pydantic validator on purpose: the
    default 422 handler echoes the offending ``input``, which for an oversized
    attachment would reflect megabytes of base64 back to the client. The
    endpoint catches ``ValueError`` and returns a small ``{"detail": ...}``
    body instead. The message is safe to log (it never contains the payload).

    Raises:
        ValueError: for invalid base64, an empty payload or more than 5 MiB of
            decoded data.
    """
    if value is None:
        return None
    compact = "".join(value.split())
    if not compact:
        return None

    legacy = _LEGACY_PREFIX.match(compact)
    if legacy is not None:
        body = compact[legacy.end():]
        if not body:
            raise ValueError(_EMPTY_ATTACHMENT_MESSAGE)
        # '=' padding and '/' are valid RFC 4648 but never produced by JUCE, so
        # a prefixed body containing them was plain base64 that carried a size
        # prefix anyway.
        if "/" in body or "=" in body:
            decoded = _decode_rfc4648(body)
        else:
            decoded = _decode_juce(body)
    else:
        decoded = _decode_rfc4648(compact)

    if not decoded:
        raise ValueError(_EMPTY_ATTACHMENT_MESSAGE)
    return decoded


def _decode_rfc4648(text: str) -> bytes:
    """Decode standard, padded base64. Empty input decodes to ``b""``."""
    if len(text) > _MAX_ENCODED_LENGTH:
        raise ValueError(_OVERSIZE_MESSAGE)

    try:
        decoded = base64.b64decode(text, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise ValueError(_INVALID_BASE64_MESSAGE) from exc

    if len(decoded) > ATTACHMENT_MAX_BYTES:
        raise ValueError(_OVERSIZE_MESSAGE)
    return decoded


def _decode_juce(text: str) -> bytes:
    """Decode the data part of a legacy JUCE ``"<size>.<data>"`` attachment.

    JUCE emits no padding and 6 bits per character; trailing bits that do not
    fill a byte are zero and are dropped, which matches
    ``MemoryBlock::fromBase64Encoding``.
    """
    if len(text) > _MAX_ENCODED_LENGTH:
        raise ValueError(_OVERSIZE_MESSAGE)

    translated = text.translate(_JUCE_BASE64_TO_RFC)
    padding = (-len(translated)) % 4
    if padding == 3:
        # Three characters are missing, i.e. the length is 1 more than a
        # multiple of 4: a single leftover character carries 6 bits, less than a
        # byte. No JUCE encoding can produce this, so treat it as garbage.
        raise ValueError(_INVALID_BASE64_MESSAGE)

    try:
        decoded = base64.b64decode(translated + "=" * padding, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise ValueError(_INVALID_BASE64_MESSAGE) from exc

    if len(decoded) > ATTACHMENT_MAX_BYTES:
        raise ValueError(_OVERSIZE_MESSAGE)
    return decoded


class FeedbackResponse(BaseModel):
    """``{"ok": true}`` on success, ``{"ok": false, "detail": ...}`` otherwise."""

    ok: bool
    detail: str | None = None
