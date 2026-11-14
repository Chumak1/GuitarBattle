"""Guitar Skills rating: formula scenarios and API contract tests.

The formula tests are pure (no database) and pin the documented behaviour:
accuracy/challenge/score weighting, difficulty tiers, note factor, recency,
anti-farm cap, duel expectation, the shared duel accuracy formula, progression
and the final weighted total.

The API tests exercise the real migrations and the HTTP contract the JUCE
client uses: ranked solo finish, idempotent backfill, duel finalisation,
public leaderboard/detail, social additive fields and the admin endpoints.
"""
from __future__ import annotations

import uuid
from datetime import datetime, timedelta, timezone

import pytest
from sqlalchemy import func, select

from app.models.duel import DuelMatch, DuelParticipant
from app.models.game_session import GameSession, Score
from app.models.guitar_skills import (
    GuitarSkillsEvent,
    GuitarSkillsHistory,
    UserGuitarSkills,
)
from app.models.song import Song
from app.models.user import User
from app.services import duel_service, guitar_skills
from app.services.guitar_skills import (
    EventRecord,
    SkillsConfig,
    compute_skills,
)
from tests import helpers

pytestmark = pytest.mark.db

NOW = datetime(2026, 11, 12, 12, 0, tzinfo=timezone.utc)
CONFIG = SkillsConfig(median_note_count=100)


def _event(
    key: str,
    *,
    accuracy: float = 100.0,
    score: int = 10_000,
    note_count: int | None = 100,
    difficulty: int | None = 10,
    occurred_at: datetime | None = None,
    song_id: uuid.UUID | None = None,
    **kwargs,
) -> EventRecord:
    return EventRecord(
        event_key=key,
        song_id=song_id or uuid.uuid4(),
        accuracy=accuracy,
        score=score,
        note_count=note_count,
        difficulty=difficulty,
        occurred_at=occurred_at or NOW,
        **kwargs,
    )


def _duel_event(
    key: str,
    *,
    result: str,
    player_skills: float,
    opponent_skills: float,
    margin: float,
    occurred_at: datetime | None = None,
) -> EventRecord:
    return EventRecord(
        event_key=key,
        song_id=uuid.uuid4(),
        accuracy=90.0,
        score=9000,
        note_count=100,
        difficulty=5,
        occurred_at=occurred_at or NOW,
        match_id=uuid.uuid4(),
        result=result,
        score_margin=margin,
        player_pre_match_rating=1000,
        opponent_pre_match_rating=1000,
        player_pre_match_guitar_skills=player_skills,
        opponent_pre_match_guitar_skills=opponent_skills,
    )


# ---------------------------------------------------------------------------
# A..J formula scenarios
# ---------------------------------------------------------------------------
def test_a_empty_history_is_neutral_and_provisional():
    result = compute_skills([], level=1, xp_total=0, config=CONFIG)
    assert result.accuracy.value == 50.0
    assert result.challenge.value == 50.0
    assert result.score.value == 50.0
    assert result.duel.value == 50.0
    # P = 70% level + 30% XP; level 1 / 50 and zero XP.
    assert result.progression.value == 1.4
    assert result.total == 45.1
    assert result.provisional is True
    assert result.confidence == 0.1
    assert result.events_counted == 0
    assert result.duel_matches == 0


def test_b_perfect_hard_event_uses_full_accuracy_and_score():
    result = compute_skills(
        [_event("solo:1", accuracy=100.0, score=10_000, note_count=100, difficulty=10)],
        level=1,
        xp_total=0,
        config=CONFIG,
    )
    assert result.accuracy.value == 100.0
    # Challenge is accuracy * difficulty * note / 1.25 = 100 * 1 / 1.25.
    assert result.challenge.value == 80.0
    assert result.score.value == 100.0
    assert result.total == 73.6
    assert result.accuracy_percent == 100.0


def test_c_difficulty_tiers_change_challenge_not_accuracy():
    events = [
        _event("solo:easy", accuracy=100, difficulty=3),
        _event(
            "solo:hard",
            accuracy=100,
            difficulty=8,
            occurred_at=NOW - timedelta(days=1),
        ),
    ]
    result = compute_skills(events, level=1, xp_total=0, config=CONFIG)
    assert result.accuracy.value == 100.0
    # Challenge is the accuracy-weighted mean of ``acc*difficulty*note/1.25``:
    # the same recency/difficulty/note weights apply to both the value and the
    # denominator, so the weights are applied consistently.
    num = (1.0 * 0.60 * 1.0 / 1.25) * (0.60 * 1.0) + (
        1.0 * 1.00 * 1.0 / 1.25
    ) * (1.00 * 2 ** -0.1)
    den = (0.60 * 1.0) + (1.00 * 2 ** -0.1)
    expected = 100 * num / den
    assert result.challenge.value == pytest.approx(expected, abs=0.1)


def test_d_note_factor_clamps_and_unknown_is_neutral():
    events = [
        _event("solo:short", accuracy=100, difficulty=10, note_count=25),
        _event(
            "solo:long",
            accuracy=100,
            difficulty=10,
            note_count=400,
            occurred_at=NOW - timedelta(days=1),
        ),
    ]
    result = compute_skills(events, level=1, xp_total=0, config=CONFIG)
    # sqrt(25/100)=0.5 clamped to 0.75; sqrt(400/100)=2 clamped to 1.25.
    # Same consistent weighting as above: value and weight share the factors.
    num = (1.0 * 1.0 * 0.75 / 1.25) * (0.75 * 1.0) + (
        1.0 * 1.0 * 1.25 / 1.25
    ) * (1.25 * 2 ** -0.1)
    den = (0.75 * 1.0) + (1.25 * 2 ** -0.1)
    expected = 100 * num / den
    assert result.challenge.value == pytest.approx(expected, abs=0.1)

    unknown = compute_skills(
        [_event("solo:unknown", note_count=None)], level=1, xp_total=0, config=CONFIG
    )
    # Unknown chart length keeps the note factor neutral (1.0) and skips S.
    assert unknown.score.value == 50.0
    assert unknown.score.events == 0


def test_e_recency_weights_newest_event_more():
    events = [
        _event("solo:new", accuracy=100),
        _event("solo:old", accuracy=50, occurred_at=NOW - timedelta(days=1)),
    ]
    result = compute_skills(events, level=1, xp_total=0, config=CONFIG)
    expected = (100 * 1.0 + 50 * 2 ** -0.1) / (1.0 + 2 ** -0.1)
    assert result.accuracy.value == pytest.approx(expected, abs=0.1)
    assert result.accuracy.value > 75.0


def test_f_per_song_cap_reduces_repeat_farming():
    song = uuid.uuid4()
    events = [
        _event(f"solo:{index}", accuracy=0, song_id=song)
        for index in range(3)
    ] + [_event("solo:other", accuracy=100, song_id=uuid.uuid4())]
    capped = compute_skills(events, level=1, xp_total=0, config=CONFIG)
    uncapped = compute_skills(
        events,
        level=1,
        xp_total=0,
        config=SkillsConfig(median_note_count=100, per_song_attempt_cap=100.0),
    )
    assert capped.accuracy.value > uncapped.accuracy.value
    assert capped.accuracy.value < 100.0


def test_g_score_efficiency_skips_unknown_chart_length():
    events = [
        _event("solo:known", score=5000, note_count=100),
        _event("solo:unknown", score=999_999, note_count=None),
    ]
    result = compute_skills(events, level=1, xp_total=0, config=CONFIG)
    assert result.score.value == 50.0
    assert result.score.events == 1


def test_h_duel_expectation_rewards_upset_and_punishes_bad_loss():
    win = compute_skills(
        [_duel_event("duel:1:a", result="win", player_skills=40, opponent_skills=80, margin=1.0)],
        level=1,
        xp_total=0,
        config=CONFIG,
    )
    loss = compute_skills(
        [_duel_event("duel:2:a", result="loss", player_skills=80, opponent_skills=40, margin=0.0)],
        level=1,
        xp_total=0,
        config=CONFIG,
    )
    assert win.duel.value > 50.0
    assert loss.duel.value < 50.0
    assert win.duel.events == 1
    assert win.duel_matches == 0  # no all-time duel record passed


def test_i_duel_accuracy_reuses_duel_rules_formula():
    match = DuelMatch(
        id=uuid.uuid4(),
        song_id=uuid.uuid4(),
        mode="online",
        status="playing",
        seed=1,
        note_count=10,
    )
    own = DuelParticipant(
        match_id=match.id,
        user_id=uuid.uuid4(),
        side="player",
        is_bot=False,
        perfect_count=8,
        great_count=1,
        good_count=1,
        notes_played=10,
        score_points=955,
        rating_before=1000,
        result="win",
    )
    other = DuelParticipant(
        match_id=match.id,
        user_id=uuid.uuid4(),
        side="opponent",
        is_bot=False,
        perfect_count=5,
        great_count=5,
        good_count=0,
        notes_played=10,
        score_points=925,
        rating_before=1000,
        result="loss",
    )
    values = guitar_skills._duel_event_values(
        match=match,
        own=own,
        other=other,
        own_level=10,
        other_level=12,
        song_difficulty=6,
        occurred_at=NOW,
    )
    # (8 + 1*0.85 + 1*0.70) / 10 = 0.955 -> 95.5%.
    assert values["accuracy"] == pytest.approx(95.5, abs=0.0001)
    assert values["score"] == 955
    assert values["note_count"] == 10
    assert values["result"] == "win"
    assert values["score_margin"] == pytest.approx(955 / (955 + 925), abs=1e-6)


def test_j_progression_uses_fixed_level_50_xp_reference():
    reference = 344_756
    result = compute_skills(
        [],
        level=25,
        xp_total=reference // 2,
        config=SkillsConfig(median_note_count=100, xp_reference=reference),
    )
    # P = 70% * 50 + 30% * 50 = 50; the other four components are neutral.
    assert result.progression.value == 50.0
    assert result.total == 50.0


# ---------------------------------------------------------------------------
# Solo finish integration
# ---------------------------------------------------------------------------
def _create_session(client, headers, song_id, *, practice: bool = False):
    response = client.post(
        f"{helpers.API}/games/sessions",
        json={"song_id": song_id, "speed_percent": 100, "practice": practice},
        headers=headers,
    )
    assert response.status_code == 201, response.text
    return response.json()


def _finish(client, headers, session_id, **overrides):
    payload = {
        "score_points": 9000,
        "accuracy_percent": 90.0,
        "max_combo": 50,
        "perfect_count": 80,
        "good_count": 10,
        "miss_count": 10,
        "completion_percent": 100.0,
        "notes_total": 100,
        "notes_played": 100,
    }
    payload.update(overrides)
    return client.post(
        f"{helpers.API}/games/sessions/{session_id}/finish",
        json=payload,
        headers=headers,
    )


def _events_for(db_session, user_id: str) -> list[GuitarSkillsEvent]:
    db_session.expire_all()
    return list(
        db_session.scalars(
            select(GuitarSkillsEvent)
            .where(GuitarSkillsEvent.user_id == uuid.UUID(user_id))
            .order_by(GuitarSkillsEvent.id)
        ).all()
    )


def test_ranked_solo_finish_creates_event_and_skills(
    db_client, db_session, user_account, published_song
):
    session = _create_session(
        db_client, user_account["headers"], published_song["id"]
    )
    response = _finish(db_client, user_account["headers"], session["id"])
    assert response.status_code == 200, response.text

    events = _events_for(db_session, user_account["id"])
    assert len(events) == 1
    assert events[0].event_key == f"solo:{session['id']}"
    assert events[0].score == 9000
    assert events[0].note_count == 100
    assert events[0].difficulty == int(published_song["difficulty"])

    row = db_session.get(UserGuitarSkills, uuid.UUID(user_account["id"]))
    assert row is not None
    assert float(row.guitar_skills) > 0.0
    assert row.events_counted == 1
    assert row.provisional is True

    body = db_client.get(
        f"{helpers.API}/leaderboard/global?limit=100"
    ).json()
    item = next(
        item for item in body["items"] if item["user_id"] == user_account["id"]
    )
    assert item["guitar_skills"] == float(row.guitar_skills)
    assert item["rating_points"] >= 9000
    assert item["games_played"] >= 1
    assert "components" not in item

    detail = db_client.get(
        f"{helpers.API}/leaderboard/global/{user_account['id']}"
    )
    assert detail.status_code == 200, detail.text
    payload = detail.json()
    assert payload["components"]["accuracy"]["value"] > 0
    assert payload["calculation_version"] >= 1

    missing = db_client.get(f"{helpers.API}/leaderboard/global/{uuid.uuid4()}")
    assert missing.status_code == 404
    assert missing.json()["detail"] == "Player not found"


def test_practice_and_low_completion_do_not_create_events(
    db_client, db_session, user_account, published_song
):
    practice = _create_session(
        db_client, user_account["headers"], published_song["id"], practice=True
    )
    assert _finish(db_client, user_account["headers"], practice["id"]).status_code == 200

    low = _create_session(
        db_client, user_account["headers"], published_song["id"]
    )
    assert (
        _finish(
            db_client,
            user_account["headers"],
            low["id"],
            completion_percent=50.0,
            notes_played=50,
            score_points=4000,
        ).status_code
        == 200
    )

    assert _events_for(db_session, user_account["id"]) == []


def test_global_leaderboard_orders_by_guitar_skills(
    db_client, db_session, user_account, second_user, published_song
):
    for account, score in ((user_account, 4000), (second_user, 9000)):
        session = _create_session(
            db_client, account["headers"], published_song["id"]
        )
        response = _finish(
            db_client,
            account["headers"],
            session["id"],
            score_points=score,
            accuracy_percent=80.0 if score == 4000 else 95.0,
        )
        assert response.status_code == 200, response.text

    body = db_client.get(f"{helpers.API}/leaderboard/global?limit=100").json()
    values = [item["guitar_skills"] for item in body["items"]]
    assert values == sorted(values, reverse=True)
    assert body["total"] >= 2
    assert body["calculated_at"] is not None

    top = body["items"][0]
    assert top["user_id"] == second_user["id"]
    for field in (
        "guitar_skills",
        "accuracy_score",
        "challenge_score",
        "score_performance",
        "duel_performance",
        "progression_score",
        "confidence",
        "provisional",
        "events_counted",
        "rating_points",
        "games_played",
        "best_accuracy",
    ):
        assert field in top


# ---------------------------------------------------------------------------
# Social additive fields
# ---------------------------------------------------------------------------
def test_social_card_and_leaderboard_expose_guitar_skills(
    db_client, user_account, second_user, published_song
):
    session = _create_session(
        db_client, user_account["headers"], published_song["id"]
    )
    assert _finish(db_client, user_account["headers"], session["id"]).status_code == 200

    card = db_client.get(
        f"{helpers.API}/social/players/{user_account['id']}",
        headers=user_account["headers"],
    )
    assert card.status_code == 200, card.text
    assert card.json()["guitar_skills"] is not None
    assert card.json()["guitar_skills_rank"] is not None
    assert card.json()["provisional"] is True

    alias = db_client.get(
        f"{helpers.API}/social/players/{user_account['id']}/card",
        headers=user_account["headers"],
    )
    assert alias.status_code == 200, alias.text
    assert alias.json()["guitar_skills"] == card.json()["guitar_skills"]

    board = db_client.get(
        f"{helpers.API}/social/leaderboard?sort=guitar_skills",
        headers=user_account["headers"],
    )
    assert board.status_code == 200, board.text
    body = board.json()
    assert body["sort"] == "guitar_skills"
    assert all("guitar_skills" in item for item in body["items"])


# ---------------------------------------------------------------------------
# Admin endpoints
# ---------------------------------------------------------------------------
def test_admin_config_validation_and_history(db_client, admin_user):
    response = db_client.get(
        f"{helpers.API}/admin/guitar-skills/config",
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    config = response.json()
    assert config["accuracy_weight"] == 0.35
    assert config["xp_reference"] == 344_756

    bad = db_client.put(
        f"{helpers.API}/admin/guitar-skills/config",
        json={
            "accuracy_weight": 0.5,
            "challenge_weight": 0.2,
            "score_weight": 0.1,
            "duel_weight": 0.25,
            "progression_weight": 0.1,
            "recalculate": False,
        },
        headers=admin_user["headers"],
    )
    assert bad.status_code == 422, bad.text

    good = db_client.put(
        f"{helpers.API}/admin/guitar-skills/config",
        json={"window_size": 15, "recalculate": False},
        headers=admin_user["headers"],
    )
    assert good.status_code == 200, good.text
    assert good.json()["window_size"] == 15
    assert good.json()["calculation_version"] == 2

    history = db_client.get(
        f"{helpers.API}/admin/guitar-skills/config/history",
        headers=admin_user["headers"],
    )
    assert history.status_code == 200, history.text
    assert history.json()["items"]
    assert history.json()["items"][0]["config"]["window_size"] == 15

    forbidden = db_client.get(f"{helpers.API}/admin/guitar-skills/config")
    assert forbidden.status_code == 401


def test_admin_dry_run_recalculation_does_not_write(
    db_client, db_session, admin_user, user_account, published_song
):
    session = _create_session(
        db_client, user_account["headers"], published_song["id"]
    )
    assert _finish(db_client, user_account["headers"], session["id"]).status_code == 200

    before = db_session.get(UserGuitarSkills, uuid.UUID(user_account["id"]))
    before_value = float(before.guitar_skills)
    before_history = int(
        db_session.scalar(select(func.count(GuitarSkillsHistory.id))) or 0
    )

    response = db_client.post(
        f"{helpers.API}/admin/guitar-skills/recalculate",
        json={"dry_run": True, "reason": "test"},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["users"] >= 2
    assert body["items"]

    db_session.expire_all()
    after = db_session.get(UserGuitarSkills, uuid.UUID(user_account["id"]))
    after_history = int(
        db_session.scalar(select(func.count(GuitarSkillsHistory.id))) or 0
    )
    assert float(after.guitar_skills) == before_value
    assert after_history == before_history

    status_body = db_client.get(
        f"{helpers.API}/admin/guitar-skills/status",
        headers=admin_user["headers"],
    )
    assert status_body.status_code == 200, status_body.text
    assert status_body.json()["users"] >= 1


# ---------------------------------------------------------------------------
# Backfill and duel finalisation idempotency
# ---------------------------------------------------------------------------
def test_backfill_events_is_idempotent(db_client, db_session, user_account, published_song):
    session = _create_session(
        db_client, user_account["headers"], published_song["id"]
    )
    assert _finish(db_client, user_account["headers"], session["id"]).status_code == 200

    first = guitar_skills.backfill_events(db_session)
    second = guitar_skills.backfill_events(db_session)
    assert first["total"] == 0  # the finish already inserted it
    assert second["total"] == 0
    assert len(_events_for(db_session, user_account["id"])) == 1

    guitar_skills.recalculate_all(db_session, reason="test")
    history_once = int(
        db_session.scalar(select(func.count(GuitarSkillsHistory.id))) or 0
    )
    guitar_skills.recalculate_all(db_session, reason="test")
    history_twice = int(
        db_session.scalar(select(func.count(GuitarSkillsHistory.id))) or 0
    )
    assert history_twice == history_once


def _playing_match(db_session, song: Song, first: User, second: User) -> DuelMatch:
    match = DuelMatch(
        song_id=song.id,
        mode="online",
        status="playing",
        rules_version=3,
        seed=1,
        note_count=100,
        started_at=NOW,
    )
    db_session.add(match)
    db_session.flush()
    db_session.add_all(
        [
            DuelParticipant(
                match_id=match.id,
                user_id=first.id,
                side="player",
                is_bot=False,
                rating_before=1000,
                # The duel winner is decided by ``hp_final`` (never by
                # ``score_points``), so give the fixture a decisive result.
                hp_final=55.0,
                perfect_count=80,
                great_count=10,
                good_count=10,
                notes_played=100,
                score_points=9000,
            ),
            DuelParticipant(
                match_id=match.id,
                user_id=second.id,
                side="opponent",
                is_bot=False,
                rating_before=1000,
                hp_final=10.0,
                perfect_count=60,
                great_count=20,
                good_count=20,
                notes_played=100,
                score_points=8000,
            ),
        ]
    )
    db_session.commit()
    return match


def test_duel_finalisation_freezes_skills_and_is_idempotent(
    db_client, db_session, user_account, second_user, published_song
):
    first = db_session.get(User, uuid.UUID(user_account["id"]))
    second = db_session.get(User, uuid.UUID(second_user["id"]))
    song = db_session.get(Song, uuid.UUID(published_song["id"]))
    match = _playing_match(db_session, song, first, second)

    participants = {
        row.side: row
        for row in db_session.scalars(
            select(DuelParticipant).where(DuelParticipant.match_id == match.id)
        ).all()
    }
    assert duel_service.finalize_match(
        db_session, match, participants=participants, resolve_remaining=False, now=NOW
    )
    db_session.commit()

    events = list(
        db_session.scalars(
            select(GuitarSkillsEvent).where(GuitarSkillsEvent.match_id == match.id)
        ).all()
    )
    assert len(events) == 2
    assert {event.event_key for event in events} == {
        f"duel:{match.id}:{first.id}",
        f"duel:{match.id}:{second.id}",
    }
    assert all(event.player_pre_match_guitar_skills is not None for event in events)
    assert all(event.opponent_pre_match_guitar_skills is not None for event in events)
    # Accuracy reuses ``duel_rules.accuracy_factor`` on the frozen counters:
    # first = (80 + 10*0.85 + 10*0.70) / 100 notes = 95.5%,
    # second = (60 + 20*0.85 + 20*0.70) / 100 notes = 91.0%.
    by_user = {event.user_id: event for event in events}
    assert by_user[first.id].accuracy == pytest.approx(95.5)
    assert by_user[second.id].accuracy == pytest.approx(91.0)

    first_skills = db_session.get(UserGuitarSkills, first.id)
    second_skills = db_session.get(UserGuitarSkills, second.id)
    assert first_skills is not None and second_skills is not None
    assert first_skills.duel_wins == 1
    assert second_skills.duel_losses == 1
    assert first_skills.duel_matches == 1

    # A retried finalisation must not duplicate events or history.
    assert not duel_service.finalize_match(
        db_session, match, participants=participants, resolve_remaining=False, now=NOW
    )
    # Even a forced re-finalisation (status forced back to ``playing``) only
    # hits the ``event_key`` idempotency guard: no duplicate events.
    match.status = "playing"
    db_session.commit()
    duel_service.finalize_match(
        db_session, match, participants=participants, resolve_remaining=False, now=NOW
    )
    db_session.commit()
    assert (
        int(
            db_session.scalar(
                select(func.count(GuitarSkillsEvent.id)).where(
                    GuitarSkillsEvent.match_id == match.id
                )
            )
            or 0
        )
        == 2
    )
