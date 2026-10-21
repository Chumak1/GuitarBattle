"""Small helpers shared by the API tests.

Everything here is deliberately transport-level: the tests exercise the same
HTTP contract the JUCE client uses, not the service internals.
"""
from __future__ import annotations

import uuid
from io import BytesIO
from typing import Any

import mido

API = "/api/v1"
DEFAULT_PASSWORD = "GuitarBattleTest123"


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
        token = login_user(client, username=username, password=password)
        account["token"] = token["access_token"]
        account["headers"] = auth_headers(token["access_token"])
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


def build_midi_bytes(
    *,
    bpm: int = 120,
    ticks_per_beat: int = 480,
    note: int = 60,
    velocity: int = 100,
    duration_ticks: int = 480,
) -> bytes:
    """Build a minimal, valid type-0 Standard MIDI File in memory."""
    midi = mido.MidiFile(type=0, ticks_per_beat=ticks_per_beat)
    track = mido.MidiTrack()
    midi.tracks.append(track)
    track.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(bpm), time=0))
    track.append(mido.Message("note_on", note=note, velocity=velocity, time=0))
    track.append(mido.Message("note_off", note=note, velocity=0, time=duration_ticks))
    buffer = BytesIO()
    midi.save(file=buffer)
    return buffer.getvalue()
