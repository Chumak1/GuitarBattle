"""End-to-end tests for the ``/api/v1/social`` "Friends" slice.

Every test talks to the real HTTP contract and the disposable PostgreSQL
database, so the partial unique indexes, the canonical friendship pair and the
foreign keys are exercised for real. Direct inserts are used only where the
feature under test depends on data another module would write (duel heartbeats,
historical results, activity rows).
"""
from __future__ import annotations

import uuid
from datetime import datetime, timedelta, timezone

import pytest
from sqlalchemy import func, select

from app.models.duel import DuelMatch, DuelParticipant
from app.models.social import (
    FriendRequest,
    Friendship,
    SocialNotification,
    UserRival,
)
from app.models.user import User
from app.services import social_service
from tests import helpers

pytestmark = pytest.mark.db

API = "/api/v1"
SOCIAL = f"{API}/social"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _url(path: str) -> str:
    return f"{SOCIAL}{path}"


def _send_request(client, sender: dict, target: dict) -> dict:
    response = client.post(
        _url(f"/friends/{target['id']}"), headers=sender["headers"]
    )
    assert response.status_code == 201, response.text
    return response.json()


def _befriend(client, first: dict, second: dict) -> None:
    request = _send_request(client, first, second)
    accepted = client.post(
        _url(f"/requests/{request['id']}/accept"), headers=second["headers"]
    )
    assert accepted.status_code == 200, accepted.text


def _count(db_session, model) -> int:
    return int(db_session.scalar(select(func.count()).select_from(model)) or 0)


def _friend_rows(db_session) -> list[Friendship]:
    db_session.expire_all()
    return list(db_session.scalars(select(Friendship)).all())


def _create_duel(
    db_session,
    song_id: str,
    *,
    status: str,
    participants: list[dict],
) -> DuelMatch:
    """Insert one duel match and its sides (the social layer only reads them)."""
    match = DuelMatch(
        song_id=uuid.UUID(song_id),
        mode="online",
        status=status,
        rules_version=2,
        seed=1,
        note_count=0,
    )
    db_session.add(match)
    db_session.flush()
    for participant in participants:
        db_session.add(
            DuelParticipant(
                match_id=match.id,
                user_id=uuid.UUID(participant["user_id"]),
                side=participant.get("side", "player"),
                joined_at=datetime.now(timezone.utc),
                **{
                    key: value
                    for key, value in participant.items()
                    if key not in ("user_id", "side")
                },
            )
        )
    db_session.commit()
    db_session.expire_all()
    return match


# ---------------------------------------------------------------------------
# Requests and friendships
# ---------------------------------------------------------------------------
def test_request_accept_creates_one_mutual_friendship(
    db_client, db_session, user_account, second_user
):
    request = _send_request(db_client, user_account, second_user)
    assert request["status"] == "pending"
    assert request["direction"] == "outgoing"
    assert request["from_user_id"] == user_account["id"]
    assert request["to_user_id"] == second_user["id"]
    assert request["user"]["user_id"] == second_user["id"]

    incoming = db_client.get(
        _url("/requests?direction=incoming"), headers=second_user["headers"]
    )
    assert incoming.status_code == 200
    assert incoming.json()["total"] == 1
    assert incoming.json()["items"][0]["id"] == request["id"]
    assert incoming.json()["items"][0]["direction"] == "incoming"

    accepted = db_client.post(
        _url(f"/requests/{request['id']}/accept"),
        headers=second_user["headers"],
    )
    assert accepted.status_code == 200, accepted.text
    assert accepted.json()["status"] == "accepted"
    assert accepted.json()["user"]["user_id"] == user_account["id"]

    # Exactly one canonical row, never two.
    rows = _friend_rows(db_session)
    assert len(rows) == 1
    assert rows[0].user_id < rows[0].friend_id
    assert {str(rows[0].user_id), str(rows[0].friend_id)} == {
        user_account["id"],
        second_user["id"],
    }

    for account, friend in (
        (user_account, second_user),
        (second_user, user_account),
    ):
        friends = db_client.get(
            _url("/friends"), headers=account["headers"]
        )
        assert friends.status_code == 200
        body = friends.json()
        assert body["total"] == 1
        assert body["items"][0]["user_id"] == friend["id"]
        assert body["items"][0]["relationship"] == "friend"

    # Accepting twice is idempotent and still one friendship.
    again = db_client.post(
        _url(f"/requests/{request['id']}/accept"),
        headers=second_user["headers"],
    )
    assert again.status_code == 200
    assert len(_friend_rows(db_session)) == 1


def test_duplicate_and_reverse_requests_are_rejected(
    db_client, user_account, second_user
):
    _send_request(db_client, user_account, second_user)

    duplicate = db_client.post(
        _url(f"/friends/{second_user['id']}"), headers=user_account["headers"]
    )
    assert duplicate.status_code == 409
    assert duplicate.json()["detail"]["code"] == "request_pending"

    reverse = db_client.post(
        _url(f"/friends/{user_account['id']}"), headers=second_user["headers"]
    )
    assert reverse.status_code == 409
    assert reverse.json()["detail"]["code"] == "request_pending"

    # Self-targeting is a validation error, not a conflict.
    self_request = db_client.post(
        _url(f"/friends/{user_account['id']}"), headers=user_account["headers"]
    )
    assert self_request.status_code == 422


def test_only_the_addressee_can_accept_or_decline(
    db_client, user_account, second_user, third_user
):
    request = _send_request(db_client, user_account, second_user)

    by_sender = db_client.post(
        _url(f"/requests/{request['id']}/accept"),
        headers=user_account["headers"],
    )
    assert by_sender.status_code == 403
    assert by_sender.json()["detail"]["code"] == "not_addressee"

    by_stranger = db_client.post(
        _url(f"/requests/{request['id']}/accept"),
        headers=third_user["headers"],
    )
    assert by_stranger.status_code == 403

    declined = db_client.post(
        _url(f"/requests/{request['id']}/decline"),
        headers=second_user["headers"],
    )
    assert declined.status_code == 200
    assert declined.json()["status"] == "declined"

    # Declining twice is idempotent.
    again = db_client.post(
        _url(f"/requests/{request['id']}/decline"),
        headers=second_user["headers"],
    )
    assert again.status_code == 200
    assert again.json()["status"] == "declined"


def test_sender_cancels_request(db_client, user_account, second_user):
    request = _send_request(db_client, user_account, second_user)

    by_addressee = db_client.delete(
        _url(f"/requests/{request['id']}"), headers=second_user["headers"]
    )
    assert by_addressee.status_code == 403
    assert by_addressee.json()["detail"]["code"] == "not_sender"

    cancelled = db_client.delete(
        _url(f"/requests/{request['id']}"), headers=user_account["headers"]
    )
    assert cancelled.status_code == 204

    # Idempotent retry.
    again = db_client.delete(
        _url(f"/requests/{request['id']}"), headers=user_account["headers"]
    )
    assert again.status_code == 204

    incoming = db_client.get(
        _url("/requests?direction=incoming"), headers=second_user["headers"]
    )
    assert incoming.json()["total"] == 0


# ---------------------------------------------------------------------------
# Removing and blocking
# ---------------------------------------------------------------------------
def test_remove_friendship_from_either_side(
    db_client, db_session, user_account, second_user
):
    _befriend(db_client, user_account, second_user)

    removed = db_client.delete(
        _url(f"/friends/{user_account['id']}"), headers=second_user["headers"]
    )
    assert removed.status_code == 204
    assert _count(db_session, Friendship) == 0

    # DELETE is idempotent.
    again = db_client.delete(
        _url(f"/friends/{user_account['id']}"), headers=second_user["headers"]
    )
    assert again.status_code == 204

    self_remove = db_client.delete(
        _url(f"/friends/{user_account['id']}"), headers=user_account["headers"]
    )
    assert self_remove.status_code == 422


def test_block_removes_friendship_and_blocks_both_directions(
    db_client, db_session, user_account, second_user
):
    _befriend(db_client, user_account, second_user)

    blocked = db_client.post(
        _url(f"/blocks/{second_user['id']}"), headers=user_account["headers"]
    )
    assert blocked.status_code == 204
    assert _count(db_session, Friendship) == 0

    # Both directions of a new request are refused.
    outgoing = db_client.post(
        _url(f"/friends/{second_user['id']}"), headers=user_account["headers"]
    )
    assert outgoing.status_code == 409
    assert outgoing.json()["detail"]["code"] == "blocked"

    incoming = db_client.post(
        _url(f"/friends/{user_account['id']}"), headers=second_user["headers"]
    )
    assert incoming.status_code == 409
    assert incoming.json()["detail"]["code"] == "blocked"

    # Blocked players are not searchable for each other.
    search = db_client.get(
        _url(f"/search?q={second_user['username']}"),
        headers=user_account["headers"],
    )
    assert search.status_code == 200
    assert search.json()["total"] == 0

    # The card reports the block and refuses challenges.
    card = db_client.get(
        _url(f"/players/{second_user['id']}"), headers=user_account["headers"]
    )
    assert card.status_code == 200
    assert card.json()["relationship"] == "blocked"
    assert card.json()["can_challenge"] is False
    assert card.json()["stats"] is None

    unblocked = db_client.delete(
        _url(f"/blocks/{second_user['id']}"), headers=user_account["headers"]
    )
    assert unblocked.status_code == 204
    # Unblocking is idempotent and does not restore the friendship.
    db_client.delete(
        _url(f"/blocks/{second_user['id']}"), headers=user_account["headers"]
    )
    assert _count(db_session, Friendship) == 0

    card = db_client.get(
        _url(f"/players/{second_user['id']}"), headers=user_account["headers"]
    )
    assert card.json()["relationship"] == "none"
    assert card.json()["can_challenge"] is True

    self_block = db_client.post(
        _url(f"/blocks/{user_account['id']}"), headers=user_account["headers"]
    )
    assert self_block.status_code == 422


def test_block_cancels_pending_requests(
    db_client, db_session, user_account, second_user
):
    request = _send_request(db_client, user_account, second_user)
    db_client.post(
        _url(f"/blocks/{second_user['id']}"), headers=user_account["headers"]
    )
    db_session.expire_all()
    stored = db_session.get(FriendRequest, uuid.UUID(request["id"]))
    assert stored is not None
    assert stored.status == "cancelled"
    assert stored.responded_at is not None


# ---------------------------------------------------------------------------
# Privacy
# ---------------------------------------------------------------------------
def test_request_policy_nobody_rejects_requests(
    db_client, user_account, second_user
):
    patched = db_client.patch(
        _url("/privacy"),
        json={"request_policy": "nobody"},
        headers=second_user["headers"],
    )
    assert patched.status_code == 200
    assert patched.json()["request_policy"] == "nobody"

    rejected = db_client.post(
        _url(f"/friends/{second_user['id']}"), headers=user_account["headers"]
    )
    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == "requests_disabled"


def test_show_online_false_hides_presence(
    db_client, user_account, second_user
):
    settings = db_client.get(_url("/privacy"), headers=second_user["headers"])
    assert settings.status_code == 200
    assert settings.json()["show_online"] is True

    db_client.patch(
        _url("/privacy"),
        json={"show_online": False},
        headers=second_user["headers"],
    )

    other_view = db_client.get(
        _url(f"/players/{second_user['id']}"), headers=user_account["headers"]
    )
    assert other_view.status_code == 200
    assert other_view.json()["presence"] == "hidden"

    own_view = db_client.get(
        _url(f"/players/{second_user['id']}"), headers=second_user["headers"]
    )
    assert own_view.json()["presence"] == "offline"


def test_show_stats_false_hides_stats(
    db_client, user_account, second_user
):
    db_client.patch(
        _url("/privacy"),
        json={"show_stats": False},
        headers=second_user["headers"],
    )

    card = db_client.get(
        _url(f"/players/{second_user['id']}"), headers=user_account["headers"]
    )
    assert card.status_code == 200
    assert card.json()["stats"] is None

    hidden = db_client.get(
        _url(f"/players/{second_user['id']}/stats"),
        headers=user_account["headers"],
    )
    assert hidden.status_code == 403
    assert hidden.json()["detail"]["code"] == "stats_hidden"

    own = db_client.get(
        _url(f"/players/{second_user['id']}/stats"),
        headers=second_user["headers"],
    )
    assert own.status_code == 200
    assert own.json()["achievements_total"] >= 0


def test_show_profile_false_hides_card_from_non_friends(
    db_client, user_account, second_user
):
    db_client.patch(
        _url("/privacy"),
        json={"show_profile": False},
        headers=second_user["headers"],
    )
    hidden = db_client.get(
        _url(f"/players/{second_user['id']}"), headers=user_account["headers"]
    )
    assert hidden.status_code == 403
    assert hidden.json()["detail"]["code"] == "profile_hidden"

    _befriend(db_client, user_account, second_user)
    visible = db_client.get(
        _url(f"/players/{second_user['id']}"), headers=user_account["headers"]
    )
    assert visible.status_code == 200
    assert visible.json()["relationship"] == "friend"


# ---------------------------------------------------------------------------
# Presence
# ---------------------------------------------------------------------------
def test_presence_is_derived_from_duel_data(
    db_client, db_session, user_account, second_user, published_song
):
    match = _create_duel(
        db_session,
        published_song["id"],
        status="waiting",
        participants=[{"user_id": second_user["id"], "side": "player"}],
    )

    def presence_of() -> str:
        response = db_client.get(
            _url(f"/players/{second_user['id']}"),
            headers=user_account["headers"],
        )
        assert response.status_code == 200
        return response.json()["presence"]

    assert presence_of() == "in_lobby"

    db_session.execute(
        DuelMatch.__table__.update()
        .where(DuelMatch.id == match.id)
        .values(status="playing")
    )
    db_session.commit()
    db_session.expire_all()
    assert presence_of() == "in_match"

    db_session.execute(
        DuelMatch.__table__.update()
        .where(DuelMatch.id == match.id)
        .values(status="finished")
    )
    participant = db_session.scalar(
        select(DuelParticipant).where(DuelParticipant.match_id == match.id)
    )
    participant.last_seen_at = datetime.now(timezone.utc)
    db_session.commit()
    db_session.expire_all()
    assert presence_of() == "online"

    participant.last_seen_at = datetime.now(timezone.utc) - timedelta(minutes=10)
    db_session.commit()
    db_session.expire_all()
    assert presence_of() == "offline"


# ---------------------------------------------------------------------------
# Leaderboard
# ---------------------------------------------------------------------------
def test_leaderboard_contains_only_friends_and_self(
    db_client, db_session, user_account, second_user, third_user, published_song
):
    _befriend(db_client, user_account, second_user)
    # ``third_user`` is deliberately not a friend.

    db_session.execute(
        User.__table__.update()
        .where(User.id == uuid.UUID(second_user["id"]))
        .values(level=7, xp_total=900)
    )
    db_session.commit()

    _create_duel(
        db_session,
        published_song["id"],
        status="finished",
        participants=[
            {
                "user_id": second_user["id"],
                "side": "player",
                "result": "win",
                "rating_before": 1400,
                "rating_after": 1450,
                "notes_played": 10,
                "perfect_count": 10,
            },
            {
                "user_id": user_account["id"],
                "side": "opponent",
                "result": "loss",
                "rating_before": 1000,
                "rating_after": 980,
                "notes_played": 10,
                "miss_count": 10,
            },
        ],
    )

    allowed = {user_account["id"], second_user["id"]}
    for sort in ("rating", "level", "wins", "win_rate"):
        response = db_client.get(
            _url(f"/leaderboard?sort={sort}&limit=10"),
            headers=user_account["headers"],
        )
        assert response.status_code == 200, response.text
        body = response.json()
        assert body["sort"] == sort
        ids = {item["user_id"] for item in body["items"]}
        assert ids == allowed, sort
        assert third_user["id"] not in ids
        assert body["items"][0]["user_id"] == second_user["id"], sort
        assert body["items"][0]["rank"] == 1

    rating_board = db_client.get(
        _url("/leaderboard?sort=rating"), headers=user_account["headers"]
    ).json()["items"]
    assert rating_board[0]["rating"] == 1450
    self_row = next(row for row in rating_board if row["is_self"])
    assert self_row["user_id"] == user_account["id"]

    wins_board = db_client.get(
        _url("/leaderboard?sort=wins"), headers=user_account["headers"]
    ).json()["items"]
    assert wins_board[0]["wins"] == 1
    assert wins_board[0]["win_rate"] == 100.0

    level_board = db_client.get(
        _url("/leaderboard?sort=level"), headers=user_account["headers"]
    ).json()["items"]
    assert level_board[0]["level"] == 7


# ---------------------------------------------------------------------------
# Rival
# ---------------------------------------------------------------------------
def test_rival_must_be_a_friend_and_only_one_exists(
    db_client, db_session, user_account, second_user, third_user
):
    empty = db_client.get(_url("/rival"), headers=user_account["headers"])
    assert empty.status_code == 200
    assert empty.json()["rival"] is None

    not_friend = db_client.put(
        _url("/rival"),
        json={"user_id": second_user["id"]},
        headers=user_account["headers"],
    )
    assert not_friend.status_code == 409
    assert not_friend.json()["detail"]["code"] == "rival_not_friend"

    _befriend(db_client, user_account, second_user)
    _befriend(db_client, user_account, third_user)

    first = db_client.put(
        _url("/rival"),
        json={"user_id": second_user["id"]},
        headers=user_account["headers"],
    )
    assert first.status_code == 200, first.text
    assert first.json()["user"]["user_id"] == second_user["id"]

    # A second PUT replaces the single rival instead of adding another row.
    second = db_client.put(
        _url("/rival"),
        json={"user_id": third_user["id"]},
        headers=user_account["headers"],
    )
    assert second.status_code == 200
    assert second.json()["user"]["user_id"] == third_user["id"]
    assert _count(db_session, UserRival) == 1

    current = db_client.get(_url("/rival"), headers=user_account["headers"])
    assert current.json()["rival"]["user"]["user_id"] == third_user["id"]

    cleared = db_client.delete(_url("/rival"), headers=user_account["headers"])
    assert cleared.status_code == 204
    assert _count(db_session, UserRival) == 0
    assert (
        db_client.get(_url("/rival"), headers=user_account["headers"]).json()[
            "rival"
        ]
        is None
    )

    self_rival = db_client.put(
        _url("/rival"),
        json={"user_id": user_account["id"]},
        headers=user_account["headers"],
    )
    assert self_rival.status_code == 422


# ---------------------------------------------------------------------------
# Notifications
# ---------------------------------------------------------------------------
def test_notifications_are_delivered_and_deduped(
    db_client, db_session, user_account, second_user
):
    request = _send_request(db_client, user_account, second_user)
    assert _count(db_session, SocialNotification) == 1

    inbox = db_client.get(
        _url("/notifications"), headers=second_user["headers"]
    )
    assert inbox.status_code == 200
    body = inbox.json()
    assert body["total"] == 1
    assert body["unread"] == 1
    assert body["items"][0]["kind"] == "friend_request"
    assert body["items"][0]["payload"]["request_id"] == request["id"]

    db_client.post(
        _url(f"/requests/{request['id']}/accept"),
        headers=second_user["headers"],
    )
    # Accepting twice must not deliver a second notification to the sender.
    db_client.post(
        _url(f"/requests/{request['id']}/accept"),
        headers=second_user["headers"],
    )
    sender_inbox = db_client.get(
        _url("/notifications?unread_only=true"), headers=user_account["headers"]
    ).json()
    assert sender_inbox["total"] == 1
    assert sender_inbox["items"][0]["kind"] == "friend_accepted"

    # The dedupe key itself is unique per account: a direct retry is a no-op.
    inserted = social_service.push_notification(
        db_session,
        uuid.UUID(user_account["id"]),
        "friend_accepted",
        {"request_id": request["id"]},
        f"friend_accepted:{request['id']}",
    )
    db_session.commit()
    assert inserted is False

    target = db_session.get(User, uuid.UUID(second_user["id"]))
    db_session.refresh(target)
    notification = db_session.scalar(
        select(SocialNotification).where(
            SocialNotification.user_id == target.id
        )
    )
    read = db_client.post(
        _url("/notifications/read"),
        json={"ids": [str(notification.id)]},
        headers=second_user["headers"],
    )
    assert read.status_code == 200
    assert read.json()["updated"] == 1
    unread = db_client.get(
        _url("/notifications?unread_only=true"),
        headers=second_user["headers"],
    ).json()
    assert unread["total"] == 0
    assert unread["unread"] == 0


# ---------------------------------------------------------------------------
# Search, activity and authentication
# ---------------------------------------------------------------------------
def test_search_returns_compact_cards_excluding_self(
    db_client, user_account, second_user
):
    response = db_client.get(
        _url(f"/search?q={second_user['username']}"),
        headers=user_account["headers"],
    )
    assert response.status_code == 200
    body = response.json()
    assert body["total"] == 1
    card = body["items"][0]
    assert card["user_id"] == second_user["id"]
    assert card["username"] == second_user["username"]
    assert card["relationship"] == "none"
    assert "email" not in card

    own = db_client.get(
        _url(f"/search?q={user_account['username']}"),
        headers=user_account["headers"],
    )
    assert own.json()["total"] == 0


def test_activity_feed_respects_show_activity(
    db_client, db_session, user_account, second_user
):
    _befriend(db_client, user_account, second_user)

    activity = social_service.record_activity(
        db_session,
        user_id=uuid.UUID(second_user["id"]),
        kind="record",
        payload={"song_id": str(uuid.uuid4()), "score": 1234},
    )
    db_session.commit()
    assert activity.id is not None

    feed = db_client.get(_url("/activity"), headers=user_account["headers"])
    assert feed.status_code == 200
    body = feed.json()
    assert body["total"] == 1
    assert body["items"][0]["kind"] == "record"
    assert body["items"][0]["payload"]["score"] == 1234
    assert body["items"][0]["user"]["user_id"] == second_user["id"]

    db_client.patch(
        _url("/privacy"),
        json={"show_activity": False},
        headers=second_user["headers"],
    )
    hidden = db_client.get(_url("/activity"), headers=user_account["headers"])
    assert hidden.status_code == 200
    assert hidden.json()["total"] == 0

    own = db_client.get(_url("/activity"), headers=second_user["headers"])
    assert own.json()["total"] == 0


def test_social_endpoints_require_authentication(db_client):
    user_id = uuid.uuid4()
    for path in (
        f"/players/{user_id}",
        f"/players/{user_id}/stats",
        "/search?q=user",
        "/friends",
        "/requests",
        "/leaderboard",
        "/privacy",
        "/rival",
        "/activity",
        "/notifications",
    ):
        response = db_client.get(_url(path))
        assert response.status_code == 401, path
        assert response.headers.get("www-authenticate") == "Bearer"

    anonymous = helpers.auth_headers("not-a-token")
    response = db_client.get(_url(f"/players/{user_id}"), headers=anonymous)
    assert response.status_code == 401
