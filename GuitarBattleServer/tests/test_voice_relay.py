"""API + unit tests for the lobby voice relay (``/api/v1/voice``).

Voice is a REST polling relay because the JUCE 9.0.2 client has no WebSocket
transport: a member posts base64 PCM chunks and polls the chunks of the other
member of the same duel lobby. The scenarios below mirror the client contract
exactly (``docs/VOICE_API.md``), plus the in-memory registry limits that cannot
be observed through HTTP without waiting three seconds.

The second half covers the direct-UDP endpoint exchange
(``POST /voice/lobbies/{lobby_id}/endpoint``): the caller publishes the port of
its own UDP socket, the server derives the public IP itself and hands back the
*other* member's stored endpoint.
"""
from __future__ import annotations

import base64
import uuid
from typing import Callable
from uuid import UUID

import pytest

from app.services.voice_relay import (
    ENDPOINT_TTL_SECONDS,
    MAX_CHUNK_BYTES,
    MAX_CHUNKS_PER_LOBBY,
    MAX_CHUNKS_PER_POLL,
    MAX_CHUNKS_PER_USER,
    MAX_POSTS_PER_SECOND,
    VoiceRateLimited,
    VoiceRelay,
    voice_relay,
)

pytestmark = pytest.mark.db

API = "/api/v1"
VOICE = f"{API}/voice/lobbies"
ROOMS = f"{API}/duels/lobbies"

PCM_NOTES_MAX = 6400

#: Public, globally routable addresses so ``client_ip`` accepts them.
#: 203.0.113.0/24 and 198.51.100.0/24 are documentation ranges and are
#: *not* ``is_global``, so they cannot be used here.
HOST_IP = "8.8.8.8"
GUEST_IP = "1.1.1.1"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
@pytest.fixture(autouse=True)
def empty_relay():
    """The registry is process-wide: never leak a buffer between tests."""
    voice_relay.reset()
    yield
    voice_relay.reset()


def _create_room(client, headers, name: str = "Voice room") -> str:
    response = client.post(ROOMS, json={"name": name}, headers=headers)
    assert response.status_code == 201, response.text
    return response.json()["lobby_id"]


def _voice_room(client, host, guest) -> str:
    lobby_id = _create_room(
        client, host["headers"], name=f"Voice {uuid.uuid4().hex[:8]}"
    )
    joined = client.post(f"{ROOMS}/{lobby_id}/join", headers=guest["headers"])
    assert joined.status_code == 200, joined.text
    return lobby_id


def _pcm(size: int = 320, marker: int = 0) -> bytes:
    return bytes((marker + index) % 251 for index in range(size))


def _b64(raw: bytes) -> str:
    return base64.b64encode(raw).decode("ascii")


def _post(client, lobby_id: str, headers, raw: bytes):
    return client.post(
        f"{VOICE}/{lobby_id}/chunks", json={"data": _b64(raw)}, headers=headers
    )


def _poll(client, lobby_id: str, headers, since: int | None = None) -> dict:
    url = f"{VOICE}/{lobby_id}/chunks"
    if since is not None:
        url += f"?since={since}"
    response = client.get(url, headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _clear(client, lobby_id: str, headers):
    return client.delete(f"{VOICE}/{lobby_id}/chunks", headers=headers)


# ---------------------------------------------------------------------------
# Access control
# ---------------------------------------------------------------------------
def test_voice_requires_a_token(db_client):
    lobby_id = uuid.uuid4()
    assert db_client.post(
        f"{VOICE}/{lobby_id}/chunks", json={"data": _b64(b"\x00\x01")}
    ).status_code == 401
    assert db_client.get(f"{VOICE}/{lobby_id}/chunks?since=0").status_code == 401
    assert db_client.delete(f"{VOICE}/{lobby_id}/chunks").status_code == 401


def test_unknown_lobby_is_404(db_client, user_account):
    lobby_id = uuid.uuid4()
    response = db_client.post(
        f"{VOICE}/{lobby_id}/chunks",
        json={"data": _b64(b"\x00\x01")},
        headers=user_account["headers"],
    )
    assert response.status_code == 404, response.text
    assert response.json()["detail"]["code"] == "lobby_not_found"
    assert (
        db_client.get(
            f"{VOICE}/{lobby_id}/chunks", headers=user_account["headers"]
        ).status_code
        == 404
    )
    assert (
        db_client.delete(
            f"{VOICE}/{lobby_id}/chunks", headers=user_account["headers"]
        ).status_code
        == 404
    )


def test_non_member_is_403(db_client, user_account, second_user, third_user):
    lobby_id = _voice_room(db_client, user_account, second_user)

    response = db_client.post(
        f"{VOICE}/{lobby_id}/chunks",
        json={"data": _b64(b"\x00\x01")},
        headers=third_user["headers"],
    )
    assert response.status_code == 403, response.text
    assert response.json()["detail"]["code"] == "not_participant"
    assert (
        db_client.get(
            f"{VOICE}/{lobby_id}/chunks", headers=third_user["headers"]
        ).status_code
        == 403
    )
    assert (
        db_client.delete(
            f"{VOICE}/{lobby_id}/chunks", headers=third_user["headers"]
        ).status_code
        == 403
    )


def test_closed_lobby_is_404_for_both_members(db_client, user_account, second_user):
    lobby_id = _voice_room(db_client, user_account, second_user)
    assert _post(db_client, lobby_id, user_account["headers"], _pcm()).status_code == 200

    left = db_client.post(f"{ROOMS}/{lobby_id}/leave", headers=user_account["headers"])
    assert left.status_code == 200, left.text
    assert left.json()["status"] == "closed"

    for account in (user_account, second_user):
        response = db_client.get(
            f"{VOICE}/{lobby_id}/chunks", headers=account["headers"]
        )
        assert response.status_code == 404, response.text


def test_guest_leave_drops_only_their_voice(
    db_client, user_account, second_user
):
    lobby_id = _voice_room(db_client, user_account, second_user)
    assert _post(db_client, lobby_id, user_account["headers"], _pcm(marker=1)).status_code == 200
    assert _post(db_client, lobby_id, second_user["headers"], _pcm(marker=2)).status_code == 200

    assert (
        db_client.post(
            f"{ROOMS}/{lobby_id}/leave", headers=second_user["headers"]
        ).status_code
        == 200
    )
    # The host's buffer is untouched; the guest is no longer a member anyway.
    data = _poll(db_client, lobby_id, user_account["headers"])
    assert data["chunks"] == []
    assert data["next_since"] >= 1


# ---------------------------------------------------------------------------
# Upload / poll contract
# ---------------------------------------------------------------------------
def test_seq_is_lobby_wide_ordered_and_self_chunks_are_hidden(
    db_client, user_account, second_user
):
    lobby_id = _voice_room(db_client, user_account, second_user)

    first = _post(db_client, lobby_id, second_user["headers"], _pcm(marker=10))
    assert first.status_code == 200, first.text
    assert set(first.json()) == {"seq"}
    self_chunk = _post(db_client, lobby_id, user_account["headers"], _pcm(marker=20))
    second = _post(db_client, lobby_id, second_user["headers"], _pcm(marker=30))
    assert second.status_code == 200, second.text

    seqs = [first.json()["seq"], self_chunk.json()["seq"], second.json()["seq"]]
    assert seqs == sorted(seqs)
    assert len(set(seqs)) == 3

    data = _poll(db_client, lobby_id, user_account["headers"], since=0)
    assert set(data) == {"chunks", "next_since"}
    # Only the guest's two chunks, oldest first.
    assert [item["seq"] for item in data["chunks"]] == [seqs[0], seqs[2]]
    assert {item["user_id"] for item in data["chunks"]} == {second_user["id"]}
    assert {item["username"] for item in data["chunks"]} == {
        second_user["username"]
    }
    for item in data["chunks"]:
        assert set(item) == {"seq", "user_id", "username", "data"}
        assert isinstance(item["data"], str)
        assert len(base64.b64decode(item["data"])) == 320
    # ``next_since`` counts the caller's own chunk as well.
    assert data["next_since"] == seqs[2]

    # The guest only gets the host's chunk, never their own two.
    mine = _poll(db_client, lobby_id, second_user["headers"], since=0)
    assert [item["seq"] for item in mine["chunks"]] == [seqs[1]]
    assert mine["next_since"] == seqs[2]

    # Polling from the cursor returns nothing new.
    empty = _poll(db_client, lobby_id, user_account["headers"], since=data["next_since"])
    assert empty == {"chunks": [], "next_since": data["next_since"]}

    # Payload round-trip is byte exact.
    marker_chunk = _post(
        db_client, lobby_id, second_user["headers"], _pcm(size=200, marker=99)
    )
    assert marker_chunk.status_code == 200
    latest = _poll(
        db_client, lobby_id, user_account["headers"], since=data["next_since"]
    )
    assert len(latest["chunks"]) == 1
    assert base64.b64decode(latest["chunks"][0]["data"]) == _pcm(size=200, marker=99)


def test_since_filters_and_next_since_never_moves_backwards(
    db_client, user_account, second_user
):
    lobby_id = _voice_room(db_client, user_account, second_user)

    assert _post(db_client, lobby_id, second_user["headers"], _pcm()).status_code == 200
    data = _poll(db_client, lobby_id, user_account["headers"], since=0)
    cursor = data["next_since"]
    assert cursor == data["chunks"][0]["seq"]

    assert _post(db_client, lobby_id, second_user["headers"], _pcm(marker=7)).status_code == 200
    increment = _poll(db_client, lobby_id, user_account["headers"], since=cursor)
    assert len(increment["chunks"]) == 1
    assert increment["chunks"][0]["seq"] > cursor
    assert increment["next_since"] == increment["chunks"][0]["seq"]

    # A cursor from the future (e.g. a client that reloaded) is echoed back.
    ahead = _poll(db_client, lobby_id, user_account["headers"], since=10_000)
    assert ahead == {"chunks": [], "next_since": 10_000}

    # Default ``since`` is 0 and a negative one is a validation error.
    assert _poll(db_client, lobby_id, user_account["headers"])["chunks"] != []
    negative = db_client.get(
        f"{VOICE}/{lobby_id}/chunks?since=-1", headers=user_account["headers"]
    )
    assert negative.status_code == 422


def test_poll_caps_the_batch_at_twenty(db_client, user_account, second_user, monkeypatch):
    lobby_id = _voice_room(db_client, user_account, second_user)
    # Advance the relay clock per call so the flood guard is never hit and the
    # chunks stay inside the 3 s TTL while 25 of them are posted.
    monkeypatch.setattr(voice_relay, "clock", _stepping_clock(0.11))

    for _ in range(MAX_CHUNKS_PER_POLL + 5):
        assert (
            _post(db_client, lobby_id, second_user["headers"], _pcm()).status_code
            == 200
        )

    data = _poll(db_client, lobby_id, user_account["headers"], since=0)
    assert len(data["chunks"]) == MAX_CHUNKS_PER_POLL
    seqs = [item["seq"] for item in data["chunks"]]
    assert seqs == sorted(seqs)
    assert data["next_since"] == seqs[-1]


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------
def test_base64_and_size_validation(db_client, user_account, second_user):
    lobby_id = _voice_room(db_client, user_account, second_user)
    url = f"{VOICE}/{lobby_id}/chunks"
    headers = user_account["headers"]

    def post(payload):
        return db_client.post(url, json=payload, headers=headers)

    assert post({}).status_code == 422
    assert post({"data": ""}).status_code == 422
    assert post({"data": "not base64!"}).status_code == 422
    assert post({"data": _b64(b"")}).status_code == 422
    # Exactly the documented limit is accepted, one byte more is not.
    assert post({"data": _b64(_pcm(size=MAX_CHUNK_BYTES))}).status_code == 200
    too_big = post({"data": _b64(_pcm(size=MAX_CHUNK_BYTES + 1))})
    assert too_big.status_code == 422, too_big.text
    assert post({"data": "A" * 20_000}).status_code == 422
    # The documented limit matches the relay guard.
    assert PCM_NOTES_MAX == MAX_CHUNK_BYTES


def test_rate_limit_answers_429_with_the_documented_body(
    db_client, user_account, second_user, monkeypatch
):
    lobby_id = _voice_room(db_client, user_account, second_user)
    # Freeze the relay clock so the sliding window cannot refill mid-test.
    monkeypatch.setattr(voice_relay, "clock", lambda: 1_000.0)

    statuses = [
        _post(db_client, lobby_id, second_user["headers"], _pcm()).status_code
        for _ in range(MAX_POSTS_PER_SECOND + 1)
    ]
    assert statuses[:MAX_POSTS_PER_SECOND] == [200] * MAX_POSTS_PER_SECOND
    assert statuses[-1] == 429

    limited = db_client.post(
        f"{VOICE}/{lobby_id}/chunks",
        json={"data": _b64(_pcm())},
        headers=second_user["headers"],
    )
    assert limited.status_code == 429
    assert limited.json() == {"detail": "voice rate limit"}

    # The other member has their own budget.
    assert _post(db_client, lobby_id, user_account["headers"], _pcm()).status_code == 200


# ---------------------------------------------------------------------------
# DELETE
# ---------------------------------------------------------------------------
def test_delete_clears_only_the_caller_and_is_idempotent(
    db_client, user_account, second_user
):
    lobby_id = _voice_room(db_client, user_account, second_user)
    assert _post(db_client, lobby_id, user_account["headers"], _pcm(marker=1)).status_code == 200
    guest_post = _post(db_client, lobby_id, second_user["headers"], _pcm(marker=2))
    assert guest_post.status_code == 200

    first = _clear(db_client, lobby_id, user_account["headers"])
    assert first.status_code == 204
    assert first.content == b""
    assert _clear(db_client, lobby_id, user_account["headers"]).status_code == 204

    # The guest's chunk survives the host's DELETE.
    data = _poll(db_client, lobby_id, user_account["headers"], since=0)
    assert [item["seq"] for item in data["chunks"]] == [guest_post.json()["seq"]]

    assert _clear(db_client, lobby_id, second_user["headers"]).status_code == 204
    assert _poll(db_client, lobby_id, user_account["headers"], since=0)["chunks"] == []
    # An empty buffer still reports the cursor the client already has.
    assert _poll(db_client, lobby_id, user_account["headers"], since=5)["next_since"] == 5


# ---------------------------------------------------------------------------
# Registry limits (unit: no HTTP, no waiting for the real TTL)
# ---------------------------------------------------------------------------
class _Clock:
    def __init__(self, now: float = 1_000.0) -> None:
        self.now = now

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


def _stepping_clock(step: float = 0.11) -> Callable[[], float]:
    """A clock that moves forward on every call (one call per relay action)."""
    state = {"now": 1_000.0}

    def clock() -> float:
        state["now"] += step
        return state["now"]

    return clock


def test_relay_expires_chunks_after_three_seconds():
    clock = _Clock()
    relay = VoiceRelay(clock=clock)
    lobby_id = uuid.uuid4()
    speaker = uuid.uuid4()
    listener = uuid.uuid4()

    seq = relay.push(
        lobby_id=lobby_id, user_id=speaker, username="speaker", data=b"\x00\x01"
    )
    chunks, next_since = relay.poll(
        lobby_id=lobby_id, user_id=listener, since=0
    )
    assert [chunk.seq for chunk in chunks] == [seq]
    assert next_since == seq

    clock.advance(2.9)
    assert relay.poll(lobby_id=lobby_id, user_id=listener, since=0)[0] != []

    clock.advance(0.2)
    chunks, next_since = relay.poll(lobby_id=lobby_id, user_id=listener, since=0)
    assert chunks == []
    # Expiry does not move the caller's cursor backwards.
    assert next_since == 0

    clock.advance(0.1)
    newer = relay.push(
        lobby_id=lobby_id, user_id=speaker, username="speaker", data=b"\x00\x02"
    )
    assert newer > seq
    chunks, next_since = relay.poll(lobby_id=lobby_id, user_id=listener, since=seq)
    assert [chunk.seq for chunk in chunks] == [newer]
    assert next_since == newer


def test_relay_keeps_twenty_newest_chunks_per_user():
    clock = _Clock()
    relay = VoiceRelay(clock=clock)
    lobby_id = uuid.uuid4()
    speaker = uuid.uuid4()
    listener = uuid.uuid4()

    seqs = []
    for _ in range(MAX_CHUNKS_PER_USER + 5):
        seqs.append(
            relay.push(
                lobby_id=lobby_id,
                user_id=speaker,
                username="speaker",
                data=b"\x00\x01",
            )
        )
        clock.advance(0.11)  # 9 posts per second: under the flood guard

    chunks, next_since = relay.poll(lobby_id=lobby_id, user_id=listener, since=0)
    # 25 posts took 2.75 s, so every newer chunk is still inside the TTL.
    assert len(chunks) == MAX_CHUNKS_PER_USER
    assert [chunk.seq for chunk in chunks] == seqs[-MAX_CHUNKS_PER_USER:]
    assert next_since == seqs[-1]


def test_relay_caps_a_lobby_at_sixty_chunks():
    clock = _Clock()
    relay = VoiceRelay(clock=clock)
    lobby_id = uuid.uuid4()
    speakers = [uuid.uuid4() for _ in range(4)]

    # 21 posts per speaker, interleaved so each speaker stays at 9 posts per
    # second; the whole round takes 2.31 s and fits into the 3 s TTL.
    for _ in range(MAX_CHUNKS_PER_USER + 1):
        for speaker in speakers:
            relay.push(
                lobby_id=lobby_id,
                user_id=speaker,
                username="speaker",
                data=b"\x00\x01",
            )
        clock.advance(0.11)

    total = 0
    since = 0
    listener = uuid.uuid4()
    while True:
        chunks, next_since = relay.poll(
            lobby_id=lobby_id, user_id=listener, since=since
        )
        if not chunks:
            break
        total += len(chunks)
        # ``next_since`` is the highest stored seq; page with the last returned
        # one to prove the buffer really holds 60 chunks.
        since = chunks[-1].seq
        assert next_since >= since
    assert total == MAX_CHUNKS_PER_LOBBY


def test_relay_rejects_oversized_data_and_rate_limits():
    clock = _Clock()
    relay = VoiceRelay(clock=clock)
    lobby_id = uuid.uuid4()
    user_id = uuid.uuid4()

    with pytest.raises(ValueError):
        relay.push(
            lobby_id=lobby_id,
            user_id=user_id,
            username="speaker",
            data=b"\x00" * (MAX_CHUNK_BYTES + 1),
        )

    for _ in range(MAX_POSTS_PER_SECOND):
        relay.push(
            lobby_id=lobby_id, user_id=user_id, username="speaker", data=b"\x00\x01"
        )
    with pytest.raises(VoiceRateLimited):
        relay.push(
            lobby_id=lobby_id, user_id=user_id, username="speaker", data=b"\x00\x01"
        )

    # The window slides: one second later the budget is back.
    clock.advance(1.0)
    relay.push(lobby_id=lobby_id, user_id=user_id, username="speaker", data=b"\x00\x01")

    # Leaving the room drops the buffer and the flood stamps.
    relay.clear_lobby(lobby_id)
    assert relay.poll(lobby_id=lobby_id, user_id=user_id, since=0) == ([], 0)


def test_relay_clear_user_is_idempotent_and_keeps_the_other_speaker():
    relay = VoiceRelay(clock=_Clock())
    lobby_id = UUID("11111111-1111-1111-1111-111111111111")
    first = UUID("22222222-2222-2222-2222-222222222222")
    second = UUID("33333333-3333-3333-3333-333333333333")

    relay.push(lobby_id=lobby_id, user_id=first, username="a", data=b"\x00\x01")
    relay.push(lobby_id=lobby_id, user_id=second, username="b", data=b"\x00\x02")

    relay.clear_user(lobby_id, first)
    relay.clear_user(lobby_id, first)  # idempotent
    chunks, _ = relay.poll(lobby_id=lobby_id, user_id=first, since=0)
    assert [chunk.username for chunk in chunks] == ["b"]


# ---------------------------------------------------------------------------
# Direct-UDP endpoint exchange
# ---------------------------------------------------------------------------
def _endpoint(
    client,
    lobby_id: str,
    headers: dict,
    port: int = 49152,
    ip: str | None = HOST_IP,
    extra: dict | None = None,
):
    payload: dict = {"port": port}
    if extra:
        payload.update(extra)
    request_headers = dict(headers)
    if ip is not None:
        request_headers["X-Forwarded-For"] = ip
    return client.post(
        f"{VOICE}/{lobby_id}/endpoint", json=payload, headers=request_headers
    )


def test_endpoint_requires_a_token(db_client):
    lobby_id = uuid.uuid4()
    response = db_client.post(
        f"{VOICE}/{lobby_id}/endpoint", json={"port": 49152}
    )
    assert response.status_code == 401


def test_endpoint_unknown_lobby_is_404(db_client, user_account):
    response = _endpoint(db_client, uuid.uuid4(), user_account["headers"])
    assert response.status_code == 404, response.text
    assert response.json()["detail"]["code"] == "lobby_not_found"


def test_endpoint_closed_lobby_is_404_for_both_members(
    db_client, user_account, second_user
):
    lobby_id = _voice_room(db_client, user_account, second_user)
    assert _endpoint(
        db_client, lobby_id, user_account["headers"], port=50001
    ).status_code == 200

    left = db_client.post(
        f"{ROOMS}/{lobby_id}/leave", headers=user_account["headers"]
    )
    assert left.status_code == 200, left.text
    assert left.json()["status"] == "closed"

    for account in (user_account, second_user):
        response = _endpoint(db_client, lobby_id, account["headers"])
        assert response.status_code == 404, response.text
        assert response.json()["detail"]["code"] == "lobby_not_found"


def test_endpoint_non_member_is_403(
    db_client, user_account, second_user, third_user
):
    lobby_id = _voice_room(db_client, user_account, second_user)
    response = _endpoint(db_client, lobby_id, third_user["headers"])
    assert response.status_code == 403, response.text
    assert response.json()["detail"]["code"] == "not_participant"


def test_endpoint_publish_and_read_peer_for_both_members(
    db_client, user_account, second_user
):
    lobby_id = _voice_room(db_client, user_account, second_user)

    # Host first: nobody has published a peer endpoint yet.
    host = _endpoint(
        db_client, lobby_id, user_account["headers"], port=50001, ip=HOST_IP
    )
    assert host.status_code == 200, host.text
    assert host.json() == {"ip": HOST_IP, "port": 50001, "peer": None}

    # The guest sees the host's endpoint, while the host's own answer never
    # contains the caller's entry as a peer.
    guest = _endpoint(
        db_client, lobby_id, second_user["headers"], port=49152, ip=GUEST_IP
    )
    assert guest.status_code == 200, guest.text
    body = guest.json()
    assert set(body) == {"ip", "port", "peer"}
    assert body == {
        "ip": GUEST_IP,
        "port": 49152,
        "peer": {"ip": HOST_IP, "port": 50001},
    }

    # Refreshing a port replaces the stored one; the peer is still the other
    # member (never the caller).
    refreshed = _endpoint(
        db_client, lobby_id, user_account["headers"], port=50002, ip=HOST_IP
    )
    assert refreshed.json() == {
        "ip": HOST_IP,
        "port": 50002,
        "peer": {"ip": GUEST_IP, "port": 49152},
    }

    again = _endpoint(
        db_client, lobby_id, second_user["headers"], port=49153, ip=GUEST_IP
    )
    assert again.json() == {
        "ip": GUEST_IP,
        "port": 49153,
        "peer": {"ip": HOST_IP, "port": 50002},
    }


def test_endpoint_host_alone_has_no_peer(db_client, user_account):
    lobby_id = _create_room(db_client, user_account["headers"], name="Solo UDP")
    response = _endpoint(db_client, lobby_id, user_account["headers"])
    assert response.status_code == 200, response.text
    assert response.json() == {"ip": HOST_IP, "port": 49152, "peer": None}


def test_endpoint_ignores_a_client_supplied_ip(
    db_client, user_account, second_user
):
    lobby_id = _voice_room(db_client, user_account, second_user)
    # Body extras are ignored: only ``port`` is read, the address always comes
    # from the connection.
    spoofed = _endpoint(
        db_client,
        lobby_id,
        user_account["headers"],
        port=50001,
        ip=HOST_IP,
        extra={"ip": "6.6.6.6", "public_ip": "6.6.6.6", "host": "6.6.6.6"},
    )
    assert spoofed.status_code == 200, spoofed.text
    assert spoofed.json()["ip"] == HOST_IP

    # The first X-Forwarded-For entry wins: everything after the comma was
    # appended by a proxy.
    chained = _endpoint(
        db_client,
        lobby_id,
        second_user["headers"],
        port=49152,
        ip=f"{GUEST_IP}, 10.0.0.1, 172.16.0.1",
    )
    assert chained.status_code == 200, chained.text
    assert chained.json()["ip"] == GUEST_IP
    assert chained.json()["peer"] == {"ip": HOST_IP, "port": 50001}


def test_endpoint_requires_a_derived_ip(db_client, user_account, second_user):
    """No trustworthy source address means no published endpoint."""
    lobby_id = _voice_room(db_client, user_account, second_user)

    # TestClient has no routable socket peer; without X-Forwarded-For there is
    # nothing to publish, so the route refuses instead of guessing.
    missing = _endpoint(db_client, lobby_id, user_account["headers"], ip=None)
    assert missing.status_code == 400, missing.text
    assert missing.json()["detail"]["code"] == "client_ip_unavailable"

    # A forwarded private address is not trusted either (it would point the
    # opponent inside our network or at their own machine).
    private = _endpoint(
        db_client, lobby_id, user_account["headers"], ip="192.168.1.5"
    )
    assert private.status_code == 400, private.text
    assert private.json()["detail"]["code"] == "client_ip_unavailable"


def test_endpoint_rejects_invalid_port(db_client, user_account, second_user):
    lobby_id = _voice_room(db_client, user_account, second_user)
    url = f"{VOICE}/{lobby_id}/endpoint"
    headers = {**user_account["headers"], "X-Forwarded-For": HOST_IP}

    for payload in (
        {},
        {"port": None},
        {"port": 0},
        {"port": -1},
        {"port": 65536},
        {"port": 1_000_000},
        {"port": "49152"},
        {"port": 49152.5},
        {"port": True},
    ):
        response = db_client.post(url, json=payload, headers=headers)
        assert response.status_code == 422, (payload, response.text)

    for port in (1, 65535):
        accepted = db_client.post(url, json={"port": port}, headers=headers)
        assert accepted.status_code == 200, accepted.text
        assert accepted.json()["port"] == port


def test_endpoint_expires_after_thirty_seconds(
    db_client, user_account, second_user, monkeypatch
):
    clock = _Clock()
    monkeypatch.setattr(voice_relay, "clock", clock)
    lobby_id = _voice_room(db_client, user_account, second_user)

    host = _endpoint(
        db_client, lobby_id, user_account["headers"], port=50001, ip=HOST_IP
    )
    assert host.json()["peer"] is None

    # 29.9 s later the host entry is still inside the TTL.
    clock.advance(ENDPOINT_TTL_SECONDS - 0.1)
    fresh = _endpoint(
        db_client, lobby_id, second_user["headers"], port=49152, ip=GUEST_IP
    )
    assert fresh.json()["peer"] == {"ip": HOST_IP, "port": 50001}

    # The host stopped refreshing: after 30 s its entry is pruned on write, so
    # the guest gets a null peer instead of a stale socket.
    clock.advance(0.2)
    expired = _endpoint(
        db_client, lobby_id, second_user["headers"], port=49152, ip=GUEST_IP
    )
    assert expired.json()["peer"] is None

    # Publishing again makes the guest visible to the host immediately.
    clock.advance(ENDPOINT_TTL_SECONDS - 1.0)
    back = _endpoint(
        db_client, lobby_id, user_account["headers"], port=50003, ip=HOST_IP
    )
    assert back.json()["peer"] == {"ip": GUEST_IP, "port": 49152}


def test_relay_endpoints_are_cleared_with_the_member_and_the_lobby():
    clock = _Clock()
    relay = VoiceRelay(clock=clock)
    lobby_id = uuid.uuid4()
    host = uuid.uuid4()
    guest = uuid.uuid4()

    relay.publish_endpoint(
        lobby_id=lobby_id, user_id=host, ip=HOST_IP, port=50001
    )
    relay.publish_endpoint(
        lobby_id=lobby_id, user_id=guest, ip=GUEST_IP, port=49152
    )

    mine, peer = relay.endpoints(
        lobby_id=lobby_id, caller_id=guest, peer_id=host
    )
    assert (mine.ip, mine.port) == (GUEST_IP, 49152)
    assert (peer.ip, peer.port) == (HOST_IP, 50001)

    relay.clear_user(lobby_id, host)
    relay.clear_user(lobby_id, host)  # idempotent
    _, peer = relay.endpoints(lobby_id=lobby_id, caller_id=guest, peer_id=host)
    assert peer is None
    mine, _ = relay.endpoints(lobby_id=lobby_id, caller_id=guest)
    assert (mine.ip, mine.port) == (GUEST_IP, 49152)

    relay.clear_lobby(lobby_id)
    relay.clear_lobby(lobby_id)  # idempotent
    assert relay.endpoints(
        lobby_id=lobby_id, caller_id=guest, peer_id=host
    ) == (None, None)


def test_relay_endpoint_refreshes_and_validates_the_port():
    clock = _Clock()
    relay = VoiceRelay(clock=clock)
    lobby_id = uuid.uuid4()
    user_id = uuid.uuid4()

    # Reading an unknown room creates nothing.
    assert relay.endpoints(lobby_id=lobby_id, caller_id=user_id) == (None, None)

    for port in (0, -1, 65536):
        with pytest.raises(ValueError):
            relay.publish_endpoint(
                lobby_id=lobby_id, user_id=user_id, ip=HOST_IP, port=port
            )

    first = relay.publish_endpoint(
        lobby_id=lobby_id, user_id=user_id, ip=HOST_IP, port=50001
    )
    assert first.updated_at == clock.now

    clock.advance(10.0)
    second = relay.publish_endpoint(
        lobby_id=lobby_id, user_id=user_id, ip=HOST_IP, port=50002
    )
    assert second.updated_at == clock.now

    # One entry per member: the refresh replaced the previous port.
    mine, _ = relay.endpoints(lobby_id=lobby_id, caller_id=user_id)
    assert (mine.ip, mine.port) == (HOST_IP, 50002)
    assert mine.updated_at == clock.now

    # The TTL is measured from the last POST, not from the first one.
    clock.advance(ENDPOINT_TTL_SECONDS - 0.1)
    assert relay.endpoints(lobby_id=lobby_id, caller_id=user_id)[0] is not None
    clock.advance(0.1)
    assert relay.endpoints(lobby_id=lobby_id, caller_id=user_id) == (None, None)
