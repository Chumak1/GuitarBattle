"""Player progression: XP, levels, personal bests, achievements, daily challenges."""
from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

from app.services.achievement_catalog import ACHIEVEMENT_CATALOG

revision = "0008_progression"
down_revision = "0007_app_statistics"
branch_labels = None
depends_on = None


def upgrade() -> None:
    # --- users: progression counters -------------------------------------
    # Existing accounts start at level 1 with 0 XP; ``server_default`` backfills
    # every current row, so no separate UPDATE is needed.
    op.add_column(
        "users",
        sa.Column("xp_total", sa.BigInteger(), nullable=False, server_default="0"),
    )
    op.add_column(
        "users",
        sa.Column("level", sa.Integer(), nullable=False, server_default="1"),
    )

    # --- scores: progression inputs ---------------------------------------
    # ``completion_percent`` defaults to 100 so historical rows count as full
    # attempts and keep the old statistics unchanged.
    op.add_column(
        "scores",
        sa.Column(
            "completion_percent", sa.Float(), nullable=False, server_default="100"
        ),
    )
    op.add_column("scores", sa.Column("avg_timing_error_ms", sa.Float(), nullable=True))
    op.add_column("scores", sa.Column("notes_total", sa.Integer(), nullable=True))
    op.add_column("scores", sa.Column("notes_played", sa.Integer(), nullable=True))

    # --- game_sessions: mode ---------------------------------------------
    op.add_column(
        "game_sessions",
        sa.Column("mode", sa.String(16), nullable=False, server_default="normal"),
    )
    op.create_index(
        "ix_game_sessions_user_song_mode",
        "game_sessions",
        ["user_id", "song_id", "mode"],
    )

    # --- per-song personal best ------------------------------------------
    op.create_table(
        "song_best_scores",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
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
        sa.Column("best_score", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("best_accuracy", sa.Float(), nullable=False, server_default="0"),
        sa.Column("best_max_combo", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("best_avg_timing_error_ms", sa.Float(), nullable=True),
        sa.Column("attempts", sa.Integer(), nullable=False, server_default="0"),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.UniqueConstraint(
            "user_id", "song_id", name="uq_song_best_scores_user_song"
        ),
    )
    op.create_index("ix_song_best_scores_user_id", "song_best_scores", ["user_id"])
    op.create_index("ix_song_best_scores_song_id", "song_best_scores", ["song_id"])

    # --- XP ledger: unique session_id makes the award idempotent ----------
    op.create_table(
        "xp_awards",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "session_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("game_sessions.id", ondelete="CASCADE"),
            nullable=False,
            unique=True,
        ),
        sa.Column("amount", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("breakdown", postgresql.JSONB(), nullable=True),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
    )
    op.create_index("ix_xp_awards_user_id", "xp_awards", ["user_id"])

    # --- achievement catalog and player state ----------------------------
    op.create_table(
        "achievements",
        sa.Column("code", sa.String(64), primary_key=True),
        sa.Column("title", sa.String(200), nullable=False),
        sa.Column("description", sa.String(500), nullable=False),
        sa.Column("category", sa.String(32), nullable=False),
        sa.Column("xp_bonus", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("goal", sa.Integer(), nullable=True),
        sa.Column(
            "is_active", sa.Boolean(), nullable=False, server_default=sa.true()
        ),
        sa.Column("sort_order", sa.Integer(), nullable=False, server_default="0"),
    )
    op.create_table(
        "user_achievements",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "achievement_code",
            sa.String(64),
            sa.ForeignKey("achievements.code", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "awarded_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column("progress", sa.Integer(), nullable=False, server_default="0"),
        sa.UniqueConstraint(
            "user_id", "achievement_code", name="uq_user_achievements_user_code"
        ),
    )
    op.create_index(
        "ix_user_achievements_user_id", "user_achievements", ["user_id"]
    )

    # --- daily challenge --------------------------------------------------
    op.create_table(
        "daily_challenges",
        sa.Column("challenge_date", sa.Date(), primary_key=True),
        sa.Column(
            "song_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("songs.id", ondelete="RESTRICT"),
            nullable=False,
        ),
        sa.Column("rules_version", sa.Integer(), nullable=False, server_default="1"),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
    )
    op.create_table(
        "daily_results",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "challenge_date",
            sa.Date(),
            sa.ForeignKey("daily_challenges.challenge_date", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "session_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("game_sessions.id", ondelete="CASCADE"),
            nullable=False,
            unique=True,
        ),
        sa.Column("score_points", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("accuracy_percent", sa.Float(), nullable=False, server_default="0"),
        sa.Column("avg_timing_error_ms", sa.Float(), nullable=True),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.UniqueConstraint(
            "challenge_date", "user_id", name="uq_daily_results_date_user"
        ),
    )
    op.create_index(
        "ix_daily_results_challenge_date", "daily_results", ["challenge_date"]
    )

    # --- seed the achievement catalog ------------------------------------
    # The catalog lives in ``app/services/achievement_catalog.py``: the startup
    # sync uses the same list, so migration and code cannot drift apart.
    op.bulk_insert(
        sa.table(
            "achievements",
            sa.column("code", sa.String),
            sa.column("title", sa.String),
            sa.column("description", sa.String),
            sa.column("category", sa.String),
            sa.column("xp_bonus", sa.Integer),
            sa.column("goal", sa.Integer),
            sa.column("is_active", sa.Boolean),
            sa.column("sort_order", sa.Integer),
        ),
        [{**item, "is_active": True} for item in ACHIEVEMENT_CATALOG],
    )


def downgrade() -> None:
    op.drop_index("ix_daily_results_challenge_date", table_name="daily_results")
    op.drop_table("daily_results")
    op.drop_table("daily_challenges")
    op.drop_index("ix_user_achievements_user_id", table_name="user_achievements")
    op.drop_table("user_achievements")
    op.drop_table("achievements")
    op.drop_index("ix_xp_awards_user_id", table_name="xp_awards")
    op.drop_table("xp_awards")
    op.drop_index("ix_song_best_scores_song_id", table_name="song_best_scores")
    op.drop_index("ix_song_best_scores_user_id", table_name="song_best_scores")
    op.drop_table("song_best_scores")
    op.drop_index("ix_game_sessions_user_song_mode", table_name="game_sessions")
    op.drop_column("game_sessions", "mode")
    op.drop_column("scores", "notes_played")
    op.drop_column("scores", "notes_total")
    op.drop_column("scores", "avg_timing_error_ms")
    op.drop_column("scores", "completion_percent")
    op.drop_column("users", "level")
    op.drop_column("users", "xp_total")
