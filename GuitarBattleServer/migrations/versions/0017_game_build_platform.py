"""Per-platform game builds: ``game_builds.platform``.

The Windows installer and the universal macOS build are published independently,
so a build now belongs to one platform and activation only unpublishes the other
builds of the same platform.

``server_default="windows"`` is what makes the migration safe: every row that
already exists is a Windows installer, and PostgreSQL backfills the new
``NOT NULL`` column in place. ``is_active`` is not touched, so the currently
published Windows build stays published and the desktop client keeps its
download URL.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0017_game_build_platform"
down_revision = "0016_game_build_release_notes"
branch_labels = None
depends_on = None

TABLE = "game_builds"
COLUMN = "platform"
CONSTRAINT = "ck_game_builds_platform"
INDEX = "ix_game_builds_platform"

# Kept in sync with app.services.game_builds.GAME_BUILD_PLATFORMS. Migrations
# are historical records and must not import the application code.
PLATFORMS = ("windows", "macos")


def upgrade() -> None:
    op.add_column(
        TABLE,
        sa.Column(
            COLUMN,
            sa.String(16),
            nullable=False,
            server_default="windows",
        ),
    )
    op.create_check_constraint(
        CONSTRAINT,
        TABLE,
        "platform IN (" + ", ".join(f"'{platform}'" for platform in PLATFORMS) + ")",
    )
    op.create_index(INDEX, TABLE, [COLUMN])


def downgrade() -> None:
    op.drop_index(INDEX, table_name=TABLE)
    op.drop_constraint(CONSTRAINT, TABLE, type_="check")
    op.drop_column(TABLE, COLUMN)
