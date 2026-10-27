"""Internal statistics tests: activity telemetry and admin aggregates.

The suite exercises the same HTTP contract the JUCE client uses and checks the
database directly only where the API deliberately hides details (session
timestamps, launch rows).
"""
import uuid
from datetime import datetime, timedelta, timezone

import pytest
from sqlalchemy import func, select, update

from app.models.app_session import AppSession, GameLaunch
from app.models.email_auth_code import EmailAuthCode
from app.models.game_session import GameSession, Score
from app.models.user import User
from tests import helpers

pytestmark = pytest.mark.db


def _heartbeat(client, account, **overrides):
    payload = {
        "client_session_id": "client-session-1",
        "app_version": "1.0.0",
        "platform": "windows",
        **overrides,
    }
    return client.post(
        f"{helpers.API}/activity/heartbeat", json=payload, headers=account["headers"]
    )


def _launch(client, account, **overrides):
    payload = {
        "client_session_id": "client-session-1",
        "game_key": "practice",
        "mode": "practice",
        **overrides,
    }
    return client.post(
        f"{helpers.API}/activity/game-launch", json=payload, headers=account["headers"]
    )


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


# ---------------------------------------------------------------------------
# Telemetry
# ---------------------------------------------------------------------------
def test_heartbeat_requires_auth(db_client):
    response = db_client.post(
        f"{helpers.API}/activity/heartbeat",
        json={"client_session_id": "abc"},
    )
    assert response.status_code == 401


def test_heartbeat_creates_then_updates_session(db_client, db_session, user_account):
    first = _heartbeat(db_client, user_account)
    assert first.status_code == 200, first.text
    body = first.json()
    assert body["session_id"]
    assert body["started_at"]
    assert body["last_seen_at"]

    session = db_session.scalar(select(AppSession))
    assert session is not None
    assert session.client_session_id == "client-session-1"
    assert session.app_version == "1.0.0"
    assert session.platform == "windows"
    assert session.ended_at is None
    started_at = session.started_at

    second = _heartbeat(db_client, user_account)
    assert second.status_code == 200, second.text
    assert second.json()["session_id"] == body["session_id"]

    db_session.expire_all()
    assert db_session.scalar(select(func.count(AppSession.id))) == 1
    refreshed = db_session.scalar(select(AppSession))
    assert refreshed.started_at == started_at
    assert refreshed.last_seen_at >= started_at


def test_heartbeat_final_sets_ended_at(db_client, db_session, user_account):
    _heartbeat(db_client, user_account)

    final = _heartbeat(db_client, user_account, final=True)
    assert final.status_code == 200, final.text

    db_session.expire_all()
    session = db_session.scalar(select(AppSession))
    assert session.ended_at is not None
    assert session.ended_at >= session.started_at


def test_heartbeat_validates_client_session_id(db_client, user_account):
    too_long = _heartbeat(db_client, user_account, client_session_id="x" * 65)
    assert too_long.status_code == 422

    blank = _heartbeat(db_client, user_account, client_session_id="   ")
    assert blank.status_code == 422


def test_heartbeat_truncates_cosmetic_fields(db_client, db_session, user_account):
    # A long OS name or build tag must not lose the heartbeat; the values are
    # cut to the column width instead of rejecting the request.
    response = _heartbeat(
        db_client,
        user_account,
        app_version="1.0.0-rc.1+build.20261026.abcdef",
        platform="Windows 11 Pro for Workstations 25H2",
    )
    assert response.status_code == 200, response.text

    session = db_session.scalar(select(AppSession))
    assert len(session.app_version) == 32
    assert len(session.platform) == 32


def test_game_launch_stores_row_and_links_app_session(
    db_client, db_session, user_account, published_song
):
    _heartbeat(db_client, user_account)

    response = _launch(
        db_client,
        user_account,
        game_key="song",
        song_id=published_song["id"],
        mode="single",
    )
    assert response.status_code == 201, response.text
    assert response.json()["id"]

    launch = db_session.scalar(select(GameLaunch))
    assert launch is not None
    assert launch.game_key == "song"
    assert launch.mode == "single"
    assert str(launch.song_id) == published_song["id"]
    assert launch.app_session_id is not None
    assert launch.started_at is not None


def test_game_launch_tolerates_unknown_session_and_song(db_client, db_session, user_account):
    response = _launch(
        db_client,
        user_account,
        client_session_id="never-seen",
        game_key="daily_challenge",
        song_id=str(uuid.uuid4()),
    )
    assert response.status_code == 201, response.text

    launch = db_session.scalar(select(GameLaunch))
    assert launch.app_session_id is None
    assert launch.song_id is None


def test_game_launch_requires_auth_and_game_key(db_client, user_account):
    unauthorised = db_client.post(
        f"{helpers.API}/activity/game-launch", json={"game_key": "practice"}
    )
    assert unauthorised.status_code == 401

    blank = _launch(db_client, user_account, game_key="   ")
    assert blank.status_code == 422


def test_game_launch_truncates_long_game_key(db_client, db_session, user_account):
    # ``local:<slug>`` can exceed the column width; the launch is kept anyway.
    response = _launch(db_client, user_account, game_key="local:" + "a" * 130)
    assert response.status_code == 201, response.text

    launch = db_session.scalar(select(GameLaunch))
    assert len(launch.game_key) == 120


# ---------------------------------------------------------------------------
# Admin access
# ---------------------------------------------------------------------------
def test_statistics_require_admin(db_client, user_account):
    for path in ("overview", "users", "games"):
        anonymous = db_client.get(f"{helpers.API}/admin/statistics/{path}")
        assert anonymous.status_code == 401

        forbidden = db_client.get(
            f"{helpers.API}/admin/statistics/{path}", headers=user_account["headers"]
        )
        assert forbidden.status_code == 403


# ---------------------------------------------------------------------------
# Users aggregate
# ---------------------------------------------------------------------------
def _seed_user_activity(db_session, user_id: str) -> None:
    now = _utc_now()
    db_session.add_all(
        [
            # 40 minutes: closed session.
            AppSession(
                user_id=uuid.UUID(user_id),
                client_session_id="session-a",
                started_at=now - timedelta(hours=2),
                last_seen_at=now - timedelta(hours=1, minutes=20),
                ended_at=now - timedelta(hours=1, minutes=20),
                app_version="1.0.0",
                platform="windows",
            ),
            # 60 minutes: still running, counted up to last_seen_at.
            AppSession(
                user_id=uuid.UUID(user_id),
                client_session_id="session-b",
                started_at=now - timedelta(hours=5),
                last_seen_at=now - timedelta(hours=4),
            ),
            GameLaunch(
                user_id=uuid.UUID(user_id),
                game_key="song",
                mode="single",
                started_at=now - timedelta(minutes=30),
            ),
            GameLaunch(
                user_id=uuid.UUID(user_id),
                game_key="practice",
                mode="practice",
                started_at=now - timedelta(minutes=10),
            ),
        ]
    )
    db_session.commit()


def _seed_score(db_session, user_id: str, song_id: str, points: int, played_at: datetime):
    """Create one finished game session holding a single score row."""
    session = GameSession(
        user_id=uuid.UUID(user_id),
        song_id=uuid.UUID(song_id),
        song_version=1,
        speed_percent=100,
        started_at=played_at,
        finished_at=played_at,
        status="finished",
    )
    db_session.add(session)
    db_session.flush()
    db_session.add(
        Score(
            session_id=session.id,
            score_points=points,
            accuracy_percent=95.0,
            max_combo=10,
            perfect_count=8,
            good_count=1,
            miss_count=0,
        )
    )
    return session


def test_users_statistics_aggregate(db_client, db_session, admin_user):
    account = helpers.register_user(db_client, display_name="Player One")
    _seed_user_activity(db_session, account["id"])

    response = db_client.get(
        f"{helpers.API}/admin/statistics/users", headers=admin_user["headers"]
    )
    assert response.status_code == 200, response.text
    payload = response.json()

    row = next(item for item in payload["items"] if item["user_id"] == account["id"])
    assert row["username"] == account["username"]
    assert row["display_name"] == "Player One"
    assert row["email"] == account["email"]
    assert row["is_active"] is True
    # 80 min + 60 min.
    assert row["app_seconds_total"] == 100 * 60
    assert row["sessions_count"] == 2
    assert row["launches_count"] == 2
    assert row["games_distinct"] == 2
    assert row["last_seen_at"] is not None
    assert row["last_launch_at"] is not None

    # The admin has no telemetry at all and must still be listed.
    admin_row = next(item for item in payload["items"] if item["user_id"] == admin_user["id"])
    assert admin_row["app_seconds_total"] == 0
    assert admin_row["sessions_count"] == 0
    assert admin_row["last_seen_at"] is None
    assert admin_row["launches_count"] == 0
    assert admin_row["scores_count"] == 0
    assert admin_row["best_score"] is None
    assert admin_row["level"] >= 1
    assert admin_row["xp_total"] >= 0


def test_users_statistics_search_is_case_insensitive(db_client, admin_user):
    active = helpers.register_user(
        db_client, username=helpers.unique("searched"), display_name="ZZ Top"
    )
    helpers.register_user(db_client)

    by_username = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"search": active["username"].upper()[:12]},
        headers=admin_user["headers"],
    )
    assert by_username.status_code == 200, by_username.text
    usernames = [item["username"] for item in by_username.json()["items"]]
    assert active["username"] in usernames
    assert all(active["username"][:12] in name for name in usernames)

    by_display_name = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"search": "zz top"},
        headers=admin_user["headers"],
    )
    assert by_display_name.status_code == 200
    assert [item["username"] for item in by_display_name.json()["items"]] == [
        active["username"]
    ]

    by_email = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"search": active["email"].upper()},
        headers=admin_user["headers"],
    )
    assert by_email.status_code == 200
    assert [item["username"] for item in by_email.json()["items"]] == [
        active["username"]
    ]


def test_users_statistics_sorting_and_pagination(db_client, db_session, admin_user):
    busy = helpers.register_user(db_client)
    quiet = helpers.register_user(db_client)
    _seed_user_activity(db_session, busy["id"])

    by_activity = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"sort": "last_seen", "limit": 1, "offset": 0},
        headers=admin_user["headers"],
    )
    assert by_activity.status_code == 200, by_activity.text
    payload = by_activity.json()
    assert payload["limit"] == 1
    assert payload["offset"] == 0
    assert payload["total"] >= 3
    assert len(payload["items"]) == 1
    # The user with telemetry is the most recent one.
    assert payload["items"][0]["user_id"] == busy["id"]

    by_time = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"sort": "app_seconds"},
        headers=admin_user["headers"],
    )
    assert by_time.status_code == 200
    first = by_time.json()["items"][0]
    assert first["user_id"] == busy["id"]
    assert first["app_seconds_total"] == 100 * 60

    by_launches = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"sort": "launches_count"},
        headers=admin_user["headers"],
    )
    assert by_launches.status_code == 200
    assert by_launches.json()["items"][0]["user_id"] == busy["id"]

    # Users without activity still appear, at the end of the list.
    all_users = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"sort": "last_seen", "limit": 200},
        headers=admin_user["headers"],
    ).json()["items"]
    ids = [item["user_id"] for item in all_users]
    assert quiet["id"] in ids
    assert ids.index(quiet["id"]) > ids.index(busy["id"])

    invalid_sort = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"sort": "password"},
        headers=admin_user["headers"],
    )
    assert invalid_sort.status_code == 422

    invalid_order = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"sort": "username", "order": "sideways"},
        headers=admin_user["headers"],
    )
    assert invalid_order.status_code == 422


def test_users_statistics_sort_by_username_and_email(db_client, admin_user):
    token = uuid.uuid4().hex[:6]
    first = helpers.register_user(
        db_client, username=f"s{token}a", email=f"zzz-{token}@example.com"
    )
    second = helpers.register_user(
        db_client, username=f"s{token}b", email=f"aaa-{token}@example.com"
    )

    def usernames(**params):
        params.setdefault("search", token)
        params.setdefault("limit", 200)
        response = db_client.get(
            f"{helpers.API}/admin/statistics/users",
            params=params,
            headers=admin_user["headers"],
        )
        assert response.status_code == 200, response.text
        return [item["username"] for item in response.json()["items"]]

    assert usernames(sort="username", order="asc") == [
        first["username"],
        second["username"],
    ]
    assert usernames(sort="username", order="desc") == [
        second["username"],
        first["username"],
    ]
    # Email order is the reverse of the login order on purpose.
    assert usernames(sort="email", order="asc") == [
        second["username"],
        first["username"],
    ]
    assert usernames(sort="email", order="desc") == [
        first["username"],
        second["username"],
    ]
    # Text columns default to ascending when ``order`` is omitted.
    assert usernames(sort="username") == [first["username"], second["username"]]
    assert usernames(sort="email") == [second["username"], first["username"]]


def test_users_statistics_scores_and_sort_by_scores(
    db_client, db_session, admin_user, published_song
):
    token = uuid.uuid4().hex[:6]
    heavy = helpers.register_user(db_client, username=f"p{token}a")
    light = helpers.register_user(db_client, username=f"p{token}b")
    now = _utc_now()
    _seed_score(db_session, heavy["id"], published_song["id"], 100, now - timedelta(hours=3))
    _seed_score(db_session, heavy["id"], published_song["id"], 500, now - timedelta(hours=2))
    _seed_score(db_session, heavy["id"], published_song["id"], 300, now - timedelta(hours=1))
    _seed_score(db_session, light["id"], published_song["id"], 42, now - timedelta(minutes=5))
    db_session.commit()

    def rows(**params):
        params.setdefault("search", token)
        params.setdefault("limit", 200)
        response = db_client.get(
            f"{helpers.API}/admin/statistics/users",
            params=params,
            headers=admin_user["headers"],
        )
        assert response.status_code == 200, response.text
        return response.json()["items"]

    desc = rows(sort="scores_count", order="desc")
    assert [item["username"] for item in desc] == [heavy["username"], light["username"]]
    assert desc[0]["scores_count"] == 3
    assert desc[0]["best_score"] == 500
    assert desc[1]["scores_count"] == 1
    assert desc[1]["best_score"] == 42

    asc = rows(sort="scores_count", order="asc")
    assert [item["username"] for item in asc] == [light["username"], heavy["username"]]

    # Numeric columns default to descending when ``order`` is omitted.
    assert rows(sort="scores_count")[0]["username"] == heavy["username"]

    # Scores do not disturb the other aggregates: heavy has three finished
    # sessions but no app session or game launch.
    assert desc[0]["sessions_count"] == 0
    assert desc[0]["launches_count"] == 0


def test_users_statistics_sort_by_last_seen_both_directions(
    db_client, db_session, admin_user
):
    token = uuid.uuid4().hex[:6]
    recent = helpers.register_user(db_client, username=f"t{token}a")
    older = helpers.register_user(db_client, username=f"t{token}b")
    idle = helpers.register_user(db_client, username=f"t{token}c")
    now = _utc_now()
    db_session.add_all(
        [
            AppSession(
                user_id=uuid.UUID(older["id"]),
                client_session_id="older",
                started_at=now - timedelta(hours=3),
                last_seen_at=now - timedelta(hours=2),
            ),
            AppSession(
                user_id=uuid.UUID(recent["id"]),
                client_session_id="recent",
                started_at=now - timedelta(minutes=10),
                last_seen_at=now - timedelta(minutes=5),
            ),
        ]
    )
    db_session.commit()

    def usernames(**params):
        params.setdefault("search", token)
        params.setdefault("limit", 200)
        response = db_client.get(
            f"{helpers.API}/admin/statistics/users",
            params=params,
            headers=admin_user["headers"],
        )
        assert response.status_code == 200, response.text
        return [item["username"] for item in response.json()["items"]]

    assert usernames(sort="last_seen", order="desc") == [
        recent["username"],
        older["username"],
        idle["username"],
    ]
    # Empty values stay last in both directions.
    assert usernames(sort="last_seen", order="asc") == [
        older["username"],
        recent["username"],
        idle["username"],
    ]
    assert usernames(sort="last_seen") == [
        recent["username"],
        older["username"],
        idle["username"],
    ]


# ---------------------------------------------------------------------------
# Account deletion
# ---------------------------------------------------------------------------
def test_delete_user_removes_account_and_related_rows(
    db_client, db_session, admin_user, published_song
):
    account = helpers.register_user(db_client)
    user_id = uuid.UUID(account["id"])
    now = _utc_now()
    db_session.add(
        AppSession(
            user_id=user_id,
            client_session_id="delete-me",
            started_at=now - timedelta(minutes=30),
            last_seen_at=now,
        )
    )
    db_session.add(GameLaunch(user_id=user_id, game_key="practice", started_at=now))
    session = _seed_score(db_session, account["id"], published_song["id"], 250, now)
    db_session.commit()
    session_id = session.id

    response = db_client.delete(
        f"{helpers.API}/admin/users/{account['id']}", headers=admin_user["headers"]
    )
    assert response.status_code == 204, response.text
    assert response.text == ""

    db_session.expire_all()
    assert db_session.scalar(select(func.count(User.id)).where(User.id == user_id)) == 0
    assert (
        db_session.scalar(
            select(func.count(AppSession.id)).where(AppSession.user_id == user_id)
        )
        == 0
    )
    assert (
        db_session.scalar(
            select(func.count(GameLaunch.id)).where(GameLaunch.user_id == user_id)
        )
        == 0
    )
    assert (
        db_session.scalar(
            select(func.count(GameSession.id)).where(GameSession.user_id == user_id)
        )
        == 0
    )
    # The only seeded score belonged to that user's session.
    assert db_session.scalar(select(func.count(Score.id))) == 0
    assert (
        db_session.scalar(
            select(func.count(Score.id)).where(Score.session_id == session_id)
        )
        == 0
    )
    # Registration created email codes; they are cascaded too.
    assert (
        db_session.scalar(
            select(func.count(EmailAuthCode.id)).where(EmailAuthCode.user_id == user_id)
        )
        == 0
    )

    listing = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"search": account["username"]},
        headers=admin_user["headers"],
    ).json()
    assert listing["total"] == 0
    assert listing["items"] == []


def test_delete_user_rejects_self_missing_and_non_admin(
    db_client, admin_user, user_account
):
    self_delete = db_client.delete(
        f"{helpers.API}/admin/users/{admin_user['id']}", headers=admin_user["headers"]
    )
    assert self_delete.status_code == 400
    assert self_delete.json()["detail"] == "You cannot delete your own account"

    missing = db_client.delete(
        f"{helpers.API}/admin/users/{uuid.uuid4()}", headers=admin_user["headers"]
    )
    assert missing.status_code == 404

    forbidden = db_client.delete(
        f"{helpers.API}/admin/users/{user_account['id']}",
        headers=user_account["headers"],
    )
    assert forbidden.status_code == 403

    anonymous = db_client.delete(f"{helpers.API}/admin/users/{user_account['id']}")
    assert anonymous.status_code == 401

    # Rejected requests must not delete anything.
    listing = db_client.get(
        f"{helpers.API}/admin/statistics/users",
        params={"search": user_account["username"]},
        headers=admin_user["headers"],
    ).json()
    assert listing["total"] == 1
    assert listing["items"][0]["user_id"] == user_account["id"]


# ---------------------------------------------------------------------------
# Overview and games aggregates
# ---------------------------------------------------------------------------
def test_overview_counts_within_window(db_client, db_session, admin_user):
    account = helpers.register_user(db_client)
    _seed_user_activity(db_session, account["id"])
    old = _utc_now() - timedelta(days=10)
    db_session.add(
        AppSession(
            user_id=uuid.UUID(account["id"]),
            client_session_id="old-session",
            started_at=old,
            last_seen_at=old + timedelta(minutes=30),
            ended_at=old + timedelta(minutes=30),
        )
    )
    db_session.add(
        GameLaunch(
            user_id=uuid.UUID(account["id"]),
            game_key="practice",
            started_at=old,
        )
    )
    db_session.commit()

    response = db_client.get(
        f"{helpers.API}/admin/statistics/overview",
        params={"days": 30},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    payload = response.json()

    assert payload["generated_at"]
    assert payload["users_total"] == 2
    assert payload["users_active_24h"] == 1
    assert payload["users_active_7d"] == 1
    assert payload["sessions_total"] == 3
    assert payload["app_seconds_total"] == 100 * 60 + 30 * 60
    assert payload["avg_session_seconds"] == round((100 * 60 + 30 * 60) / 3)
    assert payload["launches_total"] == 3
    assert payload["launches_24h"] == 2

    narrow = db_client.get(
        f"{helpers.API}/admin/statistics/overview",
        params={"days": 7},
        headers=admin_user["headers"],
    ).json()
    assert narrow["sessions_total"] == 2
    assert narrow["launches_total"] == 2
    assert narrow["app_seconds_total"] == 100 * 60
    # users_total is not limited by the window.
    assert narrow["users_total"] == 2


def test_overview_requires_valid_days(db_client, admin_user):
    response = db_client.get(
        f"{helpers.API}/admin/statistics/overview",
        params={"days": 0},
        headers=admin_user["headers"],
    )
    assert response.status_code == 422


def test_games_statistics_group_by_game_key(
    db_client, db_session, admin_user, published_song
):
    player = helpers.register_user(db_client)
    second = helpers.register_user(db_client)
    now = _utc_now()

    db_session.add_all(
        [
            GameLaunch(
                user_id=uuid.UUID(player["id"]),
                game_key="song",
                song_id=uuid.UUID(published_song["id"]),
                mode="single",
                started_at=now - timedelta(minutes=30),
            ),
            GameLaunch(
                user_id=uuid.UUID(second["id"]),
                game_key="song",
                song_id=uuid.UUID(published_song["id"]),
                mode="single",
                started_at=now - timedelta(minutes=20),
            ),
            GameLaunch(
                user_id=uuid.UUID(player["id"]),
                game_key="song",
                song_id=None,
                mode="single",
                started_at=now - timedelta(minutes=10),
            ),
            GameLaunch(
                user_id=uuid.UUID(player["id"]),
                game_key="daily_challenge",
                started_at=now - timedelta(minutes=5),
            ),
        ]
    )
    db_session.commit()

    response = db_client.get(
        f"{helpers.API}/admin/statistics/games",
        params={"days": 30, "limit": 100},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    payload = response.json()
    assert payload["total"] == 2

    by_key = {item["game_key"]: item for item in payload["items"]}
    song = by_key["song"]
    assert song["launches"] == 3
    assert song["unique_users"] == 2
    assert song["title"] == published_song["title"]
    assert song["last_launch_at"] is not None

    challenge = by_key["daily_challenge"]
    assert challenge["launches"] == 1
    assert challenge["unique_users"] == 1
    # No song is attached, so the key itself is the title.
    assert challenge["title"] == "daily_challenge"

    narrow = db_client.get(
        f"{helpers.API}/admin/statistics/games",
        params={"days": 0},
        headers=admin_user["headers"],
    )
    assert narrow.status_code == 422


def test_games_statistics_respects_window(db_client, db_session, admin_user):
    account = helpers.register_user(db_client)
    old = _utc_now() - timedelta(days=20)
    db_session.add(
        GameLaunch(
            user_id=uuid.UUID(account["id"]),
            game_key="local:my-song",
            started_at=old,
        )
    )
    db_session.commit()

    recent = db_client.get(
        f"{helpers.API}/admin/statistics/games",
        params={"days": 7},
        headers=admin_user["headers"],
    ).json()
    assert recent["total"] == 0
    assert recent["items"] == []

    windowed = db_client.get(
        f"{helpers.API}/admin/statistics/games",
        params={"days": 30},
        headers=admin_user["headers"],
    ).json()
    assert windowed["total"] == 1
    assert windowed["items"][0]["title"] == "local:my-song"


def test_statistics_is_a_tab_in_the_admin_spa(db_client):
    # The standalone page was removed: the numbers live in the admin SPA.
    assert db_client.get("/statistics").status_code == 404

    page = db_client.get("/admin/")
    assert page.status_code == 200
    assert page.headers["content-type"].startswith("text/html")
    html = page.text
    assert 'data-admin-section="statistics"' in html
    assert 'data-admin-panel="statistics"' in html
    assert "admin-statistics.js" in html
    assert "Статистика" in html


def test_users_statistics_exposes_level_and_xp(db_client, db_session, admin_user):
    """Level and XP are returned and the column is sortable."""
    low = helpers.register_user(db_client, username=helpers.unique("lvl-low"))
    high = helpers.register_user(db_client, username=helpers.unique("lvl-high"))

    db_session.execute(
        update(User)
        .where(User.id == uuid.UUID(low["id"]))
        .values(xp_total=0, level=1)
    )
    db_session.execute(
        update(User)
        .where(User.id == uuid.UUID(high["id"]))
        .values(xp_total=5000, level=12)
    )
    db_session.commit()

    response = db_client.get(
        f"{helpers.API}/admin/statistics/users?sort=level&order=desc&limit=200",
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    items = response.json()["items"]
    assert items[0]["user_id"] == high["id"]
    assert items[0]["level"] == 12
    assert items[0]["xp_total"] == 5000
