"""Scoring normalization and anti-farm rewards (Battle + Online).

Additive migration on top of ``0029_character_unlocks``:

* ``scores.normalized_score`` — actual score extrapolated to a full song for
  ranked solo rows (``NULL`` for early ends, unranked attempts and old rows).
* ``duel_participants.score_points`` / ``normalized_score`` / ``xp_breakdown``
  (JSONB) / ``xp_applied_at`` — the online-duel actual score, its extrapolation,
  the XP audit snapshot and the once-only marker.
* ``duel_song_bests`` — per-player online PB on one song (anti-farm bonus).
* ``campaign_stage_runs`` — every reported Battle stage, unique per
  ``(user_id, run_id)`` so a retried report cannot be rewarded twice.
* ``user_campaign_progress.completion_count`` — number of **full** tower
  completions; backfilled to ``1`` for towers already completed, so a replayed
  tower starts on the 50% repeat slot instead of a fresh 100%.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0030_scoring_normalization"
down_revision = "0029_character_unlocks"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "scores",
        sa.Column("normalized_score", sa.Integer(), nullable=True),
    )

    op.add_column(
        "duel_participants",
        sa.Column("score_points", sa.Integer(), nullable=True),
    )
    op.add_column(
        "duel_participants",
        sa.Column("normalized_score", sa.Integer(), nullable=True),
    )
    op.add_column(
        "duel_participants",
        sa.Column("xp_breakdown", postgresql.JSONB(), nullable=True),
    )
    op.add_column(
        "duel_participants",
        sa.Column(
            "xp_applied_at", sa.DateTime(timezone=True), nullable=True
        ),
    )

    op.create_table(
        "duel_song_bests",
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "song_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("songs.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "best_accuracy",
            sa.Float(),
            nullable=False,
            server_default="0",
        ),
        sa.Column(
            "best_score",
            sa.Integer(),
            nullable=False,
            server_default="0",
        ),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.PrimaryKeyConstraint("user_id", "song_id"),
    )

    op.create_table(
        "campaign_stage_runs",
        sa.Column(
            "id",
            postgresql.UUID(as_uuid=True),
            primary_key=True,
        ),
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
        sa.Column("position", sa.Integer(), nullable=False),
        sa.Column("run_id", postgresql.UUID(as_uuid=True), nullable=False),
        sa.Column(
            "score_points", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column("normalized_score", sa.Integer(), nullable=True),
        sa.Column(
            "accuracy_percent", sa.Float(), nullable=False, server_default="0"
        ),
        sa.Column(
            "perfect_count", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "great_count", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "good_count", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "miss_count", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "completion_percent", sa.Float(), nullable=False, server_default="0"
        ),
        sa.Column("notes_total", sa.Integer(), nullable=True),
        sa.Column("notes_played", sa.Integer(), nullable=True),
        sa.Column(
            "won", sa.Boolean(), nullable=False, server_default=sa.false()
        ),
        sa.Column(
            "xp_awarded", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "repeat_factor", sa.Float(), nullable=False, server_default="1"
        ),
        sa.Column("pb_bonus", sa.Integer(), nullable=False, server_default="0"),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.UniqueConstraint(
            "user_id", "run_id", name="uq_campaign_stage_runs_user_run"
        ),
        sa.CheckConstraint(
            "position >= 0", name="ck_campaign_stage_runs_position"
        ),
        sa.CheckConstraint(
            "score_points >= 0", name="ck_campaign_stage_runs_score"
        ),
    )
    op.create_index(
        "ix_campaign_stage_runs_user_campaign_position",
        "campaign_stage_runs",
        ["user_id", "campaign_id", "position"],
    )

    op.add_column(
        "user_campaign_progress",
        sa.Column(
            "completion_count",
            sa.Integer(),
            nullable=False,
            server_default="0",
        ),
    )
    # A tower completed before this migration already used its first full run:
    # replaying it must use the 50% slot, not another 100% one.
    op.execute(
        """
        UPDATE user_campaign_progress
        SET completion_count = 1
        WHERE completed_at IS NOT NULL AND completion_count = 0
        """
    )


def downgrade() -> None:
    op.drop_column("user_campaign_progress", "completion_count")

    op.drop_index(
        "ix_campaign_stage_runs_user_campaign_position",
        table_name="campaign_stage_runs",
    )
    op.drop_table("campaign_stage_runs")

    op.drop_table("duel_song_bests")

    op.drop_column("duel_participants", "xp_applied_at")
    op.drop_column("duel_participants", "xp_breakdown")
    op.drop_column("duel_participants", "normalized_score")
    op.drop_column("duel_participants", "score_points")

    op.drop_column("scores", "normalized_score")
