"""Campaigns ("Tower riff"): named, ordered song lists for one bot level.

Additive migration on top of ``0025_stage_level_no_character``:

* ``campaigns`` — administrator-authored name (unique, case-insensitive at the
  API level), ``bot_level`` (0 easy, 1 normal, 2 hard) and timestamps. There is
  no row limit: an administrator creates as many campaigns as needed.
* ``campaign_songs`` — the ordered stages. ``position`` is 0-based;
  ``unique(campaign_id, position)`` and ``unique(campaign_id, song_id)`` keep the
  order and the membership unambiguous. Both foreign keys cascade: deleting a
  campaign removes its list, and permanently deleting a song (only possible
  while no player result references it) removes its stage instead of leaving a
  broken reference. Archiving a song keeps the stage; the public endpoint
  filters unpublished songs out.

Downgrade drops the stages first, then the campaigns.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0026_campaigns"
down_revision = "0025_stage_level_no_character"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "campaigns",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column("name", sa.String(120), nullable=False),
        sa.Column("bot_level", sa.SmallInteger(), nullable=False),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.CheckConstraint(
            "bot_level >= 0 AND bot_level <= 2",
            name="ck_campaigns_bot_level",
        ),
    )
    op.create_index("ix_campaigns_name", "campaigns", ["name"], unique=True)

    op.create_table(
        "campaign_songs",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "campaign_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("campaigns.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "song_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("songs.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column("position", sa.Integer(), nullable=False),
        sa.UniqueConstraint(
            "campaign_id", "song_id", name="uq_campaign_songs_campaign_song"
        ),
        sa.UniqueConstraint(
            "campaign_id", "position", name="uq_campaign_songs_campaign_position"
        ),
        sa.CheckConstraint("position >= 0", name="ck_campaign_songs_position"),
    )
    op.create_index(
        "ix_campaign_songs_campaign_id", "campaign_songs", ["campaign_id"]
    )
    op.create_index("ix_campaign_songs_song_id", "campaign_songs", ["song_id"])


def downgrade() -> None:
    op.drop_index("ix_campaign_songs_song_id", table_name="campaign_songs")
    op.drop_index("ix_campaign_songs_campaign_id", table_name="campaign_songs")
    op.drop_table("campaign_songs")
    op.drop_index("ix_campaigns_name", table_name="campaigns")
    op.drop_table("campaigns")
