"""Achievement icons: ``achievements.icon`` points at a bundled SVG.

Icons live in ``app/static/media/achievements`` and are served by the API at
``/media/achievements/<icon>``. The column stores only the file name, so the
catalog stays portable (no absolute host or CDN path in the database).

Every one of the 27 catalog rows gets an icon here; the application re-syncs
the same values from ``app.services.achievement_catalog`` on every start, so
this migration is the one-time backfill for existing databases.
"""
from alembic import op
import sqlalchemy as sa

revision = "0010_achievement_icons"
down_revision = "0009_daily_slots"
branch_labels = None
depends_on = None

#: Per-achievement icons (file names under ``app/static/media/achievements``).
ICON_BY_CODE: dict[str, str] = {
    "first_notes": "first_notes.svg",
    "first_personal_best": "star.svg",
    "songs_5": "notes.svg",
    "songs_10": "notes.svg",
    "songs_25": "notes.svg",
    "songs_50": "notes.svg",
    "accuracy_80": "target.svg",
    "accuracy_90": "accuracy.svg",
    "accuracy_95": "star.svg",
    "clean_run": "accuracy.svg",
    "timing_40ms": "metronome.svg",
    "improve_10pp": "star.svg",
    "improve_5pp": "accuracy.svg",
    "genres_3": "genres.svg",
    "genres_5": "genres.svg",
    "genres_8": "genres.svg",
    "genre_master": "diploma.svg",
    "mastery_2": "first_notes.svg",
    "mastery_3": "mastery.svg",
    "mastery_4": "medal.svg",
    "mastery_5": "diploma.svg",
    "hard_80": "hard.svg",
    "daily_first": "daily.svg",
    "daily_streak_3": "clock.svg",
    "daily_streak_7": "calendar.svg",
    "daily_score_70": "target.svg",
    "daily_first_place": "medal.svg",
}

#: Fallback for rows that are not in the catalog above (categories only).
ICON_BY_CATEGORY: dict[str, str] = {
    "progress": "first_notes.svg",
    "songs": "notes.svg",
    "accuracy": "target.svg",
    "rhythm": "metronome.svg",
    "genres": "genres.svg",
    "mastery": "mastery.svg",
    "daily": "daily.svg",
}


def upgrade() -> None:
    op.add_column(
        "achievements",
        sa.Column(
            "icon",
            sa.String(length=128),
            nullable=False,
            server_default="star.svg",
        ),
    )

    achievements = sa.table(
        "achievements",
        sa.column("code", sa.String),
        sa.column("category", sa.String),
        sa.column("icon", sa.String),
    )

    # Category defaults first, then the exact per-code icons. Both statements
    # are idempotent, so a partially seeded catalog still ends up consistent.
    for category, icon in ICON_BY_CATEGORY.items():
        op.execute(
            achievements.update()
            .where(achievements.c.category == category)
            .values(icon=icon)
        )
    for code, icon in ICON_BY_CODE.items():
        op.execute(
            achievements.update()
            .where(achievements.c.code == code).values(icon=icon)
        )

    # The catalog is written explicitly by the application from now on.
    op.alter_column("achievements", "icon", server_default=None)


def downgrade() -> None:
    op.drop_column("achievements", "icon")
