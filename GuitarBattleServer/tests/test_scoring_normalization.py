"""Scoring normalization and anti-farm XP (Battle + Online).

The rules pinned here are the ones a client can observe through the API:

* ``normalized_score = actual / effective completion`` is stored next to the
  actual value (never instead of it) and is ``NULL`` below the floor;
* Battle XP comes from the existing solo formula times a tower repeat factor
  (1st completion 100%, 2nd 50%, 3rd 25%, then geometric with a floor);
* a repeated ``run_id`` never awards XP or progress twice;
* the PB bonus requires a strictly better accuracy and is granted at most once;
* online duels keep Elo untouched and decay XP per song/day;
* Practice/Study stays unranked.
"""
from __future__ import annotations

import uuid
from datetime import datetime, timezone

import pytest
from sqlalchemy import func, select

from app.models.duel import DuelMatch, DuelParticipant, DuelSongBest
from app.models.campaign import CampaignStageRun
from app.models.character_unlocks import UserCampaignProgress
from app.models.game_session import GameSession, Score
from app.models.song import Song
from app.models.user import User
from app.services import progression, reward_config
from app.services.duel_service import _apply_xp
from app.services.duel_rules import accuracy_factor, xp_for_duel
from tests import helpers

pytestmark = pytest.mark.db

API = helpers.API
CAMPAIGNS_ADMIN = f"{API}/admin/campaigns"
RULES = f"{API}/gameplay/rules"


# ---------------------------------------------------------------------------
# Fixtures/helpers
# ---------------------------------------------------------------------------
def create_campaign(
    client,
    admin_headers,
    *,
    song_count: int = 1,
    difficulty: int | None = None,
    reward_character_id: str | None = None,
) -> dict:
    if isinstance(admin_headers, dict) and "headers" in admin_headers:
        admin_headers = admin_headers["headers"]

    payload = {"name": "Tower " + uuid.uuid4().hex[:8], "bot_level": 1}
    if reward_character_id is not None:
        payload["reward_character_id"] = reward_character_id
    created = client.post(CAMPAIGNS_ADMIN, json=payload, headers=admin_headers)
    assert created.status_code == 201, created.text
    campaign = created.json()

    songs = []
    for index in range(song_count):
        overrides = {"title": f"Tower Song {index}"}
        if difficulty is not None:
            overrides["difficulty"] = difficulty
        song = helpers.create_song(client, admin_headers, **overrides)
        helpers.publish_song(client, admin_headers, song["id"])
        songs.append(song)

    updated = client.put(
        f"{CAMPAIGNS_ADMIN}/{campaign['id']}/songs",
        json={"songs": [{"song_id": song["id"]} for song in songs]},
        headers=admin_headers,
    )
    assert updated.status_code == 200, updated.text
    return updated.json()


def stage_result(
    client,
    headers,
    campaign_id: str,
    position: int,
    *,
    run_id: str | None = None,
    won: bool = True,
    score_points: int = 400,
    accuracy_percent: float = 90.0,
    perfect_count: int = 90,
    great_count: int = 0,
    good_count: int = 0,
    miss_count: int = 0,
    notes_total: int = 100,
    notes_played: int | None = None,
    completion_percent: float | None = None,
) -> dict:
    played = notes_total if notes_played is None else notes_played
    completion = (
        100.0 * played / notes_total
        if completion_percent is None and notes_total > 0
        else (completion_percent if completion_percent is not None else 0.0)
    )
    payload = {
        "run_id": run_id or str(uuid.uuid4()),
        "won": won,
        "score_points": score_points,
        "accuracy_percent": accuracy_percent,
        "perfect_count": perfect_count,
        "great_count": great_count,
        "good_count": good_count,
        "miss_count": miss_count,
        "completion_percent": completion,
        "notes_total": notes_total,
        "notes_played": played,
    }
    return client.post(
        f"{API}/campaigns/{campaign_id}/stages/{position}/result",
        json=payload,
        headers=headers,
    )


def user_xp(client, headers) -> int:
    response = client.get(f"{API}/profile/progression", headers=headers)
    assert response.status_code == 200, response.text
    return int(response.json()["xp_total"])


def start_session(client, headers, song_id: str, *, practice: bool = False):
    return client.post(
        f"{API}/games/sessions",
        json={"song_id": song_id, "speed_percent": 100, "practice": practice},
        headers=headers,
    )


# ---------------------------------------------------------------------------
# Normalization
# ---------------------------------------------------------------------------
def test_early_victory_stores_normalized_but_keeps_the_actual_score(
    db_client, admin_user, user_account
):
    """200 of 600 notes with 400 points is extrapolated to 1200."""
    campaign = create_campaign(db_client, admin_user, song_count=1)

    response = stage_result(
        db_client,
        user_account["headers"],
        campaign["id"],
        0,
        score_points=400,
        accuracy_percent=100.0,
        notes_total=600,
        notes_played=200,
        perfect_count=200,
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["score_points"] == 400
    assert body["normalized_score"] == 1200
    # Rounding of the payload's completion is the only source of slack.
    assert body["completion_percent"] == pytest.approx(100.0 / 3.0, rel=1e-6)
    # Below the 50% XP threshold: the extrapolated score is stored, but the
    # attempt earns no tower XP.
    assert body["xp_earned"] == 0
    assert body["xp_breakdown"]["normalized_score"] == 1200

    # A full run of the same score is not multiplied at all.
    campaign_full = create_campaign(db_client, admin_user, song_count=1)
    full = stage_result(
        db_client,
        user_account["headers"],
        campaign_full["id"],
        0,
        score_points=400,
        accuracy_percent=100.0,
        notes_total=600,
        notes_played=600,
        perfect_count=600,
    )
    assert full.status_code == 200, full.text
    assert full.json()["normalized_score"] == 400
    assert full.json()["xp_earned"] > 0


def test_normalization_is_null_below_the_floor(db_client, admin_user, user_account):
    campaign = create_campaign(db_client, admin_user, song_count=1)
    response = stage_result(
        db_client,
        user_account["headers"],
        campaign["id"],
        0,
        notes_total=1000,
        notes_played=50,  # 5% < default 10%
        score_points=50,
    )
    assert response.status_code == 200, response.text
    assert response.json()["normalized_score"] is None


def test_early_defeat_has_no_tower_xp_even_with_full_completion(
    db_client, admin_user, user_account
):
    campaign = create_campaign(db_client, admin_user, song_count=2)
    before = user_xp(db_client, user_account["headers"])

    defeat = stage_result(
        db_client,
        user_account["headers"],
        campaign["id"],
        0,
        won=False,
        notes_total=200,
        notes_played=100,
        accuracy_percent=95.0,
    )
    assert defeat.status_code == 200, defeat.text
    body = defeat.json()
    assert body["won"] is False
    assert body["xp_earned"] == 0
    assert body["normalized_score"] == 400 * 2  # stored, 100/200 notes
    assert user_xp(db_client, user_account["headers"]) == before

    # A defeat does not advance the tower.
    progress = db_client.get(f"{API}/campaigns/progress", headers=user_account["headers"])
    row = next(
        item for item in progress.json()["items"] if item["campaign_id"] == campaign["id"]
    )
    assert row["best_stage"] == 0


def test_completion_below_half_gives_no_battle_xp(
    db_client, admin_user, user_account
):
    campaign = create_campaign(db_client, admin_user, song_count=2)
    response = stage_result(
        db_client,
        user_account["headers"],
        campaign["id"],
        0,
        won=True,
        notes_total=400,
        notes_played=160,  # 40% < 50%
        accuracy_percent=100.0,
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["completion_percent"] == pytest.approx(40.0)
    assert body["normalized_score"] == 1000
    assert body["xp_earned"] == 0


def test_difficulty_changes_battle_xp_but_not_normalization(
    db_client, admin_user, user_account
):
    easy = create_campaign(db_client, admin_user, song_count=1, difficulty=1)
    hard = create_campaign(db_client, admin_user, song_count=1, difficulty=5)

    easy_result = stage_result(
        db_client, user_account["headers"], easy["id"], 0, accuracy_percent=100.0
    ).json()
    hard_result = stage_result(
        db_client, user_account["headers"], hard["id"], 0, accuracy_percent=100.0
    ).json()

    assert hard_result["xp_earned"] > easy_result["xp_earned"]
    assert easy_result["normalized_score"] == hard_result["normalized_score"]


def test_note_quality_changes_battle_xp(db_client, admin_user, second_user):
    """Same score, worse quality (miss instead of perfect) → less XP."""
    clean = create_campaign(db_client, admin_user, song_count=1)
    sloppy = create_campaign(db_client, admin_user, song_count=1)

    perfect = stage_result(
        db_client,
        second_user["headers"],
        clean["id"],
        0,
        accuracy_percent=100.0,
        perfect_count=100,
        notes_total=100,
    ).json()
    misses = stage_result(
        db_client,
        second_user["headers"],
        sloppy["id"],
        0,
        accuracy_percent=50.0,
        perfect_count=50,
        miss_count=50,
        notes_total=100,
    ).json()

    assert perfect["xp_earned"] > misses["xp_earned"]


# ---------------------------------------------------------------------------
# Idempotency and repeat decay
# ---------------------------------------------------------------------------
def test_repeated_run_id_never_rewards_twice(
    db_client, admin_user, user_account, db_session
):
    campaign = create_campaign(db_client, admin_user, song_count=2)
    run_id = str(uuid.uuid4())

    first = stage_result(
        db_client, user_account["headers"], campaign["id"], 0, run_id=run_id
    )
    assert first.status_code == 200, first.text
    xp_after_first = user_xp(db_client, user_account["headers"])
    assert first.json()["xp_earned"] > 0

    second = stage_result(
        db_client, user_account["headers"], campaign["id"], 0, run_id=run_id
    )
    assert second.status_code == 200, second.text
    body = second.json()
    assert body["already_processed"] is True
    # The stored reward is reported, but it is not added a second time.
    assert body["xp_earned"] == first.json()["xp_earned"]
    assert body["best_stage"] == 1
    assert user_xp(db_client, user_account["headers"]) == xp_after_first

    runs = db_session.scalar(
        select(func.count(CampaignStageRun.id)).where(
            CampaignStageRun.user_id == uuid.UUID(user_account["id"])
        )
    )
    assert runs == 1


def test_tower_repeat_decay_and_completion_count(
    db_client, admin_user, user_account, db_session
):
    campaign = create_campaign(db_client, admin_user, song_count=1)

    factors: list[float] = []
    for _ in range(6):
        response = stage_result(
            db_client, user_account["headers"], campaign["id"], 0, won=True
        )
        assert response.status_code == 200, response.text
        factors.append(response.json()["repeat_factor"])

    expected = [
        1.0,
        reward_config.TOWER_REPEAT_DECAY,
        reward_config.TOWER_REPEAT_DECAY**2,
        reward_config.TOWER_REPEAT_DECAY**3,
        reward_config.TOWER_REPEAT_DECAY**4,
        reward_config.TOWER_REPEAT_FLOOR,
    ]
    assert factors == pytest.approx(expected)

    row = db_session.scalar(
        select(UserCampaignProgress).where(
            UserCampaignProgress.user_id == uuid.UUID(user_account["id"]),
            UserCampaignProgress.campaign_id == uuid.UUID(campaign["id"]),
        )
    )
    assert row is not None
    assert row.completion_count == 6

    # The completion count survives a new login (it is server-side truth).
    login = helpers.login_user(
        db_client, username=user_account["username"], password=user_account["password"]
    )
    headers = helpers.auth_headers(login["access_token"])
    progress = db_client.get(f"{API}/campaigns/progress", headers=headers)
    row_payload = next(
        item
        for item in progress.json()["items"]
        if item["campaign_id"] == campaign["id"]
    )
    assert row_payload["completed"] is True

    next_run = stage_result(db_client, headers, campaign["id"], 0, won=True)
    assert next_run.status_code == 200, next_run.text
    assert next_run.json()["repeat_factor"] == pytest.approx(
        reward_config.TOWER_REPEAT_FLOOR
    )


def test_stage_must_be_current(db_client, admin_user, user_account):
    campaign = create_campaign(db_client, admin_user, song_count=2)
    skipped = stage_result(
        db_client, user_account["headers"], campaign["id"], 1
    )
    assert skipped.status_code == 409, skipped.text
    assert skipped.json()["detail"] == "stage_not_current"

    empty = create_campaign(db_client, admin_user, song_count=0)
    no_stages = stage_result(db_client, user_account["headers"], empty["id"], 0)
    assert no_stages.status_code == 409, no_stages.text
    assert no_stages.json()["detail"] == "no_stages"


# ---------------------------------------------------------------------------
# Defeat resets the tower run
# ---------------------------------------------------------------------------
def test_loss_resets_the_run_to_the_first_stage(
    db_client, admin_user, user_account, db_session
):
    """A defeat sends the tower back to stage 0; history is not reset."""
    campaign = create_campaign(db_client, admin_user, song_count=3)
    headers = user_account["headers"]

    first = stage_result(db_client, headers, campaign["id"], 0)
    assert first.status_code == 200, first.text
    assert first.json()["best_stage"] == 1

    second = stage_result(db_client, headers, campaign["id"], 1)
    assert second.status_code == 200, second.text
    assert second.json()["best_stage"] == 2

    loss_run_id = str(uuid.uuid4())
    loss = stage_result(
        db_client, headers, campaign["id"], 2, run_id=loss_run_id, won=False
    )
    assert loss.status_code == 200, loss.text
    assert loss.json()["won"] is False
    assert loss.json()["best_stage"] == 0

    progress = db_client.get(f"{API}/campaigns/progress", headers=headers)
    row = next(
        item
        for item in progress.json()["items"]
        if item["campaign_id"] == campaign["id"]
    )
    assert row["best_stage"] == 0
    assert row["completed"] is False

    stored = db_session.scalar(
        select(UserCampaignProgress).where(
            UserCampaignProgress.user_id == uuid.UUID(user_account["id"]),
            UserCampaignProgress.campaign_id == uuid.UUID(campaign["id"]),
        )
    )
    assert stored is not None
    assert stored.completion_count == 0
    assert stored.completed_at is None

    # Retrying the same loss report is idempotent and resets nothing again.
    retry = stage_result(
        db_client, headers, campaign["id"], 2, run_id=loss_run_id, won=False
    )
    assert retry.status_code == 200, retry.text
    assert retry.json()["already_processed"] is True
    assert retry.json()["best_stage"] == 0

    # The next attempt starts at the first opponent and is accepted.
    replay = stage_result(db_client, headers, campaign["id"], 0)
    assert replay.status_code == 200, replay.text
    assert replay.json()["best_stage"] == 1

    # A late retry of the stored loss must not roll the new run back.
    late = stage_result(
        db_client, headers, campaign["id"], 2, run_id=loss_run_id, won=False
    )
    assert late.status_code == 200, late.text
    assert late.json()["already_processed"] is True
    assert late.json()["best_stage"] == 1


def test_loss_after_a_completed_tower_resets_stage_but_keeps_history(
    db_client, admin_user, user_account, db_session
):
    campaign = create_campaign(db_client, admin_user, song_count=2)
    headers = user_account["headers"]

    for position in (0, 1):
        won = stage_result(db_client, headers, campaign["id"], position)
        assert won.status_code == 200, won.text
    assert won.json()["best_stage"] == 2
    assert won.json()["completed"] is True

    # The finished tower restarts at stage 0; losing there resets the run.
    loss = stage_result(db_client, headers, campaign["id"], 0, won=False)
    assert loss.status_code == 200, loss.text
    assert loss.json()["best_stage"] == 0
    # The completion record is history and stays visible.
    assert loss.json()["completed"] is True

    progress = db_client.get(f"{API}/campaigns/progress", headers=headers)
    row = next(
        item
        for item in progress.json()["items"]
        if item["campaign_id"] == campaign["id"]
    )
    assert row["best_stage"] == 0
    assert row["completed"] is True

    stored = db_session.scalar(
        select(UserCampaignProgress).where(
            UserCampaignProgress.user_id == uuid.UUID(user_account["id"]),
            UserCampaignProgress.campaign_id == uuid.UUID(campaign["id"]),
        )
    )
    assert stored is not None
    assert stored.completion_count == 1
    assert stored.completed_at is not None


# ---------------------------------------------------------------------------
# Personal best bonus
# ---------------------------------------------------------------------------
def test_pb_bonus_requires_strict_improvement(db_client, admin_user, user_account):
    campaign = create_campaign(db_client, admin_user, song_count=1)

    first = stage_result(
        db_client, user_account["headers"], campaign["id"], 0, accuracy_percent=80.0
    ).json()
    assert first["pb_improved"] is False
    assert first["xp_breakdown"]["pb_bonus"] == 0

    better = stage_result(
        db_client, user_account["headers"], campaign["id"], 0, accuracy_percent=95.0
    ).json()
    assert better["pb_improved"] is True
    assert better["xp_breakdown"]["pb_bonus"] > 0

    equal = stage_result(
        db_client, user_account["headers"], campaign["id"], 0, accuracy_percent=95.0
    ).json()
    assert equal["pb_improved"] is False
    assert equal["xp_breakdown"]["pb_bonus"] == 0


# ---------------------------------------------------------------------------
# Solo sessions
# ---------------------------------------------------------------------------
def test_solo_completion_below_half_gives_zero_xp_and_keeps_normalized(
    db_client, user_account, published_song
):
    session = start_session(db_client, user_account["headers"], published_song["id"])
    assert session.status_code == 201, session.text

    before = user_xp(db_client, user_account["headers"])
    finished = db_client.post(
        f"{API}/games/sessions/{session.json()['id']}/finish",
        json={
            "score_points": 400,
            "accuracy_percent": 90.0,
            "max_combo": 10,
            "perfect_count": 100,
            "good_count": 0,
            "miss_count": 0,
            "completion_percent": 40.0,
            "notes_total": 1000,
            "notes_played": 400,
        },
        headers=user_account["headers"],
    )
    assert finished.status_code == 200, finished.text
    body = finished.json()
    assert body["xp_earned"] == 0
    assert body["normalized_score"] == 1000
    assert user_xp(db_client, user_account["headers"]) == before


def test_practice_session_stays_unranked(
    db_client, user_account, published_song, db_session
):
    session = start_session(
        db_client, user_account["headers"], published_song["id"], practice=True
    )
    assert session.status_code == 201, session.text
    before = user_xp(db_client, user_account["headers"])

    finished = db_client.post(
        f"{API}/games/sessions/{session.json()['id']}/finish",
        json={
            "score_points": 9999,
            "accuracy_percent": 100.0,
            "max_combo": 100,
            "perfect_count": 100,
            "good_count": 0,
            "miss_count": 0,
            "completion_percent": 100.0,
            "notes_total": 100,
            "notes_played": 100,
        },
        headers=user_account["headers"],
    )
    assert finished.status_code == 200, finished.text
    body = finished.json()
    assert body["xp_earned"] == 0
    assert body["normalized_score"] is None
    assert user_xp(db_client, user_account["headers"]) == before

    score = db_session.scalar(
        select(Score).where(Score.session_id == uuid.UUID(session.json()["id"]))
    )
    assert score is not None and score.normalized_score is None


def test_gameplay_rules_are_public_and_documented(db_client):
    response = db_client.get(RULES)
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["min_completion_for_xp"] == progression.MIN_COMPLETION_FOR_XP
    assert body["tower_repeat_decay"] == reward_config.TOWER_REPEAT_DECAY
    assert body["online_repeat_factors"] == list(reward_config.ONLINE_REPEAT_FACTORS)
    assert body["pb_bonus_factor"] == reward_config.PB_BONUS_FACTOR
    assert body["normalization_min_completion"] == reward_config.NORMALIZATION_MIN_COMPLETION


# ---------------------------------------------------------------------------
# Online duels (service level: the match is already server-authoritative)
# ---------------------------------------------------------------------------
def _participant(match: DuelMatch, user_id: uuid.UUID, **overrides) -> DuelParticipant:
    values = {
        "match_id": match.id,
        "user_id": user_id,
        "side": "player",
        "perfect_count": 100,
        "great_count": 0,
        "good_count": 0,
        "miss_count": 0,
        "notes_played": 100,
        "result": "win",
    }
    values.update(overrides)
    return DuelParticipant(**values)


def _online_match(song: Song, *, note_count: int = 100) -> DuelMatch:
    return DuelMatch(
        song_id=song.id,
        mode="online",
        status="playing",
        rules_version=3,
        seed=1,
        note_count=note_count,
    )


def test_online_xp_repeat_decay_and_idempotency(
    db_client, user_account, published_song, db_session
):
    user_id = uuid.UUID(user_account["id"])
    song = db_session.get(Song, uuid.UUID(published_song["id"]))
    assert song is not None
    user = db_session.get(User, user_id)
    assert user is not None
    user.xp_total = 0
    user.level = 1
    db_session.commit()

    def run_match():
        match = _online_match(song)
        db_session.add(match)
        db_session.flush()
        participant = _participant(match, user_id)
        db_session.add(participant)
        db_session.flush()
        awarded = _apply_xp(db_session, participant, "win", match=match)
        match.status = "finished"
        match.finished_at = datetime.now(timezone.utc)
        db_session.commit()
        return match, participant, awarded

    base = xp_for_duel(perfect=100, great=0, good=0, result="win")
    expected_first = int(round(base * progression.difficulty_factor(song.difficulty)))

    _, first, first_xp = run_match()
    assert first_xp == expected_first
    assert first.xp_awarded == expected_first
    assert first.score_points == 10000
    assert first.normalized_score == 10000
    assert first.xp_breakdown["repeat_factor"] == 1.0
    assert first.xp_breakdown["pb_improved"] is False
    assert first.xp_applied_at is not None

    # A second finished duel of the same song today uses the 50% slot.
    _, second, second_xp = run_match()
    assert second.xp_breakdown["repeat_factor"] == pytest.approx(
        reward_config.ONLINE_REPEAT_FACTORS[1]
    )
    assert second_xp == int(
        round(base * progression.difficulty_factor(song.difficulty) * 0.5)
    )
    # The same quality is not an improvement: no PB bonus.
    assert second.xp_breakdown["pb_improved"] is False
    assert second.xp_breakdown["pb_bonus"] == 0

    # Re-applying XP to the same participant is a no-op.
    balance = user_xp(db_client, user_account["headers"])
    again = _apply_xp(db_session, second, "win", match=_online_match(song))
    db_session.rollback()
    assert again == 0
    assert user_xp(db_client, user_account["headers"]) == balance

    best = db_session.scalar(
        select(DuelSongBest).where(
            DuelSongBest.user_id == user_id, DuelSongBest.song_id == song.id
        )
    )
    assert best is not None
    assert best.best_accuracy == pytest.approx(100.0)


def test_online_pb_bonus_requires_strict_improvement(
    db_client, user_account, published_song, db_session
):
    user_id = uuid.UUID(user_account["id"])
    song = db_session.get(Song, uuid.UUID(published_song["id"]))
    assert song is not None

    def run(perfect: int, miss: int):
        match = _online_match(song)
        db_session.add(match)
        db_session.flush()
        participant = _participant(
            match, user_id, perfect_count=perfect, miss_count=miss
        )
        db_session.add(participant)
        db_session.flush()
        awarded = _apply_xp(db_session, participant, "win", match=match)
        match.status = "finished"
        match.finished_at = datetime.now(timezone.utc)
        db_session.commit()
        return participant, awarded

    _, first_xp = run(50, 50)
    assert first_xp > 0

    improved, improved_xp = run(100, 0)
    assert improved.xp_breakdown["pb_improved"] is True
    assert improved.xp_breakdown["pb_bonus"] > 0
    assert improved_xp == improved.xp_breakdown["pb_bonus"] + int(
        round(
            xp_for_duel(perfect=100, great=0, good=0, result="win")
            * progression.difficulty_factor(song.difficulty)
            * improved.xp_breakdown["repeat_factor"]
        )
    )

    equal, _ = run(100, 0)
    assert equal.xp_breakdown["pb_improved"] is False
    assert equal.xp_breakdown["pb_bonus"] == 0


def test_online_difficulty_and_quality_change_xp(
    db_client, admin_user, user_account, db_session
):
    user_id = uuid.UUID(user_account["id"])

    def published(difficulty: int) -> Song:
        song = helpers.create_song(
            db_client,
            admin_user["headers"],
            title=f"Online {uuid.uuid4().hex[:6]}",
            difficulty=difficulty,
        )
        helpers.publish_song(db_client, admin_user["headers"], song["id"])
        model = db_session.get(Song, uuid.UUID(song["id"]))
        assert model is not None
        return model

    easy_song = published(1)
    hard_song = published(5)
    sloppy_song = published(3)
    clean_song = published(3)

    def award(song: Song, perfect: int, miss: int) -> int:
        match = _online_match(song)
        db_session.add(match)
        db_session.flush()
        participant = _participant(
            match, user_id, perfect_count=perfect, miss_count=miss
        )
        db_session.add(participant)
        db_session.flush()
        awarded = _apply_xp(db_session, participant, "win", match=match)
        match.status = "finished"
        match.finished_at = datetime.now(timezone.utc)
        db_session.commit()
        return awarded

    easy_xp = award(easy_song, 100, 0)
    hard_xp = award(hard_song, 100, 0)
    assert hard_xp > easy_xp

    # Worse quality on a fresh song of the same difficulty → less XP.
    sloppy_xp = award(sloppy_song, 50, 50)
    clean_xp = award(clean_song, 100, 0)
    assert sloppy_xp < clean_xp


def test_online_accuracy_uses_duel_quality_formula():
    """The PB comparison uses the same accuracy as ``duel_rules``."""
    assert accuracy_factor(100, 0, 0, 100) == 1.0
    assert accuracy_factor(50, 0, 0, 100) == pytest.approx(0.5)
