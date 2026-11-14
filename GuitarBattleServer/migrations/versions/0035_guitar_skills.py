"""Guitar Skills rating: config, event log, snapshots and history.

Additive migration on top of ``0034_campaign_cover``. Nothing existing is
rewritten; the only new column on an existing table is
``duel_participants.guitar_skills_before`` (both players' pre-match Guitar
Skills, frozen when a match finishes).

The single configuration row is seeded with the documented defaults. The
``median_note_count`` is computed once here from the published, non-daily,
non-archived songs that have an analysed active MIDI file. If the catalogue is
empty the column stays ``NULL`` and the engine treats the note factor as
neutral (1.0) instead of inventing a note count.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0035_guitar_skills"
down_revision = "0034_campaign_cover"
branch_labels = None
depends_on = None

#: ``progression._level_start(50)``; kept literal so a later progression change
#: cannot silently move the rating reference point of already stored configs.
XP_REFERENCE_LEVEL_50 = 344_756


def upgrade() -> None:
    op.add_column(
        "duel_participants",
        sa.Column("guitar_skills_before", sa.Float(), nullable=True),
    )

    op.create_table(
        "guitar_skills_config",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=False),
        sa.Column(
            "accuracy_weight", sa.Float(), nullable=False, server_default="0.35"
        ),
        sa.Column(
            "challenge_weight", sa.Float(), nullable=False, server_default="0.20"
        ),
        sa.Column(
            "score_weight", sa.Float(), nullable=False, server_default="0.10"
        ),
        sa.Column(
            "duel_weight", sa.Float(), nullable=False, server_default="0.25"
        ),
        sa.Column(
            "progression_weight",
            sa.Float(),
            nullable=False,
            server_default="0.10",
        ),
        sa.Column("window_size", sa.Integer(), nullable=False, server_default="30"),
        sa.Column(
            "recency_decay", sa.Float(), nullable=False, server_default="10"
        ),
        sa.Column(
            "difficulty_easy_max",
            sa.Integer(),
            nullable=False,
            server_default="3",
        ),
        sa.Column(
            "difficulty_normal_max",
            sa.Integer(),
            nullable=False,
            server_default="7",
        ),
        sa.Column(
            "difficulty_easy_factor",
            sa.Float(),
            nullable=False,
            server_default="0.60",
        ),
        sa.Column(
            "difficulty_normal_factor",
            sa.Float(),
            nullable=False,
            server_default="0.80",
        ),
        sa.Column(
            "difficulty_hard_factor",
            sa.Float(),
            nullable=False,
            server_default="1.00",
        ),
        sa.Column(
            "note_factor_min", sa.Float(), nullable=False, server_default="0.75"
        ),
        sa.Column(
            "note_factor_max", sa.Float(), nullable=False, server_default="1.25"
        ),
        sa.Column("median_note_count", sa.Integer(), nullable=True),
        sa.Column(
            "level_normalization_max",
            sa.Integer(),
            nullable=False,
            server_default="50",
        ),
        sa.Column(
            "xp_reference",
            sa.Integer(),
            nullable=False,
            server_default=str(XP_REFERENCE_LEVEL_50),
        ),
        sa.Column(
            "duel_strength_guitar_weight",
            sa.Float(),
            nullable=False,
            server_default="0.70",
        ),
        sa.Column(
            "duel_strength_level_weight",
            sa.Float(),
            nullable=False,
            server_default="0.30",
        ),
        sa.Column(
            "expected_score_scale",
            sa.Float(),
            nullable=False,
            server_default="25",
        ),
        sa.Column(
            "reliability_constant",
            sa.Float(),
            nullable=False,
            server_default="10",
        ),
        sa.Column(
            "per_song_attempt_cap",
            sa.Float(),
            nullable=False,
            server_default="2",
        ),
        sa.Column(
            "confidence_expected_events",
            sa.Integer(),
            nullable=False,
            server_default="10",
        ),
        sa.Column(
            "confidence_expected_duels",
            sa.Integer(),
            nullable=False,
            server_default="10",
        ),
        sa.Column(
            "provisional_threshold",
            sa.Float(),
            nullable=False,
            server_default="0.35",
        ),
        sa.Column(
            "min_ranked_events",
            sa.Integer(),
            nullable=False,
            server_default="5",
        ),
        sa.Column(
            "calculation_version", sa.Integer(), nullable=False, server_default="1"
        ),
        sa.Column(
            "updated_by",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.CheckConstraint(
            "accuracy_weight >= 0 AND accuracy_weight <= 1",
            name="ck_guitar_skills_config_accuracy_weight",
        ),
        sa.CheckConstraint(
            "challenge_weight >= 0 AND challenge_weight <= 1",
            name="ck_guitar_skills_config_challenge_weight",
        ),
        sa.CheckConstraint(
            "score_weight >= 0 AND score_weight <= 1",
            name="ck_guitar_skills_config_score_weight",
        ),
        sa.CheckConstraint(
            "duel_weight >= 0 AND duel_weight <= 1",
            name="ck_guitar_skills_config_duel_weight",
        ),
        sa.CheckConstraint(
            "progression_weight >= 0 AND progression_weight <= 1",
            name="ck_guitar_skills_config_progression_weight",
        ),
        sa.CheckConstraint(
            "window_size >= 1 AND window_size <= 200",
            name="ck_guitar_skills_config_window_size",
        ),
        sa.CheckConstraint(
            "recency_decay > 0", name="ck_guitar_skills_config_recency_decay"
        ),
        sa.CheckConstraint(
            "difficulty_easy_max >= 1 AND difficulty_easy_max <= 10",
            name="ck_guitar_skills_config_easy_max",
        ),
        sa.CheckConstraint(
            "difficulty_normal_max >= difficulty_easy_max "
            "AND difficulty_normal_max <= 10",
            name="ck_guitar_skills_config_normal_max",
        ),
        sa.CheckConstraint(
            "note_factor_min >= 0 AND note_factor_max >= note_factor_min",
            name="ck_guitar_skills_config_note_factor",
        ),
        sa.CheckConstraint(
            "level_normalization_max >= 1",
            name="ck_guitar_skills_config_level_normalization",
        ),
        sa.CheckConstraint(
            "xp_reference >= 1", name="ck_guitar_skills_config_xp_reference"
        ),
        sa.CheckConstraint(
            "provisional_threshold >= 0 AND provisional_threshold <= 1",
            name="ck_guitar_skills_config_provisional_threshold",
        ),
        sa.CheckConstraint(
            "min_ranked_events >= 0",
            name="ck_guitar_skills_config_min_ranked_events",
        ),
    )

    # Seed the one active row. The median is derived from real chart lengths,
    # never from a constant.
    op.execute(
        sa.text(
            """
        INSERT INTO guitar_skills_config (
            id, accuracy_weight, challenge_weight, score_weight, duel_weight,
            progression_weight, window_size, recency_decay,
            difficulty_easy_max, difficulty_normal_max,
            difficulty_easy_factor, difficulty_normal_factor,
            difficulty_hard_factor, note_factor_min, note_factor_max,
            median_note_count, level_normalization_max, xp_reference,
            duel_strength_guitar_weight, duel_strength_level_weight,
            expected_score_scale, reliability_constant, per_song_attempt_cap,
            confidence_expected_events, confidence_expected_duels,
            provisional_threshold, min_ranked_events, calculation_version
        )
        SELECT
            1, 0.35, 0.20, 0.10, 0.25, 0.10, 30, 10,
            3, 7, 0.60, 0.80, 1.00, 0.75, 1.25,
            (
                WITH per_song AS (
                    SELECT
                        s.id AS song_id,
                        max(a.note_count) AS note_count
                    FROM songs AS s
                    JOIN song_files AS f
                      ON f.song_id = s.id
                     AND f.role = 'midi'
                     AND f.is_active = true
                    JOIN song_midi_analyses AS a
                      ON a.song_file_id = f.id
                    WHERE s.is_published = true
                      AND s.daily_weekday IS NULL
                      AND s.archived_at IS NULL
                      AND a.note_count > 0
                    GROUP BY s.id
                )
                SELECT round(
                    percentile_cont(0.5) WITHIN GROUP (ORDER BY note_count)
                )::integer
                FROM per_song
            ),
            50, :xp_reference, 0.70, 0.30, 25, 10, 2, 10, 10, 0.35, 5, 1
        """
        ).bindparams(xp_reference=XP_REFERENCE_LEVEL_50)
    )

    op.create_table(
        "guitar_skills_config_history",
        sa.Column(
            "id", sa.BigInteger(), primary_key=True, autoincrement=True
        ),
        sa.Column("config", postgresql.JSONB(), nullable=False),
        sa.Column(
            "admin_user_id",
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
    )
    op.create_index(
        "ix_guitar_skills_config_history_created_at",
        "guitar_skills_config_history",
        ["created_at"],
    )

    op.create_table(
        "user_guitar_skills",
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            primary_key=True,
        ),
        sa.Column(
            "guitar_skills",
            sa.Numeric(5, 1),
            nullable=False,
            server_default="50",
        ),
        sa.Column(
            "accuracy_score",
            sa.Numeric(5, 1),
            nullable=False,
            server_default="50",
        ),
        sa.Column(
            "challenge_score",
            sa.Numeric(5, 1),
            nullable=False,
            server_default="50",
        ),
        sa.Column(
            "score_performance",
            sa.Numeric(5, 1),
            nullable=False,
            server_default="50",
        ),
        sa.Column(
            "duel_performance",
            sa.Numeric(5, 1),
            nullable=False,
            server_default="50",
        ),
        sa.Column(
            "progression_score",
            sa.Numeric(5, 1),
            nullable=False,
            server_default="50",
        ),
        sa.Column("confidence", sa.Float(), nullable=False, server_default="0"),
        sa.Column(
            "provisional", sa.Boolean(), nullable=False, server_default="true"
        ),
        sa.Column(
            "events_counted", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "duel_matches", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "accuracy_percent", sa.Float(), nullable=False, server_default="0"
        ),
        sa.Column("duel_wins", sa.Integer(), nullable=False, server_default="0"),
        sa.Column(
            "duel_losses", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column("duel_draws", sa.Integer(), nullable=False, server_default="0"),
        sa.Column(
            "duel_win_rate", sa.Float(), nullable=False, server_default="0"
        ),
        sa.Column("rank", sa.Integer(), nullable=True),
        sa.Column("previous_rank", sa.Integer(), nullable=True),
        sa.Column(
            "accuracy_events", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "challenge_events", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "score_events", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "duel_events", sa.Integer(), nullable=False, server_default="0"
        ),
        sa.Column(
            "progression_events",
            sa.Integer(),
            nullable=False,
            server_default="0",
        ),
        sa.Column(
            "accuracy_confidence", sa.Float(), nullable=False, server_default="0"
        ),
        sa.Column(
            "challenge_confidence",
            sa.Float(),
            nullable=False,
            server_default="0",
        ),
        sa.Column(
            "score_confidence", sa.Float(), nullable=False, server_default="0"
        ),
        sa.Column(
            "duel_confidence", sa.Float(), nullable=False, server_default="0"
        ),
        sa.Column(
            "progression_confidence",
            sa.Float(),
            nullable=False,
            server_default="0",
        ),
        sa.Column(
            "calculation_version", sa.Integer(), nullable=False, server_default="1"
        ),
        sa.Column(
            "last_calculated_at", sa.DateTime(timezone=True), nullable=True
        ),
        sa.CheckConstraint(
            "guitar_skills >= 0 AND guitar_skills <= 100",
            name="ck_user_guitar_skills_total",
        ),
        sa.CheckConstraint(
            "accuracy_score >= 0 AND accuracy_score <= 100",
            name="ck_user_guitar_skills_accuracy",
        ),
        sa.CheckConstraint(
            "challenge_score >= 0 AND challenge_score <= 100",
            name="ck_user_guitar_skills_challenge",
        ),
        sa.CheckConstraint(
            "score_performance >= 0 AND score_performance <= 100",
            name="ck_user_guitar_skills_score",
        ),
        sa.CheckConstraint(
            "duel_performance >= 0 AND duel_performance <= 100",
            name="ck_user_guitar_skills_duel",
        ),
        sa.CheckConstraint(
            "progression_score >= 0 AND progression_score <= 100",
            name="ck_user_guitar_skills_progression",
        ),
        sa.CheckConstraint(
            "confidence >= 0 AND confidence <= 1",
            name="ck_user_guitar_skills_confidence",
        ),
        sa.CheckConstraint(
            "rank IS NULL OR rank >= 1", name="ck_user_guitar_skills_rank"
        ),
    )
    op.create_index(
        "ix_user_guitar_skills_leaderboard",
        "user_guitar_skills",
        ["guitar_skills", "duel_performance"],
    )
    op.create_index(
        "ix_user_guitar_skills_rank", "user_guitar_skills", ["rank"]
    )
    op.create_index(
        "ix_user_guitar_skills_provisional",
        "user_guitar_skills",
        ["provisional"],
    )

    op.create_table(
        "guitar_skills_events",
        sa.Column(
            "id", sa.BigInteger(), primary_key=True, autoincrement=True
        ),
        sa.Column("event_key", sa.String(length=80), nullable=False, unique=True),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "match_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("duel_matches.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.Column(
            "song_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("songs.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.Column("mode", sa.String(length=16), nullable=False),
        sa.Column("accuracy", sa.Float(), nullable=False),
        sa.Column("score", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("note_count", sa.Integer(), nullable=True),
        sa.Column("difficulty", sa.Integer(), nullable=True),
        sa.Column(
            "opponent_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.Column("opponent_level", sa.Integer(), nullable=True),
        sa.Column("opponent_pre_match_rating", sa.Integer(), nullable=True),
        sa.Column(
            "opponent_pre_match_guitar_skills", sa.Float(), nullable=True
        ),
        sa.Column("player_pre_match_rating", sa.Integer(), nullable=True),
        sa.Column(
            "player_pre_match_guitar_skills", sa.Float(), nullable=True
        ),
        sa.Column("result", sa.String(length=8), nullable=True),
        sa.Column("score_margin", sa.Float(), nullable=True),
        sa.Column(
            "occurred_at", sa.DateTime(timezone=True), nullable=False
        ),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.CheckConstraint(
            "accuracy >= 0 AND accuracy <= 100",
            name="ck_guitar_skills_events_accuracy",
        ),
        sa.CheckConstraint("score >= 0", name="ck_guitar_skills_events_score"),
        sa.CheckConstraint(
            "note_count IS NULL OR note_count > 0",
            name="ck_guitar_skills_events_note_count",
        ),
        sa.CheckConstraint(
            "difficulty IS NULL OR (difficulty >= 1 AND difficulty <= 10)",
            name="ck_guitar_skills_events_difficulty",
        ),
        sa.CheckConstraint(
            "result IS NULL OR result IN ('win', 'loss', 'draw')",
            name="ck_guitar_skills_events_result",
        ),
        sa.CheckConstraint(
            "score_margin IS NULL OR (score_margin >= 0 AND score_margin <= 1)",
            name="ck_guitar_skills_events_margin",
        ),
    )
    op.create_index(
        "ix_guitar_skills_events_user_occurred",
        "guitar_skills_events",
        ["user_id", "occurred_at"],
    )
    op.create_index(
        "ix_guitar_skills_events_song_user",
        "guitar_skills_events",
        ["song_id", "user_id"],
    )
    op.create_index(
        "ix_guitar_skills_events_match",
        "guitar_skills_events",
        ["match_id"],
    )

    op.create_table(
        "guitar_skills_history",
        sa.Column(
            "id", sa.BigInteger(), primary_key=True, autoincrement=True
        ),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column("old_guitar_skills", sa.Numeric(5, 1), nullable=True),
        sa.Column("new_guitar_skills", sa.Numeric(5, 1), nullable=False),
        sa.Column("old_rank", sa.Integer(), nullable=True),
        sa.Column("new_rank", sa.Integer(), nullable=True),
        sa.Column("component_values", postgresql.JSONB(), nullable=False),
        sa.Column("calculation_version", sa.Integer(), nullable=False),
        sa.Column("reason", sa.String(length=64), nullable=False),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
    )
    op.create_index(
        "ix_guitar_skills_history_user_created",
        "guitar_skills_history",
        ["user_id", "created_at"],
    )


def downgrade() -> None:
    op.drop_index(
        "ix_guitar_skills_history_user_created",
        table_name="guitar_skills_history",
    )
    op.drop_table("guitar_skills_history")

    op.drop_index(
        "ix_guitar_skills_events_match", table_name="guitar_skills_events"
    )
    op.drop_index(
        "ix_guitar_skills_events_song_user", table_name="guitar_skills_events"
    )
    op.drop_index(
        "ix_guitar_skills_events_user_occurred",
        table_name="guitar_skills_events",
    )
    op.drop_table("guitar_skills_events")

    op.drop_index(
        "ix_user_guitar_skills_provisional", table_name="user_guitar_skills"
    )
    op.drop_index("ix_user_guitar_skills_rank", table_name="user_guitar_skills")
    op.drop_index(
        "ix_user_guitar_skills_leaderboard", table_name="user_guitar_skills"
    )
    op.drop_table("user_guitar_skills")

    op.drop_index(
        "ix_guitar_skills_config_history_created_at",
        table_name="guitar_skills_config_history",
    )
    op.drop_table("guitar_skills_config_history")

    op.drop_table("guitar_skills_config")

    op.drop_column("duel_participants", "guitar_skills_before")
