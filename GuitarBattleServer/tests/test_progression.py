"""Progression, XP, achievements and daily challenge tests.

Two kinds of tests live here:

* pure formula tests (``pytest.mark.formula``, no database) that pin the maths
  documented in README "Progression";
* HTTP tests against the disposable database that exercise the same contract
  the JUCE client uses.
"""
from __future__ import annotations

import uuid
from datetime import datetime, timedelta, timezone

import pytest

from tests import helpers

SCORE = {
    "score_points": 1000,
    "accuracy_percent": 90.0,
    "max_combo": 20,
    "perfect_count": 18,
    "good_count": 2,
    "miss_count": 1,
}


# ---------------------------------------------------------------------------
# Pure formula tests (no database)
# ---------------------------------------------------------------------------
def test_level_curve_matches_the_formula():
    from app.services import progression as prog

    assert prog.xp_required(1) == 50
    assert prog.xp_required(2) == 141  # round(50 * 2 ** 1.5) = 141.42
    assert prog.xp_required(3) == 260  # round(50 * 3 ** 1.5) = 259.81
    assert prog.level_for_xp(0) == 1
    assert prog.level_for_xp(49) == 1
    assert prog.level_for_xp(50) == 2
    assert prog.level_for_xp(190) == 2
    assert prog.level_for_xp(191) == 3
    assert prog.level_for_xp(10**9) > 100


def test_level_progress_through_several_levels():
    from app.services import progression as prog

    level3_start = prog.xp_required(1) + prog.xp_required(2)
    info = prog.level_progress(level3_start + 10)
    assert info["level"] == 3
    assert info["xp_into_level"] == 10
    assert info["xp_for_next"] == prog.xp_required(3)
    assert info["xp_remaining"] == prog.xp_required(3) - 10
    assert prog.xp_to_next_level(level3_start + 10) == info["xp_remaining"]
    # A level boundary is inclusive: exactly the required XP reaches the level.
    assert prog.level_progress(level3_start)["level"] == 3
    assert prog.level_progress(level3_start)["xp_into_level"] == 0


def test_difficulty_factors_are_capped():
    from app.services import progression as prog

    assert prog.difficulty_factor(1) == 0.8
    assert prog.difficulty_factor(5) == 1.5
    assert prog.difficulty_factor(6) == 1.55
    assert prog.difficulty_factor(99) == 1.8
    assert prog.difficulty_factor(0) == 0.6


def test_tempo_factor_is_bounded():
    from app.services import progression as prog

    assert prog.tempo_factor(0) == 0.90
    assert prog.tempo_factor(60) == 0.90
    assert prog.tempo_factor(200) == 1.15
    assert prog.tempo_factor(10_000) == 1.15


def test_repeat_factor_slots():
    from app.services import progression as prog

    assert [prog.repeat_factor(index) for index in range(5)] == [
        1.0,
        0.5,
        0.25,
        0.0,
        0.0,
    ]


def test_completion_is_clamped_by_note_counters():
    from app.services import progression as prog

    assert prog.effective_completion(100, 100, 50) == 50
    assert prog.effective_completion(100, 100, 500) == 100
    assert prog.effective_completion(120) == 100
    assert prog.effective_completion(-5) == 0
    assert prog.effective_completion(80, None, 10) == 80
    assert prog.effective_completion(80, 0, 10) == 80


def test_attempt_xp_is_proportional_to_accuracy():
    from app.services import progression as prog

    perfect = prog.xp_for_attempt(
        accuracy_percent=100,
        difficulty=3,
        bpm=120,
        duration_seconds=90,
        completion_percent=100,
    )
    sixty = prog.xp_for_attempt(
        accuracy_percent=60,
        difficulty=3,
        bpm=120,
        duration_seconds=90,
        completion_percent=100,
    )
    assert perfect == 123
    assert sixty == 74
    assert sixty / perfect == pytest.approx(0.6, abs=0.03)


def test_attempt_xp_needs_half_the_song():
    from app.services import progression as prog

    assert (
        prog.xp_for_attempt(
            accuracy_percent=100,
            difficulty=5,
            bpm=120,
            duration_seconds=90,
            completion_percent=49.9,
        )
        == 0
    )


def test_mastery_tiers_and_next_threshold():
    from app.services import progression as prog

    assert prog.mastery_tier(0) == ("Novice", 60.0)
    assert prog.mastery_tier(59.9) == ("Novice", 60.0)
    assert prog.mastery_tier(60) == ("Learner", 75.0)
    assert prog.mastery_tier(75) == ("Guitarist", 87.0)
    assert prog.mastery_tier(87) == ("Pro", 95.0)
    assert prog.mastery_tier(95) == ("Master", None)
    # Difficulty adds two points of mastery per level above 1.
    assert prog.mastery_value(90, 3) == 94
    assert prog.mastery_value(100, 5) == 100


# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------
def _create_song(db_client, admin_user, **overrides):
    song = helpers.create_song(db_client, admin_user["headers"], **overrides)
    updated = helpers.publish_song(db_client, admin_user["headers"], song["id"])
    return {**song, **updated}


def _start(client, headers, song_id, *, daily=False, speed=100):
    response = client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": song_id, "speed_percent": speed, "daily": daily},
        headers=headers,
    )
    return response


def _finish(client, headers, session_id, **overrides):
    payload = {**SCORE, **overrides}
    return client.post(
        f"{helpers.API}/games/sessions/{session_id}/finish",
        json=payload,
        headers=headers,
    )


def _play(client, headers, song_id, **overrides):
    session = _start(client, headers, song_id)
    assert session.status_code == 201, session.text
    response = _finish(client, headers, session.json()["id"], **overrides)
    assert response.status_code == 200, response.text
    return response.json()


def _profile(client, headers):
    response = client.get(f"{helpers.API}/profile/progression", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _achievements(client, headers):
    response = client.get(f"{helpers.API}/achievements", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def _codes(payload):
    return [item["code"] for item in payload["achievements_awarded"]]


# ---------------------------------------------------------------------------
# Attempt XP through the API
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_first_attempt_awards_xp_and_profile(db_client, user_account, published_song):
    from app.services import progression as prog

    body = _play(db_client, user_account["headers"], published_song["id"])

    assert body["xp_earned"] > 0
    assert body["xp_total"] >= body["xp_earned"]
    assert body["level"] == prog.level_for_xp(body["xp_total"])
    assert body["level_up"] is (body["level"] > 1)
    assert body["xp_to_next"] == body["xp_remaining"]
    assert body["personal_best_updated"] is True
    assert body["mastery"]["tier"] == "Pro"  # 90 accuracy + 2 * (3 - 1) = 94
    assert body["completion_percent"] == 100

    profile = _profile(db_client, user_account["headers"])
    assert profile["xp_total"] == body["xp_total"]
    assert profile["songs_mastered"] == 1
    assert profile["personal_bests"][0]["best_score"] == SCORE["score_points"]
    assert profile["stats"]["samples"] == 1
    assert profile["stats"]["accuracy"] == pytest.approx(SCORE["accuracy_percent"])


@pytest.mark.db
def test_completion_below_half_gives_no_attempt_xp(
    db_client, user_account, published_song
):
    body = _play(
        db_client,
        user_account["headers"],
        published_song["id"],
        completion_percent=30,
        accuracy_percent=100,
    )
    assert body["xp_earned"] == 0
    assert body["completion_percent"] == 30
    # The attempt still counts as a personal best, but no mastery achievement
    # may be handed out for an unfinished run.
    assert body["personal_best_updated"] is True
    assert "mastery_5" not in _codes(body)
    assert "first_notes" not in _codes(body)


@pytest.mark.db
def test_xp_uses_note_counters_to_limit_completion(
    db_client, user_account, admin_user
):
    long_song = _create_song(
        db_client, admin_user, duration_seconds=600, title="Long Song"
    )
    # A 10-minute song is worth at most 1.4x content, and the client cannot
    # claim more than the notes it played.
    body = _play(
        db_client,
        user_account["headers"],
        long_song["id"],
        notes_total=1000,
        notes_played=100,
        completion_percent=100,
    )
    assert body["completion_percent"] == pytest.approx(10.0)
    assert body["xp_earned"] == 0

    full = _play(
        db_client,
        user_account["headers"],
        long_song["id"],
        notes_total=1000,
        notes_played=1000,
    )
    assert full["completion_percent"] == 100
    from app.services import progression as prog

    assert full["xp_earned"] == prog.xp_for_attempt(
        accuracy_percent=SCORE["accuracy_percent"],
        difficulty=long_song["difficulty"],
        bpm=long_song["bpm"],
        duration_seconds=long_song["duration_seconds"],
        completion_percent=100,
        repeat_multiplier=0.5,
    )


@pytest.mark.db
def test_note_counters_cannot_exceed_each_other(db_client, user_account, published_song):
    session = _start(db_client, user_account["headers"], published_song["id"]).json()
    response = _finish(
        db_client,
        user_account["headers"],
        session["id"],
        notes_total=10,
        notes_played=11,
    )
    assert response.status_code == 422


@pytest.mark.db
def test_harder_song_gives_more_xp_than_an_easy_one(
    db_client, user_account, admin_user
):
    easy = _create_song(
        db_client, admin_user, title="Easy", difficulty=1, duration_seconds=90
    )
    hard = _create_song(
        db_client, admin_user, title="Hard", difficulty=5, duration_seconds=90
    )
    easy_body = _play(db_client, user_account["headers"], easy["id"])
    hard_body = _play(db_client, user_account["headers"], hard["id"])
    assert hard_body["xp_earned"] > easy_body["xp_earned"]


@pytest.mark.db
def test_bpm_cannot_inflate_xp_more_than_15_percent(
    db_client, user_account, admin_user
):
    slow = _create_song(db_client, admin_user, title="Slow", bpm=60)
    fast = _create_song(db_client, admin_user, title="Fast", bpm=200)
    slow_body = _play(db_client, user_account["headers"], slow["id"])
    fast_body = _play(db_client, user_account["headers"], fast["id"])
    assert fast_body["xp_earned"] / slow_body["xp_earned"] == pytest.approx(
        1.15 / 0.90, abs=0.02
    )


@pytest.mark.db
def test_repeat_slots_100_50_25_0(db_client, user_account, published_song):
    from app.services import progression as prog

    headers = user_account["headers"]
    song_id = published_song["id"]
    base = prog.xp_for_attempt(
        accuracy_percent=SCORE["accuracy_percent"],
        difficulty=published_song["difficulty"],
        bpm=published_song["bpm"],
        duration_seconds=published_song["duration_seconds"],
        completion_percent=100,
    )
    xp = [
        _play(db_client, headers, song_id)["xp_earned"] for _ in range(3)
    ]
    assert xp[0] == base
    assert xp[1] == prog.xp_for_attempt(
        accuracy_percent=SCORE["accuracy_percent"],
        difficulty=published_song["difficulty"],
        bpm=published_song["bpm"],
        duration_seconds=published_song["duration_seconds"],
        completion_percent=100,
        repeat_multiplier=0.5,
    )
    assert xp[2] == prog.xp_for_attempt(
        accuracy_percent=SCORE["accuracy_percent"],
        difficulty=published_song["difficulty"],
        bpm=published_song["bpm"],
        duration_seconds=published_song["duration_seconds"],
        completion_percent=100,
        repeat_multiplier=0.25,
    )

    # Fourth attempt of the day: no attempt XP, but the record still updates.
    fourth = _play(db_client, headers, song_id, score_points=5000)
    assert fourth["xp_earned"] == 0
    assert fourth["personal_best_updated"] is True


@pytest.mark.db
def test_personal_best_only_moves_on_improvement(
    db_client, user_account, published_song
):
    headers = user_account["headers"]
    first = _play(
        db_client, headers, published_song["id"], score_points=1000, accuracy_percent=90
    )
    assert first["personal_best_updated"] is True

    worse = _play(
        db_client, headers, published_song["id"], score_points=900, accuracy_percent=99
    )
    assert worse["personal_best_updated"] is False

    better_accuracy = _play(
        db_client, headers, published_song["id"], score_points=1000, accuracy_percent=96
    )
    assert better_accuracy["personal_best_updated"] is True

    page = db_client.get(
        f"{helpers.API}/profile/personal-bests", headers=headers
    ).json()
    assert page["total"] == 1
    item = page["items"][0]
    assert item["best_score"] == 1000
    assert item["best_accuracy"] == pytest.approx(96)
    assert item["attempts"] == 3


@pytest.mark.db
def test_finish_is_idempotent_per_session(db_client, user_account, published_song):
    headers = user_account["headers"]
    session = _start(db_client, headers, published_song["id"]).json()
    first = _finish(db_client, headers, session["id"])
    assert first.status_code == 200, first.text

    second = _finish(db_client, headers, session["id"])
    assert second.status_code == 409

    profile = _profile(db_client, headers)
    assert profile["xp_total"] == first.json()["xp_total"]


@pytest.mark.db
def test_parallel_finish_awards_xp_once(db_client, user_account, published_song):
    from concurrent.futures import ThreadPoolExecutor

    headers = user_account["headers"]
    session = _start(db_client, headers, published_song["id"]).json()

    def submit():
        return _finish(db_client, headers, session["id"]).status_code

    with ThreadPoolExecutor(max_workers=2) as pool:
        statuses = sorted(pool.map(lambda _: submit(), range(2)))

    # The session row is locked: exactly one submission wins.
    assert statuses == [200, 409]

    from sqlalchemy import func, select

    from app.database import SessionLocal
    from app.models.progression import XpAward

    with SessionLocal() as db:
        awards = db.scalar(
            select(func.count(XpAward.id)).where(
                XpAward.session_id == uuid.UUID(session["id"])
            )
        )
    assert awards == 1


# ---------------------------------------------------------------------------
# Achievements
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_achievement_is_awarded_once_and_bonus_is_not_repeated(
    db_client, user_account, published_song
):
    headers = user_account["headers"]
    first = _play(
        db_client,
        headers,
        published_song["id"],
        accuracy_percent=100,
        miss_count=0,
        avg_timing_error_ms=20,
    )
    first_bonus = sum(item["xp_bonus"] for item in first["achievements_awarded"])
    assert "first_notes" in _codes(first)
    assert "first_personal_best" in _codes(first)
    assert "clean_run" in _codes(first)
    assert "timing_40ms" in _codes(first)
    assert first["xp_total"] == first["xp_earned"] + first_bonus

    second = _play(
        db_client,
        headers,
        published_song["id"],
        accuracy_percent=100,
        miss_count=0,
        avg_timing_error_ms=20,
    )
    assert "first_notes" not in _codes(second)
    assert second["achievements_awarded"] == []
    # The repeat factor reduces attempt XP, but never an achievement bonus.
    assert second["xp_total"] == first["xp_total"] + second["xp_earned"]


@pytest.mark.db
def test_timing_achievement_stays_locked_without_metric(
    db_client, user_account, published_song
):
    body = _play(
        db_client,
        user_account["headers"],
        published_song["id"],
        avg_timing_error_ms=None,
    )
    assert "timing_40ms" not in _codes(body)

    catalog = _achievements(db_client, user_account["headers"])
    timing = next(item for item in catalog["items"] if item["code"] == "timing_40ms")
    assert timing["unlocked"] is False
    assert timing["progress"] == 0


@pytest.mark.db
def test_genre_achievements_never_invent_genres(db_client, user_account, published_song):
    body = _play(db_client, user_account["headers"], published_song["id"])
    assert "genres_3" in _codes(body)
    assert "genres_5" not in _codes(body)
    assert "genres_8" not in _codes(body)

    profile = _profile(db_client, user_account["headers"])
    assert [row["genre"] for row in profile["genre_mastery"]] == [
        published_song["genre"]
    ]
    assert profile["genre_mastery"][0]["songs"] == 1

    catalog = _achievements(db_client, user_account["headers"])
    by_code = {item["code"]: item for item in catalog["items"]}
    assert by_code["genres_3"]["unlocked"] is True
    assert by_code["genres_5"]["unlocked"] is False
    assert "Недоступно" in by_code["genres_5"]["description"]
    assert "Недоступно" in by_code["genres_8"]["description"]
    # The full catalog is always listed, active only.
    assert catalog["total"] == len(catalog["items"]) >= 27


@pytest.mark.db
def test_hard_song_accuracy_achievement(db_client, user_account, admin_user):
    easy = _create_song(db_client, admin_user, title="Easy", difficulty=1)
    hard = _create_song(db_client, admin_user, title="Hard", difficulty=4)
    headers = user_account["headers"]
    easy_body = _play(db_client, headers, easy["id"], accuracy_percent=85)
    hard_body = _play(db_client, headers, hard["id"], accuracy_percent=85)
    assert "hard_80" not in _codes(easy_body)
    assert "hard_80" in _codes(hard_body)


# ---------------------------------------------------------------------------
# Statistics window
# ---------------------------------------------------------------------------
def _insert_attempt(db, *, user_id, song_id, accuracy, completion, created_at):
    from app.models.game_session import GameSession, Score

    session = GameSession(
        user_id=user_id,
        song_id=song_id,
        song_version=1,
        speed_percent=100,
        mode="normal",
        started_at=created_at,
        finished_at=created_at,
        status="finished",
    )
    db.add(session)
    db.flush()
    db.add(
        Score(
            session_id=session.id,
            score_points=1000,
            accuracy_percent=accuracy,
            max_combo=0,
            perfect_count=0,
            good_count=0,
            miss_count=0,
            completion_percent=completion,
            created_at=created_at,
        )
    )


@pytest.mark.db
def test_stats_window_is_100_attempts_above_25_percent_completion(
    db_client, user_account, published_song, db_session
):
    user_id = uuid.UUID(user_account["id"])
    song_id = uuid.UUID(published_song["id"])
    now = datetime.now(timezone.utc)
    for index in range(120):
        _insert_attempt(
            db_session,
            user_id=user_id,
            song_id=song_id,
            accuracy=80,
            completion=100,
            created_at=now - timedelta(minutes=index),
        )
    for index in range(5):
        _insert_attempt(
            db_session,
            user_id=user_id,
            song_id=song_id,
            accuracy=100,
            completion=20,
            created_at=now - timedelta(minutes=index, seconds=30),
        )
    db_session.commit()

    profile = _profile(db_client, user_account["headers"])
    assert profile["stats"]["samples"] == 100
    assert profile["stats"]["accuracy"] == pytest.approx(80)
    assert profile["stats"]["timing"] is None
    assert profile["stats"]["consistency"] == pytest.approx(100)
    # 100 attempts on the only published song and genre: full coverage.
    assert profile["stats"]["versatility"] == pytest.approx(100)


@pytest.mark.db
def test_empty_profile_is_safe(db_client, user_account):
    profile = _profile(db_client, user_account["headers"])
    assert profile["level"] == 1
    assert profile["xp_total"] == 0
    assert profile["personal_bests"] == []
    assert profile["mastery_by_song"] == []
    assert profile["genre_mastery"] == []
    assert profile["stats"]["samples"] == 0
    assert profile["stats"]["timing"] is None
    assert profile["achievements_unlocked"] == 0
    assert profile["achievements_total"] >= 27

    page = db_client.get(
        f"{helpers.API}/profile/personal-bests", headers=user_account["headers"]
    ).json()
    assert page == {"items": [], "total": 0, "limit": 10, "offset": 0}


# ---------------------------------------------------------------------------
# Daily challenge
# ---------------------------------------------------------------------------
def _daily_today(client, headers):
    response = client.get(f"{helpers.API}/daily/today", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


@pytest.mark.db
def test_daily_today_is_deterministic(db_client, user_account, published_song):
    first = _daily_today(db_client, user_account["headers"])
    second = _daily_today(db_client, user_account["headers"])
    assert first["song"]["id"] == second["song"]["id"] == published_song["id"]
    assert first["date"] == second["date"]
    assert first["rules_version"] == 1
    assert first["official_attempt_used"] is False
    assert first["official_result"] is None
    assert first["leaderboard_top"] == []
    assert first["song"]["slug"] == published_song["slug"]


@pytest.mark.db
def test_daily_session_requires_the_song_of_the_day(
    db_client, user_account, published_song, second_published_song
):
    headers = user_account["headers"]
    today = _daily_today(db_client, headers)
    other = (
        second_published_song
        if today["song"]["id"] != second_published_song["id"]
        else published_song
    )
    response = _start(db_client, headers, other["id"], daily=True)
    assert response.status_code == 409
    assert "daily challenge" in response.json()["detail"].lower()

    ok = _start(db_client, headers, today["song"]["id"], daily=True)
    assert ok.status_code == 201, ok.text
    assert ok.json()["mode"] == "daily"


@pytest.mark.db
def test_daily_training_does_not_enter_the_daily_leaderboard(
    db_client, user_account, published_song
):
    headers = user_account["headers"]
    session = _start(db_client, headers, published_song["id"])  # practice
    assert session.status_code == 201
    finish = _finish(db_client, headers, session.json()["id"])
    assert finish.status_code == 200

    today = _daily_today(db_client, headers)
    assert today["official_attempt_used"] is False
    assert today["leaderboard_top"] == []

    board = db_client.get(f"{helpers.API}/daily/leaderboard", headers=headers)
    assert board.status_code == 200
    assert board.json()["items"] == []
    assert board.json()["total"] == 0


@pytest.mark.db
def test_daily_attempt_is_official_once(
    db_client, user_account, second_user, published_song
):
    headers = user_account["headers"]
    today = _daily_today(db_client, headers)
    song_id = today["song"]["id"]

    first_session = _start(db_client, headers, song_id, daily=True).json()
    second_session = _start(db_client, headers, song_id, daily=True).json()
    assert first_session["mode"] == second_session["mode"] == "daily"

    first = _finish(db_client, headers, first_session["id"], score_points=1500)
    assert first.status_code == 200, first.text

    # A second official result for the same day is rejected.
    second = _finish(db_client, headers, second_session["id"], score_points=2000)
    assert second.status_code == 409

    # Nor can a new daily session be created after the attempt is used.
    assert _start(db_client, headers, song_id, daily=True).status_code == 409

    board = db_client.get(f"{helpers.API}/daily/leaderboard", headers=headers).json()
    assert board["total"] == 1
    assert board["items"][0]["username"] == user_account["username"]
    assert board["items"][0]["score_points"] == 1500

    today_after = _daily_today(db_client, headers)
    assert today_after["official_attempt_used"] is True
    assert today_after["official_result"]["score_points"] == 1500
    assert today_after["official_result"]["rank"] == 1

    # Another player gets their own attempt and a worse rank.
    other_headers = second_user["headers"]
    other_session = _start(db_client, other_headers, song_id, daily=True).json()
    other = _finish(db_client, other_headers, other_session["id"], score_points=100)
    assert other.status_code == 200

    board = db_client.get(f"{helpers.API}/daily/leaderboard", headers=headers).json()
    assert [item["score_points"] for item in board["items"]] == [1500, 100]
    assert [item["rank"] for item in board["items"]] == [1, 2]


@pytest.mark.db
def test_daily_achievements(db_client, user_account, published_song):
    headers = user_account["headers"]
    today = _daily_today(db_client, headers)
    session = _start(db_client, headers, today["song"]["id"], daily=True).json()
    body = _finish(db_client, headers, session["id"], accuracy_percent=75)
    assert body.status_code == 200, body.text
    codes = _codes(body.json())
    assert "daily_first" in codes
    assert "daily_score_70" in codes
    assert "daily_streak_3" not in codes


@pytest.mark.db
def test_daily_first_place_is_awarded_after_the_day_ends(
    db_client, user_account, published_song
):
    """There is no background worker: the award happens on the next read."""
    from app.models.game_session import GameSession
    from app.models.progression import DailyChallenge, DailyResult
    from app.database import SessionLocal
    from app.services.progression import utc_today

    headers = user_account["headers"]
    song_id = uuid.UUID(_daily_today(db_client, headers)["song"]["id"])
    user_id = uuid.UUID(user_account["id"])
    yesterday = utc_today() - timedelta(days=1)
    finished_at = datetime.now(timezone.utc) - timedelta(days=1)

    with SessionLocal() as db:
        db.add(
            DailyChallenge(
                challenge_date=yesterday, song_id=song_id, rules_version=1
            )
        )
        db.flush()
        session = GameSession(
            user_id=user_id,
            song_id=song_id,
            song_version=1,
            speed_percent=100,
            mode="daily",
            started_at=finished_at,
            finished_at=finished_at,
            status="finished",
        )
        db.add(session)
        db.flush()
        db.add(
            DailyResult(
                challenge_date=yesterday,
                user_id=user_id,
                session_id=session.id,
                score_points=900,
                accuracy_percent=88,
            )
        )
        db.commit()

    catalog = _achievements(db_client, headers)
    by_code = {item["code"]: item for item in catalog["items"]}
    assert by_code["daily_first_place"]["unlocked"] is True

    profile = _profile(db_client, headers)
    assert profile["xp_total"] == 100


# ---------------------------------------------------------------------------
# Contract guards
# ---------------------------------------------------------------------------
@pytest.mark.db
@pytest.mark.parametrize(
    "path",
    [
        "/profile/progression",
        "/profile/personal-bests",
        "/achievements",
        "/daily/today",
        "/daily/leaderboard",
    ],
)
def test_progression_endpoints_require_auth(db_client, path):
    assert db_client.get(f"{helpers.API}{path}").status_code == 401


@pytest.mark.db
def test_global_leaderboard_still_ignores_progression(
    db_client, user_account, second_user, published_song
):
    _play(db_client, user_account["headers"], published_song["id"], score_points=1500)
    _play(db_client, second_user["headers"], published_song["id"], score_points=900)

    board = db_client.get(f"{helpers.API}/leaderboard/global").json()
    assert [item["username"] for item in board["items"]] == [
        user_account["username"],
        second_user["username"],
    ]
    assert board["items"][0]["rating_points"] == 1500
    assert board["items"][0]["games_played"] == 1


def test_public_profile_exposes_level_xp_and_achievements(db_client, db_session):
    from sqlalchemy import update

    from app.models.user import User

    player = helpers.register_user(db_client)
    db_session.execute(
        update(User)
        .where(User.id == uuid.UUID(player["id"]))
        .values(xp_total=1500, level=7)
    )
    db_session.commit()

    response = db_client.get(
        f"{helpers.API}/profile/users/{player['id']}", headers=player["headers"]
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["username"] == player["username"]
    assert body["level"] == 7
    assert body["xp_total"] == 1500
    assert body["achievements_unlocked"] == len(body["achievements"])

    missing = db_client.get(
        f"{helpers.API}/profile/users/{uuid.uuid4()}", headers=player["headers"]
    )
    assert missing.status_code == 404
