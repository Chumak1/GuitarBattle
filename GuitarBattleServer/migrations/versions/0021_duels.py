"""Guitar Duels: matches, participants, events and ratings.

Additive migration: it only creates the three duel tables and their indexes.
No existing table or row is touched. The rules themselves live in
``app/services/duel_rules.py``; the tables store the frozen rules version, the
bot seed, a chart snapshot (note offsets) and the authoritative result.

Downgrade drops the three tables in reverse dependency order and is safe on an
empty and on a populated database: everything it removes belongs to duels.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0021_duels"
down_revision = "0020_song_time_signature"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "duel_matches",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "song_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("songs.id", ondelete="RESTRICT"),
            nullable=False,
        ),
        sa.Column("mode", sa.String(16), nullable=False),
        sa.Column("bot_level", sa.String(16), nullable=True),
        sa.Column(
            "status", sa.String(16), nullable=False, server_default="waiting"
        ),
        sa.Column("rules_version", sa.Integer(), nullable=False, server_default="2"),
        sa.Column("seed", sa.BigInteger(), nullable=False),
        sa.Column("note_count", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("note_times_ms", postgresql.JSONB(), nullable=True),
        sa.Column(
            "next_note_index", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column("started_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("finished_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column(
            "winner_user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="SET NULL"),
            nullable=True,
        ),
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
        sa.CheckConstraint("mode IN ('bot', 'online')", name="ck_duel_matches_mode"),
        sa.CheckConstraint(
            "status IN ('waiting', 'playing', 'finished', 'cancelled')",
            name="ck_duel_matches_status",
        ),
        sa.CheckConstraint("note_count >= 0", name="ck_duel_matches_note_count"),
        sa.CheckConstraint(
            "next_note_index >= 0", name="ck_duel_matches_next_note_index"
        ),
    )
    op.create_index(
        "ix_duel_matches_status_mode_created",
        "duel_matches",
        ["status", "mode", "created_at"],
    )
    op.create_index(
        "ix_duel_matches_finished_at", "duel_matches", ["finished_at"]
    )
    op.create_index("ix_duel_matches_song_id", "duel_matches", ["song_id"])

    op.create_table(
        "duel_participants",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "match_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("duel_matches.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.Column("side", sa.String(8), nullable=False),
        sa.Column(
            "is_bot", sa.Boolean(), nullable=False, server_default=sa.false()
        ),
        sa.Column("bot_name", sa.String(32), nullable=True),
        sa.Column("bot_avatar", sa.String(32), nullable=True),
        sa.Column("ready_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column(
            "finish_requested_at", sa.DateTime(timezone=True), nullable=True
        ),
        sa.Column("left_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column(
            "joined_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column("last_seen_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("hp_final", sa.Float(), nullable=False, server_default="100"),
        sa.Column(
            "perfect_count", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column("great_count", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("good_count", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("miss_count", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("notes_played", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("damage_dealt", sa.Float(), nullable=False, server_default="0"),
        sa.Column("damage_taken", sa.Float(), nullable=False, server_default="0"),
        sa.Column("xp_awarded", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("rating_before", sa.Integer(), nullable=True),
        sa.Column("rating_after", sa.Integer(), nullable=True),
        sa.Column("result", sa.String(8), nullable=True),
        sa.UniqueConstraint(
            "match_id", "side", name="uq_duel_participants_match_side"
        ),
        sa.CheckConstraint(
            "side IN ('player', 'opponent')", name="ck_duel_participants_side"
        ),
        sa.CheckConstraint(
            "result IS NULL OR result IN ('win', 'loss', 'draw')",
            name="ck_duel_participants_result",
        ),
        sa.CheckConstraint(
            "hp_final >= 0 AND hp_final <= 100", name="ck_duel_participants_hp"
        ),
    )
    op.create_index(
        "ix_duel_participants_user_id", "duel_participants", ["user_id"]
    )

    op.create_table(
        "duel_events",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "match_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("duel_matches.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column("note_index", sa.Integer(), nullable=False),
        sa.Column("quality", sa.String(8), nullable=False),
        sa.Column("client_time_ms", sa.BigInteger(), nullable=True),
        sa.Column("server_time_ms", sa.BigInteger(), nullable=False),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.UniqueConstraint(
            "match_id",
            "user_id",
            "note_index",
            name="uq_duel_events_match_user_note",
        ),
        sa.CheckConstraint("note_index >= 0", name="ck_duel_events_note_index"),
        sa.CheckConstraint(
            "quality IN ('perfect', 'great', 'good', 'miss')",
            name="ck_duel_events_quality",
        ),
    )
    op.create_index("ix_duel_events_user_id", "duel_events", ["user_id"])


def downgrade() -> None:
    op.drop_index("ix_duel_events_user_id", table_name="duel_events")
    op.drop_table("duel_events")
    op.drop_index(
        "ix_duel_participants_user_id", table_name="duel_participants"
    )
    op.drop_table("duel_participants")
    op.drop_index("ix_duel_matches_song_id", table_name="duel_matches")
    op.drop_index("ix_duel_matches_finished_at", table_name="duel_matches")
    op.drop_index(
        "ix_duel_matches_status_mode_created", table_name="duel_matches"
    )
    op.drop_table("duel_matches")
