"""Administrator account card: duel/tower aggregates and character management.

Pinned here:

* the overview counts only finished *online human* duels (a bot match or a
  cancelled match never leaks into the numbers) and clamps cleared stages to
  the published stage count of each tower;
* grants are idempotent, starters can never be revoked (409) and revoking the
  selected character clears the selection;
* the users table carries the six compact summary fields for the current page.
"""
from __future__ import annotations

import uuid
from datetime import datetime, timedelta, timezone

import pytest
from sqlalchemy import select, update

from app.models.campaign import CampaignStageRun
from app.models.character_unlocks import UserCharacter
from app.models.duel import DuelMatch, DuelParticipant
from app.models.gameplay_visuals import UiCharacter
from app.models.user import User
from tests import helpers

pytestmark = pytest.mark.db

API = helpers.API
CHARACTERS_ADMIN = f"{API}/admin/ui-levels"
CAMPAIGNS_ADMIN = f"{API}/admin/campaigns"
USERS_ADMIN = f"{API}/admin/users"


def png_bytes(width: int = 64, height: int = 32) -> bytes:
    ihdr = (
        b"\x00\x00\x00\x0dIHDR"
        + width.to_bytes(4, "big")
        + height.to_bytes(4, "big")
        + b"\x08\x06\x00\x00\x00"
        + b"\x00\x00\x00\x00"
    )
    return b"\x89PNG\r\n\x1a\n" + ihdr


def create_character(client, headers, *, name: str) -> dict:
    response = client.post(
        CHARACTERS_ADMIN,
        data={"name": name},
        files={"file": ("sheet.png", png_bytes(), "image/png")},
        headers=headers,
    )
    assert response.status_code == 201, response.text
    return response.json()


def mark_starters(db_session, *names: str) -> None:
    db_session.execute(
        update(UiCharacter)
        .where(UiCharacter.name.in_(list(names)))
        .values(is_starter=True)
    )
    db_session.commit()


def seed_characters(client, db_session, admin_headers) -> dict[str, dict]:
    """Ray + Luna are starters; Jackson and Bard are tower rewards.

    The admin upload response keeps the internal UUID under ``id`` and does not
    expose the player-facing integer, so the tests read it from the database.
    """
    characters: dict[str, dict] = {}
    for name in ("Ray", "Luna", "Jackson", "Bard"):
        created = create_character(client, admin_headers, name=name)
        row = db_session.get(UiCharacter, uuid.UUID(created["id"]))
        assert row is not None
        characters[name] = {**created, "public_id": row.public_id}
    mark_starters(db_session, "Ray", "Luna")
    return characters


def create_tower(client, admin_headers, *, stage_count: int, name: str | None = None) -> dict:
    created = client.post(
        CAMPAIGNS_ADMIN,
        json={"name": name or "Tower " + uuid.uuid4().hex[:8], "bot_level": 1},
        headers=admin_headers,
    )
    assert created.status_code == 201, created.text
    campaign = created.json()

    songs = []
    for index in range(stage_count):
        song = helpers.create_song(
            client, admin_headers, title=f"Stage Song {index} {uuid.uuid4().hex[:6]}"
        )
        helpers.publish_song(client, admin_headers, song["id"])
        songs.append(song)

    updated = client.put(
        f"{CAMPAIGNS_ADMIN}/{campaign['id']}/songs",
        json={"songs": [{"song_id": song["id"]} for song in songs]},
        headers=admin_headers,
    )
    assert updated.status_code == 200, updated.text
    return updated.json()


def report_progress(client, headers, campaign_id: str, best_stage: int):
    return client.post(
        f"{API}/campaigns/{campaign_id}/progress",
        json={"best_stage": best_stage},
        headers=headers,
    )


def _add_duel(
    db_session,
    *,
    song_id: uuid.UUID,
    user_id: uuid.UUID,
    opponent_id: uuid.UUID | None,
    result: str,
    mode: str = "online",
    status: str = "finished",
    finished_at: datetime | None = None,
    rating_after: int | None = None,
    rating_before: int | None = None,
    xp_awarded: int = 0,
    perfect: int = 0,
    great: int = 0,
    good: int = 0,
    miss: int = 0,
    notes: int = 0,
    damage: float = 0.0,
) -> DuelMatch:
    """Insert one finished match with a human (online) or bot (bot) opponent."""
    match = DuelMatch(
        song_id=song_id,
        mode=mode,
        bot_level="normal" if mode == "bot" else None,
        status=status,
        seed=1,
        note_count=max(notes, 1),
        started_at=finished_at,
        finished_at=finished_at if status == "finished" else None,
        winner_user_id=user_id if result == "win" else opponent_id,
    )
    db_session.add(match)
    db_session.flush()

    db_session.add(
        DuelParticipant(
            match_id=match.id,
            user_id=user_id,
            side="player",
            is_bot=False,
            result=result,
            rating_before=rating_before,
            rating_after=rating_after,
            xp_awarded=xp_awarded,
            perfect_count=perfect,
            great_count=great,
            good_count=good,
            miss_count=miss,
            notes_played=notes,
            damage_dealt=damage,
        )
    )
    other_result = {"win": "loss", "loss": "win", "draw": "draw"}[result]
    db_session.add(
        DuelParticipant(
            match_id=match.id,
            user_id=opponent_id if mode == "online" else None,
            side="opponent",
            is_bot=mode == "bot",
            bot_name=None if mode == "online" else "Bot",
            result=other_result,
            notes_played=notes,
        )
    )
    db_session.commit()
    return match


# ---------------------------------------------------------------------------
# Overview aggregates
# ---------------------------------------------------------------------------
def test_overview_aggregates_online_duels_and_battle(
    db_client, db_session, admin_user, published_song
):
    account = helpers.register_user(db_client)
    user_id = uuid.UUID(account["id"])
    other = helpers.register_user(db_client)
    other_id = uuid.UUID(other["id"])
    song_id = uuid.UUID(published_song["id"])
    now = datetime.now(timezone.utc)

    # A win (older) and a loss (newer, so it carries the current rating).
    _add_duel(
        db_session,
        song_id=song_id,
        user_id=user_id,
        opponent_id=other_id,
        result="win",
        finished_at=now - timedelta(hours=3),
        rating_before=1200,
        rating_after=1216,
        xp_awarded=40,
        perfect=80,
        great=10,
        miss=10,
        notes=100,
        damage=40.0,
    )
    _add_duel(
        db_session,
        song_id=song_id,
        user_id=user_id,
        opponent_id=other_id,
        result="loss",
        finished_at=now - timedelta(hours=1),
        rating_before=1216,
        rating_after=1200,
        xp_awarded=10,
        perfect=50,
        great=20,
        miss=30,
        notes=100,
        damage=15.0,
    )
    # Neither of these may be counted: a bot match and a cancelled online match.
    _add_duel(
        db_session,
        song_id=song_id,
        user_id=user_id,
        opponent_id=None,
        result="win",
        mode="bot",
        finished_at=now - timedelta(hours=2),
        xp_awarded=999,
        perfect=100,
        notes=100,
        damage=100.0,
    )
    _add_duel(
        db_session,
        song_id=song_id,
        user_id=user_id,
        opponent_id=other_id,
        result="win",
        status="cancelled",
        finished_at=now - timedelta(minutes=30),
        xp_awarded=999,
        notes=100,
    )

    # Two towers: one fully cleared (2 stages), one in progress (3 stages).
    first = create_tower(db_client, admin_user["headers"], stage_count=2)
    second = create_tower(db_client, admin_user["headers"], stage_count=3)
    assert report_progress(db_client, account["headers"], first["id"], 2).status_code == 200
    assert report_progress(db_client, account["headers"], second["id"], 1).status_code == 200

    db_session.add_all(
        [
            CampaignStageRun(
                user_id=user_id,
                campaign_id=uuid.UUID(first["id"]),
                position=index,
                run_id=uuid.uuid4(),
                score_points=100,
                accuracy_percent=90.0,
                won=index < 2,
                xp_awarded=xp,
                created_at=created_at,
            )
            for index, (xp, created_at) in enumerate(
                [
                    (10, now - timedelta(minutes=30)),
                    (20, now - timedelta(minutes=20)),
                    (30, now - timedelta(minutes=10)),
                ]
            )
        ]
    )
    db_session.commit()

    response = db_client.get(
        f"{USERS_ADMIN}/{account['id']}/overview", headers=admin_user["headers"]
    )
    assert response.status_code == 200, response.text
    payload = response.json()

    basics = payload["basics"]
    assert basics["user_id"] == account["id"]
    assert basics["username"] == account["username"]
    assert basics["email"] == account["email"]
    assert basics["is_active"] is True
    assert basics["level"] >= 1
    assert basics["selected_character"] is None

    online = payload["online"]
    assert online["matches"] == 2
    assert online["wins"] == 1
    assert online["losses"] == 1
    assert online["draws"] == 0
    assert online["win_rate"] == 50.0
    assert online["rating"] == 1200
    assert online["best_win_streak"] == 1
    # (8000 + 850 + 5000 + 1700) / (2 * 100 * 100) * 100.
    assert online["accuracy"] == 77.75
    assert online["damage_dealt"] == 55.0
    assert online["xp_earned"] == 50
    assert datetime.fromisoformat(
        online["last_match_at"].replace("Z", "+00:00")
    ) == now - timedelta(hours=1)

    battle = payload["battle"]
    assert battle["towers_completed"] == 1
    assert battle["stages_cleared"] == 3
    assert battle["runs"] == 3
    assert battle["xp_earned"] == 60
    assert battle["last_run_at"] is not None
    towers = {tower["campaign_id"]: tower for tower in battle["towers"]}
    assert set(towers) == {first["id"], second["id"]}
    assert towers[first["id"]]["best_stage"] == 2
    assert towers[first["id"]]["stage_count"] == 2
    assert towers[first["id"]]["completed_at"] is not None
    assert towers[second["id"]]["best_stage"] == 1
    assert towers[second["id"]]["stage_count"] == 3
    assert towers[second["id"]]["completions"] == 0
    assert towers[second["id"]]["completed_at"] is None
    # Alphabetical order by tower name, not by id.
    names = [tower["name"] for tower in battle["towers"]]
    assert names == sorted(names, key=str.casefold)

    characters = payload["characters"]
    assert characters["owned"] == []
    assert characters["available"] == []


def test_overview_without_activity_is_zeroed(db_client, admin_user):
    account = helpers.register_user(db_client)
    response = db_client.get(
        f"{USERS_ADMIN}/{account['id']}/overview", headers=admin_user["headers"]
    )
    assert response.status_code == 200, response.text
    payload = response.json()
    assert payload["online"]["matches"] == 0
    assert payload["online"]["rating"] is None
    assert payload["online"]["accuracy"] == 0.0
    assert payload["online"]["last_match_at"] is None
    assert payload["battle"]["towers"] == []
    assert payload["battle"]["runs"] == 0
    assert payload["characters"]["owned"] == []
    assert payload["characters"]["selected_character_id"] is None


# ---------------------------------------------------------------------------
# Characters
# ---------------------------------------------------------------------------
def test_character_grant_and_revoke_rules(
    db_client, db_session, admin_user, user_account
):
    characters = seed_characters(db_client, db_session, admin_user["headers"])
    user_id = uuid.UUID(user_account["id"])
    jackson = characters["Jackson"]

    overview = db_client.get(
        f"{USERS_ADMIN}/{user_account['id']}/overview", headers=admin_user["headers"]
    )
    assert overview.status_code == 200, overview.text
    block = overview.json()["characters"]
    owned = {item["name"]: item for item in block["owned"]}
    assert set(owned) == {"Ray", "Luna"}
    assert owned["Ray"]["can_remove"] is False
    assert owned["Luna"]["can_remove"] is False
    assert owned["Ray"]["unlocked_at"] is None
    assert {item["name"] for item in block["available"]} == {
        "Ray",
        "Luna",
        "Jackson",
        "Bard",
    }
    assert block["selected_character_id"] is None

    # Grant: 201 the first time, 200 on every repeat (idempotent).
    granted = db_client.post(
        f"{USERS_ADMIN}/{user_account['id']}/characters/{jackson['id']}",
        headers=admin_user["headers"],
    )
    assert granted.status_code == 201, granted.text
    assert granted.json()["created"] is True
    assert granted.json()["character"]["name"] == "Jackson"
    assert granted.json()["character"]["can_remove"] is True

    repeated = db_client.post(
        f"{USERS_ADMIN}/{user_account['id']}/characters/{jackson['id']}",
        headers=admin_user["headers"],
    )
    assert repeated.status_code == 200, repeated.text
    assert repeated.json()["created"] is False

    rows = db_session.scalars(
        select(UserCharacter).where(UserCharacter.user_id == user_id)
    ).all()
    assert len(rows) == 1

    # A starter is always owned: granting one is a no-op and writes no row.
    starter_grant = db_client.post(
        f"{USERS_ADMIN}/{user_account['id']}/characters/{characters['Ray']['id']}",
        headers=admin_user["headers"],
    )
    assert starter_grant.status_code == 200, starter_grant.text
    assert starter_grant.json()["created"] is False
    assert (
        len(
            db_session.scalars(
                select(UserCharacter).where(UserCharacter.user_id == user_id)
            ).all()
        )
        == 1
    )

    # Selecting the granted character, then revoking it, clears the selection.
    user = db_session.get(User, user_id)
    user.character_id = uuid.UUID(jackson["id"])
    db_session.commit()

    selected = db_client.get(
        f"{USERS_ADMIN}/{user_account['id']}/overview", headers=admin_user["headers"]
    ).json()
    assert selected["characters"]["selected_character_id"] == jackson["public_id"]
    assert selected["basics"]["selected_character"]["name"] == "Jackson"

    revoked = db_client.delete(
        f"{USERS_ADMIN}/{user_account['id']}/characters/{jackson['id']}",
        headers=admin_user["headers"],
    )
    assert revoked.status_code == 200, revoked.text
    assert revoked.json()["removed"] is True
    assert revoked.json()["cleared_selected"] is True
    assert revoked.json()["selected_character_id"] is None
    db_session.expire_all()
    assert db_session.get(User, user_id).character_id is None

    after = db_client.get(
        f"{USERS_ADMIN}/{user_account['id']}/overview", headers=admin_user["headers"]
    ).json()["characters"]
    assert "Jackson" not in {item["name"] for item in after["owned"]}
    assert after["selected_character_id"] is None

    # Revoking again is a 404; starters are 409; unknown ids are 404.
    again = db_client.delete(
        f"{USERS_ADMIN}/{user_account['id']}/characters/{jackson['id']}",
        headers=admin_user["headers"],
    )
    assert again.status_code == 404
    assert again.json()["detail"] == "not_owned"

    starter = db_client.delete(
        f"{USERS_ADMIN}/{user_account['id']}/characters/{characters['Luna']['id']}",
        headers=admin_user["headers"],
    )
    assert starter.status_code == 409
    assert starter.json()["detail"] == "starter_character"

    missing_character = db_client.post(
        f"{USERS_ADMIN}/{user_account['id']}/characters/{uuid.uuid4()}",
        headers=admin_user["headers"],
    )
    assert missing_character.status_code == 404
    assert missing_character.json()["detail"] == "character_not_found"

    missing_user = db_client.get(
        f"{USERS_ADMIN}/{uuid.uuid4()}/overview", headers=admin_user["headers"]
    )
    assert missing_user.status_code == 404
    assert missing_user.json()["detail"] == "user_not_found"

    missing_user_grant = db_client.post(
        f"{USERS_ADMIN}/{uuid.uuid4()}/characters/{jackson['id']}",
        headers=admin_user["headers"],
    )
    assert missing_user_grant.status_code == 404
    assert missing_user_grant.json()["detail"] == "user_not_found"


def test_revoke_keeps_another_selected_character(
    db_client, db_session, admin_user, user_account
):
    characters = seed_characters(db_client, db_session, admin_user["headers"])
    user_id = uuid.UUID(user_account["id"])
    bard = characters["Bard"]
    jackson = characters["Jackson"]

    for character in (bard, jackson):
        response = db_client.post(
            f"{USERS_ADMIN}/{user_account['id']}/characters/{character['id']}",
            headers=admin_user["headers"],
        )
        assert response.status_code == 201, response.text

    user = db_session.get(User, user_id)
    user.character_id = uuid.UUID(bard["id"])
    db_session.commit()

    revoked = db_client.delete(
        f"{USERS_ADMIN}/{user_account['id']}/characters/{jackson['id']}",
        headers=admin_user["headers"],
    )
    assert revoked.status_code == 200, revoked.text
    assert revoked.json()["cleared_selected"] is False
    assert revoked.json()["selected_character_id"] == bard["public_id"]
    db_session.expire_all()
    assert db_session.get(User, user_id).character_id == uuid.UUID(bard["id"])


def test_admin_user_endpoints_require_admin(db_client, user_account):
    character_id = uuid.uuid4()
    user_id = user_account["id"]

    for headers, expected in ((None, 401), (user_account["headers"], 403)):
        overview = db_client.get(
            f"{USERS_ADMIN}/{user_id}/overview", headers=headers or {}
        )
        assert overview.status_code == expected, overview.text

        grant = db_client.post(
            f"{USERS_ADMIN}/{user_id}/characters/{character_id}",
            headers=headers or {},
        )
        assert grant.status_code == expected, grant.text

        revoke = db_client.delete(
            f"{USERS_ADMIN}/{user_id}/characters/{character_id}",
            headers=headers or {},
        )
        assert revoke.status_code == expected, revoke.text


# ---------------------------------------------------------------------------
# Users table summary
# ---------------------------------------------------------------------------
def test_statistics_users_carry_duel_and_battle_summary(
    db_client, db_session, admin_user, published_song
):
    account = helpers.register_user(db_client)
    user_id = uuid.UUID(account["id"])
    other = helpers.register_user(db_client)
    other_id = uuid.UUID(other["id"])
    song_id = uuid.UUID(published_song["id"])
    now = datetime.now(timezone.utc)

    _add_duel(
        db_session,
        song_id=song_id,
        user_id=user_id,
        opponent_id=other_id,
        result="win",
        finished_at=now - timedelta(hours=2),
        rating_after=1030,
        xp_awarded=20,
        notes=100,
        perfect=100,
    )
    _add_duel(
        db_session,
        song_id=song_id,
        user_id=user_id,
        opponent_id=other_id,
        result="loss",
        finished_at=now - timedelta(hours=1),
        rating_after=1014,
        xp_awarded=5,
        notes=100,
        perfect=60,
        miss=40,
    )

    tower = create_tower(db_client, admin_user["headers"], stage_count=3)
    assert report_progress(db_client, account["headers"], tower["id"], 2).status_code == 200

    response = db_client.get(
        f"{API}/admin/statistics/users", headers=admin_user["headers"]
    )
    assert response.status_code == 200, response.text
    payload = response.json()
    row = next(item for item in payload["items"] if item["user_id"] == account["id"])

    # Additive fields.
    assert row["online_rating"] == 1014
    assert row["online_matches"] == 2
    assert row["online_wins"] == 1
    assert row["online_losses"] == 1
    assert row["battle_towers_completed"] == 0
    assert row["battle_stages_cleared"] == 2

    # The pre-existing fields keep their shape for older admin clients.
    for key in (
        "app_seconds_total",
        "sessions_count",
        "launches_count",
        "scores_count",
        "level",
        "xp_total",
        "platforms",
        "country_code",
    ):
        assert key in row

    admin_row = next(
        item for item in payload["items"] if item["user_id"] == admin_user["id"]
    )
    assert admin_row["online_rating"] is None
    assert admin_row["online_matches"] == 0
    assert admin_row["online_wins"] == 0
    assert admin_row["online_losses"] == 0
    assert admin_row["battle_towers_completed"] == 0
    assert admin_row["battle_stages_cleared"] == 0
