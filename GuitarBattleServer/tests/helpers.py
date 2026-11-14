"""Small helpers shared by the API tests.

Everything here is deliberately transport-level: the tests exercise the same
HTTP contract the JUCE client uses, not the service internals.
"""
from __future__ import annotations

import re
import uuid
from io import BytesIO
from typing import Any

import mido

from app.services.email_service import get_test_outbox

API = "/api/v1"
DEFAULT_PASSWORD = "GuitarBattleTest123"
_CODE_RE = re.compile(r"\b(\d{6})\b")


def _latest_message_to(email: str, subject_part: str) -> str:
    for message in reversed(get_test_outbox()):
        if message.to == email and subject_part in message.subject:
            return message.text
    raise AssertionError(f"No email with {subject_part!r} was sent to {email}")


def verification_code(email: str) -> str:
    text = _latest_message_to(email, "подтверждение")
    match = _CODE_RE.search(text)
    assert match is not None, text
    return match.group(1)


def password_reset_code(email: str) -> str:
    text = _latest_message_to(email, "восстановление")
    match = _CODE_RE.search(text)
    assert match is not None, text
    return match.group(1)


def unique(prefix: str) -> str:
    """Return a unique value so repeated runs never collide with old rows."""
    return f"{prefix}_{uuid.uuid4().hex[:12]}"


def unique_slug(prefix: str = "song") -> str:
    """Slugs accept only lowercase letters, digits and single hyphens."""
    return f"{prefix}-{uuid.uuid4().hex[:12]}"


def auth_headers(token: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}"}


def register_user(
    client,
    *,
    username: str | None = None,
    email: str | None = None,
    password: str = DEFAULT_PASSWORD,
    display_name: str | None = None,
    login: bool = True,
) -> dict[str, Any]:
    """Register a user and (by default) return its access token as well."""
    username = username or unique("user")
    # example.com is accepted by email-validator; .test/.invalid are rejected
    # as special-use domains even though they are syntactically valid.
    email = email or f"{username}@example.com"
    payload: dict[str, Any] = {
        "username": username,
        "email": email,
        "password": password,
        "display_name": display_name,
        # Registration is impossible without accepting the privacy policy.
        "privacy_consent": True,
    }
    response = client.post(f"{API}/auth/register", json=payload)
    assert response.status_code == 201, response.text
    body = response.json()
    account: dict[str, Any] = {
        "id": body["id"],
        "username": body["username"],
        "email": body["email"],
        "password": password,
        "register_response": body,
    }
    if login:
        verification = client.post(
            f"{API}/auth/verify-email",
            json={"email": body["email"], "code": verification_code(body["email"])},
        )
        assert verification.status_code == 200, verification.text
        account["token"] = verification.json()["access_token"]
        account["headers"] = auth_headers(account["token"])
    return account


def login_user(client, *, username: str, password: str = DEFAULT_PASSWORD) -> dict[str, Any]:
    response = client.post(
        f"{API}/auth/login",
        json={"identifier": username, "password": password},
    )
    assert response.status_code == 200, response.text
    return response.json()


def create_song(client, admin_headers: dict[str, str], **overrides: Any) -> dict[str, Any]:
    """Create a draft song through the admin API and return the JSON response."""
    payload: dict[str, Any] = {
        "slug": unique_slug(),
        "title": "Test Song",
        "artist": "Test Artist",
        "genre": "Rock",
        "bpm": 120,
        "difficulty": 3,
        "duration_seconds": 90,
        "description": "Created by the automated test suite",
    }
    payload.update(overrides)
    response = client.post(f"{API}/admin/songs", json=payload, headers=admin_headers)
    assert response.status_code == 201, response.text
    return response.json()


def publish_song(client, admin_headers: dict[str, str], song_id: str) -> dict[str, Any]:
    response = client.post(f"{API}/admin/songs/{song_id}/publish", headers=admin_headers)
    assert response.status_code == 200, response.text
    return response.json()


def schedule_daily(
    client, admin_headers: dict[str, str], weekday: int, song_id: str | None
) -> dict[str, Any]:
    """Assign (or clear, with ``song_id=None``) one Daily Challenge weekday."""
    response = client.put(
        f"{API}/admin/daily/{weekday}",
        json={"song_id": song_id},
        headers=admin_headers,
    )
    assert response.status_code == 200, response.text
    return response.json()


def daily_schedule(client, admin_headers: dict[str, str]) -> list[dict[str, Any]]:
    response = client.get(f"{API}/admin/daily", headers=admin_headers)
    assert response.status_code == 200, response.text
    return response.json()["items"]


def build_mp3_bytes(*, marker: int = 0, size: int = 256, id3: bool = True) -> bytes:
    """Build a byte payload that passes the MP3 signature check.

    Real audio is not needed: the server validates only the extension, the size
    limit and the leading signature (``ID3`` or an MPEG frame sync).
    """
    header = b"ID3" if id3 else b"\xff\xfb"
    body_length = max(0, size - len(header))
    return header + bytes([marker & 0xFF]) * body_length


def build_midi_bytes(
    *,
    bpm: int = 120,
    ticks_per_beat: int = 480,
    note: int = 60,
    velocity: int = 100,
    duration_ticks: int = 480,
    start_tick: int = 0,
    include_tempo: bool = True,
) -> bytes:
    """Build a minimal, valid type-0 Standard MIDI File in memory.

    ``include_tempo=False`` reproduces files exported without a ``set_tempo``
    meta event (the parser then has to fall back to the caller's BPM).
    """
    midi = mido.MidiFile(type=0, ticks_per_beat=ticks_per_beat)
    track = mido.MidiTrack()
    midi.tracks.append(track)
    if include_tempo:
        track.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(bpm), time=0))
    track.append(
        mido.Message("note_on", note=note, velocity=velocity, time=max(0, start_tick))
    )
    track.append(mido.Message("note_off", note=note, velocity=0, time=duration_ticks))
    buffer = BytesIO()
    midi.save(file=buffer)
    return buffer.getvalue()
