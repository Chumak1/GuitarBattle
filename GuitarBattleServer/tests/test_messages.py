"""End-to-end tests for the private ``/api/v1/messages`` endpoints.

Every test uses the real HTTP contract against the disposable PostgreSQL
database, so the FKs, the check constraints, the DISTINCT ON conversation query
and the read marker are exercised for real. Direct inserts are used only where
a test needs exact timestamps (paging order).
"""
from __future__ import annotations

import uuid
from datetime import datetime, timedelta, timezone

import pytest
from sqlalchemy import func, select

from app.models.messages import DirectMessage
from app.models.user import User
from tests import helpers

pytestmark = pytest.mark.db

API = "/api/v1"
MESSAGES = f"{API}/messages"
SOCIAL = f"{API}/social"


def _send(client, sender: dict, recipient: dict, body: str):
    return client.post(
        f"{MESSAGES}/{recipient['id']}",
        json={"body": body},
        headers=sender["headers"],
    )


def _thread(client, viewer: dict, peer: dict, **params):
    return client.get(
        f"{MESSAGES}/{peer['id']}", params=params, headers=viewer["headers"]
    )


def _insert_message(
    db_session,
    sender: dict,
    recipient: dict,
    body: str,
    *,
    created_at: datetime | None = None,
) -> DirectMessage:
    row = DirectMessage(
        sender_id=uuid.UUID(sender["id"]),
        recipient_id=uuid.UUID(recipient["id"]),
        body=body,
    )
    if created_at is not None:
        row.created_at = created_at
    db_session.add(row)
    db_session.commit()
    db_session.refresh(row)
    return row


# ---------------------------------------------------------------------------
# Send, conversations, thread
# ---------------------------------------------------------------------------
def test_send_lists_conversation_and_thread_in_order(
    db_client, user_account, second_user
):
    first = _send(db_client, user_account, second_user, "  hi there  ")
    assert first.status_code == 201, first.text
    body = first.json()
    assert body["body"] == "hi there"
    assert body["mine"] is True
    assert body["read"] is False
    assert uuid.UUID(body["id"])

    reply = _send(db_client, second_user, user_account, "hello back")
    assert reply.status_code == 201, reply.text
    _send(db_client, user_account, second_user, "how are you?")

    # Sender: one conversation, one unread reply, newest message last.
    sender_list = db_client.get(
        f"{MESSAGES}/conversations", headers=user_account["headers"]
    )
    assert sender_list.status_code == 200, sender_list.text
    sender_body = sender_list.json()
    assert sender_body["total"] == 1
    assert sender_body["unread_total"] == 1
    conversation = sender_body["items"][0]
    assert conversation["user"]["user_id"] == second_user["id"]
    assert conversation["user"]["username"] == second_user["username"]
    assert conversation["user"]["relationship"] == "none"
    assert conversation["unread"] == 1
    assert conversation["last_message"]["body"] == "how are you?"
    assert conversation["last_message"]["mine"] is True

    # Recipient: two unread messages from the first player.
    recipient_list = db_client.get(
        f"{MESSAGES}/conversations", headers=second_user["headers"]
    )
    assert recipient_list.status_code == 200
    recipient_body = recipient_list.json()
    assert recipient_body["total"] == 1
    assert recipient_body["unread_total"] == 2
    assert recipient_body["items"][0]["user"]["user_id"] == user_account["id"]
    assert recipient_body["items"][0]["unread"] == 2
    assert recipient_body["items"][0]["last_message"]["mine"] is False
    assert recipient_body["items"][0]["last_message"]["body"] == "how are you?"

    # The thread is oldest -> newest from either side.
    # Opening it as the sender marks the inbound reply read; the sender's own
    # messages stay unread because the other player has not opened it yet.
    thread = _thread(db_client, user_account, second_user)
    assert thread.status_code == 200, thread.text
    payload = thread.json()
    assert payload["peer"]["user_id"] == second_user["id"]
    assert payload["peer"]["relationship"] == "none"
    assert payload["has_more"] is False
    assert [item["body"] for item in payload["items"]] == [
        "hi there",
        "hello back",
        "how are you?",
    ]
    assert [item["mine"] for item in payload["items"]] == [True, False, True]
    assert [item["read"] for item in payload["items"]] == [False, True, False]

    # The other side opens the same thread: its inbound messages become read,
    # and the reply it sent was already read by the first player.
    other = _thread(db_client, second_user, user_account)
    assert other.status_code == 200
    assert [item["read"] for item in other.json()["items"]] == [
        True,
        True,
        True,
    ]
    for account in (user_account, second_user):
        badge = db_client.get(f"{MESSAGES}/unread", headers=account["headers"])
        assert badge.json() == {"unread_total": 0}


def test_opening_the_thread_zeroes_its_unread(db_client, user_account, second_user):
    _send(db_client, user_account, second_user, "one")
    _send(db_client, user_account, second_user, "two")

    badge = db_client.get(f"{MESSAGES}/unread", headers=second_user["headers"])
    assert badge.status_code == 200
    assert badge.json() == {"unread_total": 2}

    opened = _thread(db_client, second_user, user_account)
    assert opened.status_code == 200
    assert all(item["read"] for item in opened.json()["items"])

    badge = db_client.get(f"{MESSAGES}/unread", headers=second_user["headers"])
    assert badge.json() == {"unread_total": 0}

    # The explicit endpoint is idempotent and reports the remaining badge.
    read = db_client.post(
        f"{MESSAGES}/{user_account['id']}/read", headers=second_user["headers"]
    )
    assert read.status_code == 200, read.text
    assert read.json() == {"unread_total": 0}

    listed = db_client.get(
        f"{MESSAGES}/conversations", headers=second_user["headers"]
    ).json()
    assert listed["unread_total"] == 0
    assert listed["items"][0]["unread"] == 0


def test_thread_paging_with_before_cursor(
    db_client, db_session, user_account, second_user
):
    start = datetime(2026, 11, 11, 9, 0, tzinfo=timezone.utc)
    for index in range(5):
        _insert_message(
            db_session,
            user_account,
            second_user,
            f"message {index}",
            created_at=start + timedelta(minutes=index),
        )

    # Opening the thread marks every message from that peer read, not only the
    # ones the returned page contained.
    newest = _thread(db_client, second_user, user_account, limit=2)
    assert newest.status_code == 200, newest.text
    page = newest.json()
    assert page["has_more"] is True
    assert [item["body"] for item in page["items"]] == [
        "message 3",
        "message 4",
    ]
    assert db_session.scalar(
        select(func.count(DirectMessage.id)).where(
            DirectMessage.read_at.is_not(None)
        )
    ) == 5

    cursor = page["items"][0]["created_at"]
    older = _thread(
        db_client, second_user, user_account, limit=2, before=cursor
    )
    assert older.status_code == 200, older.text
    older_page = older.json()
    assert older_page["has_more"] is True
    assert [item["body"] for item in older_page["items"]] == [
        "message 1",
        "message 2",
    ]


def test_conversations_are_ordered_by_newest_message(
    db_client, user_account, second_user, third_user
):
    _send(db_client, user_account, second_user, "older thread")
    _send(db_client, user_account, third_user, "newer thread")

    listed = db_client.get(
        f"{MESSAGES}/conversations", headers=user_account["headers"]
    )
    assert listed.status_code == 200
    body = listed.json()
    assert body["total"] == 2
    assert [item["user"]["user_id"] for item in body["items"]] == [
        third_user["id"],
        second_user["id"],
    ]

    paged = db_client.get(
        f"{MESSAGES}/conversations?limit=1&offset=1",
        headers=user_account["headers"],
    ).json()
    assert paged["total"] == 2
    assert [item["user"]["user_id"] for item in paged["items"]] == [
        second_user["id"]
    ]


# ---------------------------------------------------------------------------
# Validation and access rules
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("body", ["", "   ", "\n\t "])
def test_empty_body_is_rejected(db_client, user_account, second_user, body):
    response = _send(db_client, user_account, second_user, body)
    assert response.status_code == 422, response.text


def test_too_long_body_is_rejected(db_client, user_account, second_user):
    accepted = _send(db_client, user_account, second_user, "x" * 2000)
    assert accepted.status_code == 201, accepted.text
    rejected = _send(db_client, user_account, second_user, "x" * 2001)
    assert rejected.status_code == 422, rejected.text


def test_self_message_is_rejected(db_client, user_account):
    response = _send(db_client, user_account, user_account, "note to self")
    assert response.status_code == 400, response.text
    assert response.json()["detail"]["code"] == "cannot_message_self"


def test_blocked_pair_cannot_exchange_messages(
    db_client, user_account, second_user
):
    blocked = db_client.post(
        f"{SOCIAL}/blocks/{second_user['id']}", headers=user_account["headers"]
    )
    assert blocked.status_code == 204, blocked.text

    outgoing = _send(db_client, user_account, second_user, "still here?")
    assert outgoing.status_code == 403, outgoing.text
    assert outgoing.json()["detail"]["code"] == "blocked"

    incoming = _send(db_client, second_user, user_account, "let me in")
    assert incoming.status_code == 403, incoming.text

    # A block stops new messages only; the history stays readable.
    history = _thread(db_client, second_user, user_account)
    assert history.status_code == 200, history.text


def test_unknown_and_inactive_recipients_are_404(
    db_client, db_session, user_account, second_user
):
    unknown = _send(db_client, user_account, {"id": str(uuid.uuid4())}, "hello")
    assert unknown.status_code == 404, unknown.text
    assert unknown.json()["detail"]["code"] == "user_not_found"

    missing_thread = db_client.get(
        f"{MESSAGES}/{uuid.uuid4()}", headers=user_account["headers"]
    )
    assert missing_thread.status_code == 404

    db_session.execute(
        User.__table__.update()
        .where(User.id == uuid.UUID(second_user["id"]))
        .values(is_active=False)
    )
    db_session.commit()

    inactive = _send(db_client, user_account, second_user, "hello")
    assert inactive.status_code == 404, inactive.text


def test_deactivated_peer_disappears_from_conversations(
    db_client, db_session, user_account, second_user
):
    _send(db_client, user_account, second_user, "before deactivation")
    listed = db_client.get(
        f"{MESSAGES}/conversations", headers=user_account["headers"]
    ).json()
    assert listed["total"] == 1

    db_session.execute(
        User.__table__.update()
        .where(User.id == uuid.UUID(second_user["id"]))
        .values(is_active=False)
    )
    db_session.commit()

    after = db_client.get(
        f"{MESSAGES}/conversations", headers=user_account["headers"]
    ).json()
    assert after["total"] == 0
    assert after["items"] == []


# ---------------------------------------------------------------------------
# Auth and badge
# ---------------------------------------------------------------------------
def test_endpoints_require_a_bearer_token(db_client):
    assert db_client.get(f"{MESSAGES}/unread").status_code == 401
    assert db_client.get(f"{MESSAGES}/conversations").status_code == 401
    assert (
        db_client.get(f"{MESSAGES}/{uuid.uuid4()}").status_code == 401
    )
    assert (
        db_client.post(
            f"{MESSAGES}/{uuid.uuid4()}", json={"body": "hi"}
        ).status_code
        == 401
    )


def test_unread_endpoint_totals_across_conversations(
    db_client, user_account, second_user, third_user
):
    _send(db_client, second_user, user_account, "from second 1")
    _send(db_client, second_user, user_account, "from second 2")
    _send(db_client, third_user, user_account, "from third")

    badge = db_client.get(f"{MESSAGES}/unread", headers=user_account["headers"])
    assert badge.status_code == 200
    assert badge.json() == {"unread_total": 3}

    listed = db_client.get(
        f"{MESSAGES}/conversations", headers=user_account["headers"]
    ).json()
    assert listed["unread_total"] == 3
    unread_by_peer = {
        item["user"]["user_id"]: item["unread"] for item in listed["items"]
    }
    assert unread_by_peer == {second_user["id"]: 2, third_user["id"]: 1}

    read = db_client.post(
        f"{MESSAGES}/{second_user['id']}/read", headers=user_account["headers"]
    )
    assert read.json() == {"unread_total": 1}
