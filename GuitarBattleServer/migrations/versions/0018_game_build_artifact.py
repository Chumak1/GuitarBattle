"""Two artifacts per platform: ``game_builds.artifact``.

The public download button keeps serving an installer (``.exe`` on Windows,
``.dmg`` on macOS), while the installed desktop client updates itself from a
separate portable ZIP. A build is therefore identified by ``(platform,
artifact)`` and at most one build is active per pair.

``server_default="installer"`` is what makes the migration safe: every row that
already exists was uploaded for the download button, so PostgreSQL backfills
the new ``NOT NULL`` column in place. ``is_active`` is not touched, so the
currently published builds stay published and both the site button and the
desktop client keep their URLs.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0018_game_build_artifact"
down_revision = "0017_game_build_platform"
branch_labels = None
depends_on = None

TABLE = "game_builds"
COLUMN = "artifact"
CONSTRAINT = "ck_game_builds_artifact"
INDEX = "ix_game_builds_artifact"

# Kept in sync with app.services.game_builds.BUILD_ARTIFACTS. Migrations are
# historical records and must not import the application code.
ARTIFACTS = ("installer", "portable")


def upgrade() -> None:
    op.add_column(
        TABLE,
        sa.Column(
            COLUMN,
            sa.String(16),
            nullable=False,
            server_default="installer",
        ),
    )
    op.create_check_constraint(
        CONSTRAINT,
        TABLE,
        "artifact IN (" + ", ".join(f"'{artifact}'" for artifact in ARTIFACTS) + ")",
    )
    op.create_index(INDEX, TABLE, [COLUMN])


def downgrade() -> None:
    op.drop_index(INDEX, table_name=TABLE)
    op.drop_constraint(CONSTRAINT, TABLE, type_="check")
    op.drop_column(TABLE, COLUMN)
