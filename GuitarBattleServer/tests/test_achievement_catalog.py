"""Achievement catalog display texts: English only, stable codes and rewards.

The catalog is seeded by migration ``0008_progression`` and re-synced on every
start by :func:`sync_achievement_catalog`, so a text change ships without a new
migration. What must not change is the identity of an achievement: ``code``,
``goal``, ``xp_bonus`` and ``category`` are pinned here.
"""
from __future__ import annotations

import pytest

from app.services.achievement_catalog import (
    ACHIEVEMENT_CATALOG,
    CATEGORY_ACCURACY,
    CATEGORY_DAILY,
    CATEGORY_GENRES,
    CATEGORY_MASTERY,
    CATEGORY_PROGRESS,
    CATEGORY_RHYTHM,
    CATEGORY_SONGS,
)
from tests import helpers

#: ``code -> (xp_bonus, goal, category)``. Changing a value here is a contract
#: change: the clients show the XP bonus and use ``goal`` for the progress bar.
EXPECTED_CATALOG: dict[str, tuple[int, int | None, str]] = {
    "first_notes": (20, None, CATEGORY_PROGRESS),
    "first_personal_best": (20, None, CATEGORY_PROGRESS),
    "songs_5": (30, 5, CATEGORY_SONGS),
    "songs_10": (50, 10, CATEGORY_SONGS),
    "songs_25": (100, 25, CATEGORY_SONGS),
    "songs_50": (150, 50, CATEGORY_SONGS),
    "accuracy_80": (25, 80, CATEGORY_ACCURACY),
    "accuracy_90": (50, 90, CATEGORY_ACCURACY),
    "accuracy_95": (100, 95, CATEGORY_ACCURACY),
    "clean_run": (75, None, CATEGORY_PROGRESS),
    "timing_40ms": (40, 40, CATEGORY_RHYTHM),
    "improve_10pp": (30, 10, CATEGORY_ACCURACY),
    "improve_5pp": (20, 5, CATEGORY_ACCURACY),
    "genres_3": (40, 3, CATEGORY_GENRES),
    "genres_5": (75, 5, CATEGORY_GENRES),
    "genres_8": (150, 8, CATEGORY_GENRES),
    "genre_master": (60, None, CATEGORY_GENRES),
    "mastery_2": (25, 2, CATEGORY_MASTERY),
    "mastery_3": (50, 3, CATEGORY_MASTERY),
    "mastery_4": (100, 4, CATEGORY_MASTERY),
    "mastery_5": (150, 5, CATEGORY_MASTERY),
    "hard_80": (75, 80, CATEGORY_ACCURACY),
    "daily_first": (30, None, CATEGORY_DAILY),
    "daily_streak_3": (50, 3, CATEGORY_DAILY),
    "daily_streak_7": (100, 7, CATEGORY_DAILY),
    "daily_score_70": (50, 70, CATEGORY_DAILY),
    "daily_first_place": (100, None, CATEGORY_DAILY),
}

EXPECTED_TITLES: dict[str, str] = {
    "first_notes": "First Notes",
    "first_personal_best": "First Personal Best",
    "songs_5": "Music Goes On",
    "songs_10": "Finding the Rhythm",
    "songs_25": "Regular Player",
    "songs_50": "Collector",
    "accuracy_80": "Accurate Strike",
    "accuracy_90": "Almost Perfect",
    "accuracy_95": "Flawless Performance",
    "clean_run": "Clean Performance",
    "timing_40ms": "Sense of Rhythm",
    "improve_10pp": "Getting Better",
    "improve_5pp": "New Record",
    "genres_3": "Variety Seeker",
    "genres_5": "Musical Explorer",
    "genres_8": "Genre Adventurer",
    "genre_master": "Genre Specialist",
    "mastery_2": "Learning the Song",
    "mastery_3": "Confident Performer",
    "mastery_4": "Professional Approach",
    "mastery_5": "Song Master",
    "hard_80": "Challenge Accepted",
    "daily_first": "Taking the Challenge",
    "daily_streak_3": "Three-Day Streak",
    "daily_streak_7": "Seven-Day Streak",
    "daily_score_70": "Daily High Score",
    "daily_first_place": "Daily Champion",
}


def _is_ascii(text: str) -> bool:
    return bool(text) and text.isascii()


def _has_cyrillic(text: str) -> bool:
    return any("\u0400" <= char <= "\u04ff" for char in text)


def _catalog_by_code() -> dict[str, dict]:
    return {item["code"]: item for item in ACHIEVEMENT_CATALOG}


def test_catalog_codes_goals_and_rewards_are_stable():
    by_code = _catalog_by_code()
    assert set(by_code) == set(EXPECTED_CATALOG)
    assert len(ACHIEVEMENT_CATALOG) == len(EXPECTED_CATALOG) == 27

    for code, (xp_bonus, goal, category) in EXPECTED_CATALOG.items():
        item = by_code[code]
        assert item["xp_bonus"] == xp_bonus, code
        assert item["goal"] == goal, code
        assert item["category"] == category, code


def test_catalog_display_texts_are_english_only():
    by_code = _catalog_by_code()
    for code, title in EXPECTED_TITLES.items():
        assert by_code[code]["title"] == title, code

    for item in ACHIEVEMENT_CATALOG:
        for field in ("title", "description"):
            text = item[field]
            assert _is_ascii(text), (item["code"], field, text)
            assert not _has_cyrillic(text), (item["code"], field, text)
        # Descriptions are short hints, never the title again.
        assert item["title"].lower() not in item["description"].lower(), item["code"]
        assert len(item["description"]) <= 90, (item["code"], item["description"])


@pytest.mark.db
def test_achievements_endpoint_returns_english_texts(db_client, user_account):
    response = db_client.get(
        f"{helpers.API}/achievements", headers=user_account["headers"]
    )
    assert response.status_code == 200, response.text
    payload = response.json()

    assert payload["total"] == len(EXPECTED_CATALOG) == 27
    assert len(payload["items"]) == 27

    seen: dict[str, dict] = {}
    for item in payload["items"]:
        code = item["code"]
        assert code in EXPECTED_CATALOG, code
        seen[code] = item
        for field in ("title", "description"):
            text = item[field]
            assert _is_ascii(text), (code, field, text)
            assert not _has_cyrillic(text), (code, field, text)
        assert item["title"] == EXPECTED_TITLES[code], code
        # Codes, rewards and categories did not move with the translation.
        xp_bonus, goal, category = EXPECTED_CATALOG[code]
        assert item["xp_bonus"] == xp_bonus, code
        assert item["goal"] == goal, code
        assert item["category"] == category, code

    assert set(seen) == set(EXPECTED_CATALOG)
