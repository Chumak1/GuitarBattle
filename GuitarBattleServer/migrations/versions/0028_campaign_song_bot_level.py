"""Campaigns: per-stage bot difficulty.

Additive migration on top of ``0027_duel_lobby_start_confirm``:

* ``campaign_songs.bot_level`` — ``SMALLINT NULL`` with a ``0..2`` CHECK. A
  ``NULL`` value means "use ``campaigns.bot_level``", so the campaign column
  stays the default for newly added stages and the fallback for old rows.

Every existing row is backfilled with its campaign's current ``bot_level`` in
one ``UPDATE ... FROM`` statement. A tower therefore keeps exactly the
difficulty it had before the migration, while administrators can now assign a
different level to individual stages.

Downgrade drops the column, returning every stage to the campaign-wide level.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0028_campaign_song_bot_level"
down_revision = "0027_duel_lobby_start_confirm"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "campaign_songs",
        sa.Column("bot_level", sa.SmallInteger(), nullable=True),
    )
    op.create_check_constraint(
        "ck_campaign_songs_bot_level",
        "campaign_songs",
        "bot_level >= 0 AND bot_level <= 2",
    )
    # Backfill: every stage inherits the campaign-wide difficulty it was
    # played with before this revision existed.
    op.execute(
        """
        UPDATE campaign_songs AS cs
        SET bot_level = c.bot_level
        FROM campaigns AS c
        WHERE cs.campaign_id = c.id
        """
    )


def downgrade() -> None:
    op.drop_constraint(
        "ck_campaign_songs_bot_level",
        "campaign_songs",
        type_="check",
    )
    op.drop_column("campaign_songs", "bot_level")
