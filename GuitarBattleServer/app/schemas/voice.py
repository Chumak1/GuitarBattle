"""Request/response schemas for the lobby voice relay (``/api/v1/voice``).

One request carries one chunk of raw PCM, base64-encoded. The payload is never
echoed back by the server except to the opponent it belongs to, and it is never
stored or logged.

The second half of the module is the direct-UDP endpoint exchange: a client
publishes the ephemeral port of its own UDP socket and reads back the endpoint
of the *other* member of the lobby. The IP is never accepted from the body —
the server always derives it from the connection.
"""
from __future__ import annotations

import base64
import binascii
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator

#: 200 ms of 16 kHz mono signed 16-bit little-endian PCM.
MAX_CHUNK_BYTES = 6400
#: Base64 of ``MAX_CHUNK_BYTES`` is ``ceil(6400 / 3) * 4 = 8536`` characters.
#: The extra margin keeps a base64 chunk valid while rejecting a multi-megabyte
#: body before it is decoded.
MAX_CHUNK_BASE64_CHARS = 8800


class VoiceChunkIn(BaseModel):
    """``{"data": "<base64 PCM>"}``."""

    model_config = ConfigDict(extra="ignore")

    data: str = Field(min_length=1, max_length=MAX_CHUNK_BASE64_CHARS)

    @field_validator("data")
    @classmethod
    def _validate_chunk(cls, value: str) -> str:
        try:
            raw = base64.b64decode(value, validate=True)
        except (binascii.Error, ValueError) as exc:
            raise ValueError("data must be valid base64") from exc
        if not raw:
            raise ValueError("data must not be empty")
        if len(raw) > MAX_CHUNK_BYTES:
            raise ValueError(
                f"data must decode to at most {MAX_CHUNK_BYTES} bytes"
            )
        return value

    def decoded(self) -> bytes:
        """The PCM bytes (already validated by the field validator)."""
        return base64.b64decode(self.data, validate=True)


class VoiceChunkAck(BaseModel):
    """The lobby-wide sequence number assigned to the accepted chunk."""

    seq: int = Field(ge=0)


class VoiceChunkOut(BaseModel):
    """One chunk of another lobby member."""

    seq: int
    user_id: UUID
    username: str
    data: str


class VoiceChunkListResponse(BaseModel):
    """Polled chunks (oldest first) and the cursor for the next poll."""

    chunks: list[VoiceChunkOut]
    next_since: int


class VoiceEndpointIn(BaseModel):
    """``{"port": 49152}`` — the port of the caller's local UDP socket.

    Only the port is accepted: the public IP always comes from the request
    itself (``X-Forwarded-For`` behind Caddy, the socket peer otherwise), so a
    client can never point its opponent at somebody else's address. ``strict``
    rejects ``"49152"`` and ``49152.0`` as well, which keeps a sloppy client
    from silently publishing a wrong port.
    """

    model_config = ConfigDict(extra="ignore")

    port: int = Field(ge=1, le=65535, strict=True)


class VoicePeerEndpoint(BaseModel):
    """The UDP endpoint the other lobby member published (or ``null``)."""

    ip: str
    port: int


class VoiceEndpointOut(BaseModel):
    """The caller's public endpoint echoed back plus the peer's, if known."""

    ip: str
    port: int
    peer: VoicePeerEndpoint | None = None
