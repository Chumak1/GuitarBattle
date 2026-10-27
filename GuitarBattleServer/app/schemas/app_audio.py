"""Schemas for the global application audio (background music)."""
from datetime import datetime

from pydantic import BaseModel


class BackgroundMusicResponse(BaseModel):
    """Public background music descriptor.

    The shape is part of the client contract: ``url`` is ``None`` when no track
    is uploaded, ``size_bytes`` is ``0`` in that case. The endpoint always
    answers 200, never 404, so the client can poll it without error handling.
    """

    url: str | None = None
    content_type: str | None = None
    size_bytes: int = 0


class AdminBackgroundMusicResponse(BackgroundMusicResponse):
    """Admin status: the public fields plus upload metadata."""

    uploaded_at: datetime | None = None
    original_name: str | None = None
    sha256: str | None = None
