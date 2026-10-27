"""Achievement catalog: the single source of truth for ``achievements`` rows.

The catalog is static reference data. It is seeded by migration
``0008_progression`` (which imports this module, so the migration and the app can
never disagree) and re-synced on startup by :func:`sync_achievement_catalog`.

Displayed texts (``title``/``description``) are English only: the API is
language-neutral and the clients render them verbatim, so every string here must
stay ASCII (see ``tests/test_achievement_icons.py``).

Only metadata lives here. The unlock rule for every ``code`` is implemented in
:mod:`app.services.progression`; adding an achievement means adding a row here
*and* a branch in ``evaluate_achievement``.
"""
from __future__ import annotations

from typing import Any

from sqlalchemy import select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.orm import Session

from app.models.progression import Achievement

# Categories group achievements in the client UI. These are stable identifiers,
# not display names: translate them in the UI, never in the database.
CATEGORY_PROGRESS = "progress"
CATEGORY_SONGS = "songs"
CATEGORY_ACCURACY = "accuracy"
CATEGORY_RHYTHM = "rhythm"
CATEGORY_GENRES = "genres"
CATEGORY_MASTERY = "mastery"
CATEGORY_DAILY = "daily"

# ``goal`` is the numeric target for accumulating achievements; ``None`` means
# the achievement is a single event. ``sort_order`` makes the awarding order
# deterministic and is used when genre thresholds collapse onto one value.
# ``icon`` is the file name under ``app/static/media/achievements``; the API
# turns it into ``/media/achievements/<icon>`` for the website and the clients.
ACHIEVEMENT_CATALOG: list[dict[str, Any]] = [
    {
        "code": "first_notes",
        "title": "First Notes",
        "description": "Finish at least 50% of a song for the first time.",
        "category": CATEGORY_PROGRESS,
        "xp_bonus": 20,
        "goal": None,
        "icon": "first_notes.svg",
        "sort_order": 10,
    },
    {
        "code": "first_personal_best",
        "title": "First Personal Best",
        "description": "Beat your own score on any song for the first time.",
        "category": CATEGORY_PROGRESS,
        "xp_bonus": 20,
        "goal": None,
        "icon": "star.svg",
        "sort_order": 20,
    },
    {
        "code": "songs_5",
        "title": "Music Goes On",
        "description": "Set personal bests on 5 different songs.",
        "category": CATEGORY_SONGS,
        "xp_bonus": 30,
        "goal": 5,
        "icon": "notes.svg",
        "sort_order": 30,
    },
    {
        "code": "songs_10",
        "title": "Finding the Rhythm",
        "description": "Set personal bests on 10 different songs.",
        "category": CATEGORY_SONGS,
        "xp_bonus": 50,
        "goal": 10,
        "icon": "notes.svg",
        "sort_order": 40,
    },
    {
        "code": "songs_25",
        "title": "Regular Player",
        "description": "Set personal bests on 25 different songs.",
        "category": CATEGORY_SONGS,
        "xp_bonus": 100,
        "goal": 25,
        "icon": "notes.svg",
        "sort_order": 50,
    },
    {
        "code": "songs_50",
        "title": "Collector",
        "description": "Set personal bests on 50 different songs.",
        "category": CATEGORY_SONGS,
        "xp_bonus": 150,
        "goal": 50,
        "icon": "notes.svg",
        "sort_order": 60,
    },
    {
        "code": "accuracy_80",
        "title": "Accurate Strike",
        "description": "Score 80% accuracy or higher in a run.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 25,
        "goal": 80,
        "icon": "target.svg",
        "sort_order": 70,
    },
    {
        "code": "accuracy_90",
        "title": "Almost Perfect",
        "description": "Score 90% accuracy or higher in a run.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 50,
        "goal": 90,
        "icon": "accuracy.svg",
        "sort_order": 80,
    },
    {
        "code": "accuracy_95",
        "title": "Flawless Performance",
        "description": "Score 95% accuracy or higher in a run.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 100,
        "goal": 95,
        "icon": "star.svg",
        "sort_order": 90,
    },
    {
        "code": "clean_run",
        "title": "Clean Performance",
        "description": "Finish a song with no misses and at least 50% progress.",
        "category": CATEGORY_PROGRESS,
        "xp_bonus": 75,
        "goal": None,
        "icon": "accuracy.svg",
        "sort_order": 100,
    },
    {
        "code": "timing_40ms",
        "title": "Sense of Rhythm",
        "description": "Keep the average timing error at 40 ms or less.",
        "category": CATEGORY_RHYTHM,
        "xp_bonus": 40,
        "goal": 40,
        "icon": "metronome.svg",
        "sort_order": 110,
    },
    {
        "code": "improve_10pp",
        "title": "Getting Better",
        "description": "Improve your personal accuracy by 10 percentage points.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 30,
        "goal": 10,
        "icon": "star.svg",
        "sort_order": 120,
    },
    {
        "code": "improve_5pp",
        "title": "New Record",
        "description": "Improve your personal accuracy by 5 percentage points.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 20,
        "goal": 5,
        "icon": "accuracy.svg",
        "sort_order": 130,
    },
    {
        "code": "genres_3",
        "title": "Variety Seeker",
        "description": "Set personal bests in 3 different genres.",
        "category": CATEGORY_GENRES,
        "xp_bonus": 40,
        "goal": 3,
        "icon": "genres.svg",
        "sort_order": 140,
    },
    {
        "code": "genres_5",
        "title": "Musical Explorer",
        "description": "Set personal bests in 5 different genres.",
        "category": CATEGORY_GENRES,
        "xp_bonus": 75,
        "goal": 5,
        "icon": "genres.svg",
        "sort_order": 150,
    },
    {
        "code": "genres_8",
        "title": "Genre Adventurer",
        "description": "Set personal bests in 8 different genres.",
        "category": CATEGORY_GENRES,
        "xp_bonus": 150,
        "goal": 8,
        "icon": "genres.svg",
        "sort_order": 160,
    },
    {
        "code": "genre_master",
        "title": "Genre Specialist",
        "description": "Reach Pro mastery in any genre.",
        "category": CATEGORY_GENRES,
        "xp_bonus": 60,
        "goal": None,
        "icon": "diploma.svg",
        "sort_order": 170,
    },
    {
        "code": "mastery_2",
        "title": "Learning the Song",
        "description": "Reach the Learner tier on any song.",
        "category": CATEGORY_MASTERY,
        "xp_bonus": 25,
        "goal": 2,
        "icon": "first_notes.svg",
        "sort_order": 180,
    },
    {
        "code": "mastery_3",
        "title": "Confident Performer",
        "description": "Reach the Guitarist tier on any song.",
        "category": CATEGORY_MASTERY,
        "xp_bonus": 50,
        "goal": 3,
        "icon": "mastery.svg",
        "sort_order": 190,
    },
    {
        "code": "mastery_4",
        "title": "Professional Approach",
        "description": "Reach the Pro tier on any song.",
        "category": CATEGORY_MASTERY,
        "xp_bonus": 100,
        "goal": 4,
        "icon": "medal.svg",
        "sort_order": 200,
    },
    {
        "code": "mastery_5",
        "title": "Song Master",
        "description": "Reach the Master tier on any song.",
        "category": CATEGORY_MASTERY,
        "xp_bonus": 150,
        "goal": 5,
        "icon": "diploma.svg",
        "sort_order": 210,
    },
    {
        "code": "hard_80",
        "title": "Challenge Accepted",
        "description": "Score 80% accuracy on a song of difficulty 4 or higher.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 75,
        "goal": 80,
        "icon": "hard.svg",
        "sort_order": 220,
    },
    {
        "code": "daily_first",
        "title": "Taking the Challenge",
        "description": "Play your first official Daily Challenge attempt.",
        "category": CATEGORY_DAILY,
        "xp_bonus": 30,
        "goal": None,
        "icon": "daily.svg",
        "sort_order": 230,
    },
    {
        "code": "daily_streak_3",
        "title": "Three-Day Streak",
        "description": "Submit an official Daily result 3 days in a row.",
        "category": CATEGORY_DAILY,
        "xp_bonus": 50,
        "goal": 3,
        "icon": "clock.svg",
        "sort_order": 240,
    },
    {
        "code": "daily_streak_7",
        "title": "Seven-Day Streak",
        "description": "Submit an official Daily result 7 days in a row.",
        "category": CATEGORY_DAILY,
        "xp_bonus": 100,
        "goal": 7,
        "icon": "calendar.svg",
        "sort_order": 250,
    },
    {
        "code": "daily_score_70",
        "title": "Daily High Score",
        "description": "Score at least 70% accuracy in an official Daily attempt.",
        "category": CATEGORY_DAILY,
        "xp_bonus": 50,
        "goal": 70,
        "icon": "target.svg",
        "sort_order": 260,
    },
    {
        "code": "daily_first_place",
        "title": "Daily Champion",
        "description": "Finish first in an official Daily Challenge leaderboard.",
        "category": CATEGORY_DAILY,
        "xp_bonus": 100,
        "goal": None,
        "icon": "medal.svg",
        "sort_order": 270,
    },
]

ACHIEVEMENT_CODES = tuple(item["code"] for item in ACHIEVEMENT_CATALOG)
# Achievement lookup by code for the unlock rules.
CATALOG_BY_CODE = {item["code"]: item for item in ACHIEVEMENT_CATALOG}


def sync_achievement_catalog(db: Session) -> int:
    """Insert missing catalog rows and refresh titles/descriptions/xp.

    Idempotent: safe to call on every start. Player state
    (``user_achievements``) is never touched, and rows that disappeared from the
    code are deactivated instead of deleted so awarded history stays readable.
    """
    if not ACHIEVEMENT_CATALOG:
        return 0
    rows = [
        {**item, "is_active": True}
        for item in ACHIEVEMENT_CATALOG
    ]
    stmt = pg_insert(Achievement.__table__).values(rows)
    stmt = stmt.on_conflict_do_update(
        index_elements=["code"],
        set_={
            "title": stmt.excluded.title,
            "description": stmt.excluded.description,
            "category": stmt.excluded.category,
            "xp_bonus": stmt.excluded.xp_bonus,
            "goal": stmt.excluded.goal,
            "icon": stmt.excluded.icon,
            "sort_order": stmt.excluded.sort_order,
            "is_active": True,
        },
    )
    db.execute(stmt)
    active = [item["code"] for item in ACHIEVEMENT_CATALOG]
    db.execute(
        Achievement.__table__.update()
        .where(Achievement.code.not_in(active))
        .values(is_active=False)
    )
    db.commit()
    return len(rows)


def catalog_rows(db: Session) -> list[Achievement]:
    """The active catalog in deterministic awarding order."""
    return list(
        db.scalars(
            select(Achievement)
            .where(Achievement.is_active.is_(True))
            .order_by(Achievement.sort_order.asc(), Achievement.code.asc())
        ).all()
    )
