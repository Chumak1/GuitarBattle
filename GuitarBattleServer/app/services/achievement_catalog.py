"""Achievement catalog: the single source of truth for ``achievements`` rows.

The catalog is static reference data. It is seeded by migration
``0008_progression`` (which imports this module, so the migration and the app can
never disagree) and re-synced on startup by :func:`sync_achievement_catalog`.

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

# Categories group achievements in the client UI.
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
        "title": "Первые ноты",
        "description": "Завершить первую попытку с прогрессом не ниже 50%.",
        "category": CATEGORY_PROGRESS,
        "xp_bonus": 20,
        "goal": None,
        "icon": "first_notes.svg",
        "sort_order": 10,
    },
    {
        "code": "first_personal_best",
        "title": "Первая победа над собой",
        "description": "Первый личный рекорд на любой песне.",
        "category": CATEGORY_PROGRESS,
        "xp_bonus": 20,
        "goal": None,
        "icon": "star.svg",
        "sort_order": 20,
    },
    {
        "code": "songs_5",
        "title": "Пять песен",
        "description": "Пять разных песен с личным рекордом.",
        "category": CATEGORY_SONGS,
        "xp_bonus": 30,
        "goal": 5,
        "icon": "notes.svg",
        "sort_order": 30,
    },
    {
        "code": "songs_10",
        "title": "Десять песен",
        "description": "Десять разных песен с личным рекордом.",
        "category": CATEGORY_SONGS,
        "xp_bonus": 50,
        "goal": 10,
        "icon": "notes.svg",
        "sort_order": 40,
    },
    {
        "code": "songs_25",
        "title": "Двадцать пять песен",
        "description": "Двадцать пять разных песен с личным рекордом.",
        "category": CATEGORY_SONGS,
        "xp_bonus": 100,
        "goal": 25,
        "icon": "notes.svg",
        "sort_order": 50,
    },
    {
        "code": "songs_50",
        "title": "Пятьдесят песен",
        "description": "Пятьдесят разных песен с личным рекордом.",
        "category": CATEGORY_SONGS,
        "xp_bonus": 150,
        "goal": 50,
        "icon": "notes.svg",
        "sort_order": 60,
    },
    {
        "code": "accuracy_80",
        "title": "Точность 80%",
        "description": "Завершить попытку с точностью не ниже 80%.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 25,
        "goal": 80,
        "icon": "target.svg",
        "sort_order": 70,
    },
    {
        "code": "accuracy_90",
        "title": "Точность 90%",
        "description": "Завершить попытку с точностью не ниже 90%.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 50,
        "goal": 90,
        "icon": "accuracy.svg",
        "sort_order": 80,
    },
    {
        "code": "accuracy_95",
        "title": "Точность 95%",
        "description": "Завершить попытку с точностью не ниже 95%.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 100,
        "goal": 95,
        "icon": "star.svg",
        "sort_order": 90,
    },
    {
        "code": "clean_run",
        "title": "Чистое исполнение",
        "description": "Пройти песню без единого промаха при прогрессе не ниже 50%.",
        "category": CATEGORY_PROGRESS,
        "xp_bonus": 75,
        "goal": None,
        "icon": "accuracy.svg",
        "sort_order": 100,
    },
    {
        "code": "timing_40ms",
        "title": "Чувство ритма",
        "description": "Средняя ошибка по времени не больше 40 мс.",
        "category": CATEGORY_RHYTHM,
        "xp_bonus": 40,
        "goal": 40,
        "icon": "metronome.svg",
        "sort_order": 110,
    },
    {
        "code": "improve_10pp",
        "title": "Становится лучше",
        "description": "Улучшить личную точность на 10 процентных пунктов.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 30,
        "goal": 10,
        "icon": "star.svg",
        "sort_order": 120,
    },
    {
        "code": "improve_5pp",
        "title": "Новый рекорд",
        "description": "Улучшить личную точность на 5 процентных пунктов.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 20,
        "goal": 5,
        "icon": "accuracy.svg",
        "sort_order": 130,
    },
    {
        "code": "genres_3",
        "title": "Три жанра",
        "description": "Личные рекорды в трёх разных жанрах.",
        "category": CATEGORY_GENRES,
        "xp_bonus": 40,
        "goal": 3,
        "icon": "genres.svg",
        "sort_order": 140,
    },
    {
        "code": "genres_5",
        "title": "Пять жанров",
        "description": "Личные рекорды в пяти разных жанрах.",
        "category": CATEGORY_GENRES,
        "xp_bonus": 75,
        "goal": 5,
        "icon": "genres.svg",
        "sort_order": 150,
    },
    {
        "code": "genres_8",
        "title": "Восемь жанров",
        "description": "Личные рекорды в восьми разных жанрах.",
        "category": CATEGORY_GENRES,
        "xp_bonus": 150,
        "goal": 8,
        "icon": "genres.svg",
        "sort_order": 160,
    },
    {
        "code": "genre_master",
        "title": "Покоритель жанра",
        "description": "Достичь ступени Pro (87) в мастерстве любого жанра.",
        "category": CATEGORY_GENRES,
        "xp_bonus": 60,
        "goal": None,
        "icon": "diploma.svg",
        "sort_order": 170,
    },
    {
        "code": "mastery_2",
        "title": "Ученик",
        "description": "Достичь ступени Learner на любой песне.",
        "category": CATEGORY_MASTERY,
        "xp_bonus": 25,
        "goal": 2,
        "icon": "first_notes.svg",
        "sort_order": 180,
    },
    {
        "code": "mastery_3",
        "title": "Гитарист",
        "description": "Достичь ступени Guitarist на любой песне.",
        "category": CATEGORY_MASTERY,
        "xp_bonus": 50,
        "goal": 3,
        "icon": "mastery.svg",
        "sort_order": 190,
    },
    {
        "code": "mastery_4",
        "title": "Профессионал",
        "description": "Достичь ступени Pro на любой песне.",
        "category": CATEGORY_MASTERY,
        "xp_bonus": 100,
        "goal": 4,
        "icon": "medal.svg",
        "sort_order": 200,
    },
    {
        "code": "mastery_5",
        "title": "Мастер",
        "description": "Достичь ступени Master на любой песне.",
        "category": CATEGORY_MASTERY,
        "xp_bonus": 150,
        "goal": 5,
        "icon": "diploma.svg",
        "sort_order": 210,
    },
    {
        "code": "hard_80",
        "title": "Сложная задача",
        "description": "Точность не ниже 80% на песне сложности 4 или выше.",
        "category": CATEGORY_ACCURACY,
        "xp_bonus": 75,
        "goal": 80,
        "icon": "hard.svg",
        "sort_order": 220,
    },
    {
        "code": "daily_first",
        "title": "Я принимаю вызов",
        "description": "Первая официальная попытка в испытании дня.",
        "category": CATEGORY_DAILY,
        "xp_bonus": 30,
        "goal": None,
        "icon": "daily.svg",
        "sort_order": 230,
    },
    {
        "code": "daily_streak_3",
        "title": "Три дня подряд",
        "description": "Официальный результат испытания дня три дня подряд.",
        "category": CATEGORY_DAILY,
        "xp_bonus": 50,
        "goal": 3,
        "icon": "clock.svg",
        "sort_order": 240,
    },
    {
        "code": "daily_streak_7",
        "title": "Неделя вызовов",
        "description": "Официальный результат испытания дня семь дней подряд.",
        "category": CATEGORY_DAILY,
        "xp_bonus": 100,
        "goal": 7,
        "icon": "calendar.svg",
        "sort_order": 250,
    },
    {
        "code": "daily_score_70",
        "title": "Лучший результат дня",
        "description": "Точность официальной попытки дня не ниже 70%.",
        "category": CATEGORY_DAILY,
        "xp_bonus": 50,
        "goal": 70,
        "icon": "target.svg",
        "sort_order": 260,
    },
    {
        "code": "daily_first_place",
        "title": "На вершине дня",
        "description": "Первое место в рейтинге завершившегося дня испытаний.",
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
