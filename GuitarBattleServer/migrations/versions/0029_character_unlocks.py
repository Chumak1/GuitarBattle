"""Character ownership: starters, per-user unlocks and campaign rewards.

Additive migration on top of ``0028_campaign_song_bot_level``:

* ``ui_characters.is_starter`` — ``BOOLEAN NOT NULL DEFAULT false``. Characters
  flagged here belong to every account from registration; ``Ray`` and ``Luna``
  are marked when they exist (case-insensitive, a missing name is not an error).
* ``user_characters`` — the player's collection: one row per (user, character),
  ``source_campaign_id`` records the tower that granted the reward (``NULL`` for
  starters and for the character a profile already had selected).
* ``user_campaign_progress`` — server-side tower progress (``best_stage``,
  ``completed_at``), keyed by (user, campaign).
* ``campaigns.reward_character_id`` / ``campaigns.reward_condition`` — the
  character a tower unlocks and an optional custom unlock text.

Backfill keeps every existing profile playable: each user gets rows for all
starter characters and for the character they already selected, so nobody loses
a choice when ownership starts being enforced.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0029_character_unlocks"
down_revision = "0028_campaign_song_bot_level"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "ui_characters",
        sa.Column(
            "is_starter",
            sa.Boolean(),
            nullable=False,
            server_default=sa.false(),
        ),
    )
    # Case-insensitive: an administrator may have typed "ray" or "LUNA".
    op.execute(
        """
        UPDATE ui_characters
        SET is_starter = true
        WHERE lower(name) IN ('ray', 'luna')
        """
    )

    op.add_column(
        "campaigns",
        sa.Column(
            "reward_character_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("ui_characters.id", ondelete="SET NULL"),
            nullable=True,
        ),
    )
    op.add_column(
        "campaigns",
        sa.Column("reward_condition", sa.String(200), nullable=True),
    )
    op.create_index(
        "ix_campaigns_reward_character_id",
        "campaigns",
        ["reward_character_id"],
    )

    op.create_table(
        "user_characters",
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "character_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("ui_characters.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "unlocked_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column(
            "source_campaign_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("campaigns.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.PrimaryKeyConstraint("user_id", "character_id"),
    )
    op.create_index(
        "ix_user_characters_character_id", "user_characters", ["character_id"]
    )

    op.create_table(
        "user_campaign_progress",
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "campaign_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("campaigns.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "best_stage",
            sa.Integer(),
            nullable=False,
            server_default="0",
        ),
        sa.Column("completed_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.CheckConstraint(
            "best_stage >= 0",
            name="ck_user_campaign_progress_best_stage",
        ),
        sa.PrimaryKeyConstraint("user_id", "campaign_id"),
    )
    op.create_index(
        "ix_user_campaign_progress_campaign_id",
        "user_campaign_progress",
        ["campaign_id"],
    )

    # Every existing user owns the starter characters...
    op.execute(
        """
        INSERT INTO user_characters (user_id, character_id)
        SELECT u.id, c.id
        FROM users AS u
        CROSS JOIN ui_characters AS c
        WHERE c.is_starter = true
        ON CONFLICT (user_id, character_id) DO NOTHING
        """
    )
    # ...and keeps the character selected before ownership was enforced.
    op.execute(
        """
        INSERT INTO user_characters (user_id, character_id)
        SELECT u.id, u.character_id
        FROM users AS u
        WHERE u.character_id IS NOT NULL
        ON CONFLICT (user_id, character_id) DO NOTHING
        """
    )


def downgrade() -> None:
    op.drop_index(
        "ix_user_campaign_progress_campaign_id",
        table_name="user_campaign_progress",
    )
    op.drop_table("user_campaign_progress")
    op.drop_index("ix_user_characters_character_id", table_name="user_characters")
    op.drop_table("user_characters")
    op.drop_index("ix_campaigns_reward_character_id", table_name="campaigns")
    op.drop_column("campaigns", "reward_condition")
    op.drop_column("campaigns", "reward_character_id")
    op.drop_column("ui_characters", "is_starter")
