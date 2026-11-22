"""Internal statistics opt-out per account.

Additive on top of ``0036_artwork_avatars``; nothing existing is rewritten.

* ``users.statistics_excluded`` — ``FALSE`` for every existing account, so the
  current numbers do not change. An administrator sets it to ``TRUE`` for
  friends, admin and test accounts, and every aggregate of the statistics tab
  (overview, per-game launches, active-user counters) skips them. The account,
  its telemetry and its place in the public leaderboards are untouched.

The downgrade drops the column only.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0037_statistics_excluded"
down_revision = "0036_artwork_avatars"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "users",
        sa.Column(
            "statistics_excluded",
            sa.Boolean(),
            nullable=False,
            server_default=sa.false(),
        ),
    )


def downgrade() -> None:
    op.drop_column("users", "statistics_excluded")
