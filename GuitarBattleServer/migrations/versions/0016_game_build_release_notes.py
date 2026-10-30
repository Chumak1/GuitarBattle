"""Editable "what's new" text for an uploaded game build.

The column is additive and ``server_default=""`` is required: without it the
``NOT NULL`` constraint would fail on the rows already present in
``game_builds``. Existing builds therefore start with an empty description and
can be edited from the admin console without re-uploading the installer.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0016_game_build_release_notes"
down_revision = "0015_support_page"
branch_labels = None
depends_on = None

TABLE = "game_builds"
COLUMN = "release_notes"


def upgrade() -> None:
    op.add_column(
        TABLE,
        sa.Column(COLUMN, sa.Text(), nullable=False, server_default=""),
    )


def downgrade() -> None:
    op.drop_column(TABLE, COLUMN)
