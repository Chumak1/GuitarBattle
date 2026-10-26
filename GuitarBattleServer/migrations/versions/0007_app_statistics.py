"""App activity sessions and game launches for the admin statistics page."""
from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "0007_app_statistics"
down_revision = "0006_landing_cms"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "app_sessions",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column("client_session_id", sa.String(64), nullable=False),
        sa.Column("started_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("last_seen_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("ended_at", sa.DateTime(timezone=True)),
        sa.Column("app_version", sa.String(32)),
        sa.Column("platform", sa.String(32)),
        sa.UniqueConstraint(
            "user_id", "client_session_id", name="uq_app_sessions_user_client"
        ),
    )
    op.create_index("ix_app_sessions_user_id", "app_sessions", ["user_id"])
    op.create_index(
        "ix_app_sessions_user_id_started_at", "app_sessions", ["user_id", "started_at"]
    )

    op.create_table(
        "game_launches",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "app_session_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("app_sessions.id", ondelete="SET NULL"),
        ),
        sa.Column("game_key", sa.String(120), nullable=False),
        sa.Column(
            "song_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("songs.id", ondelete="SET NULL"),
        ),
        sa.Column("mode", sa.String(32)),
        sa.Column("started_at", sa.DateTime(timezone=True), nullable=False),
    )
    op.create_index("ix_game_launches_user_id", "game_launches", ["user_id"])
    op.create_index("ix_game_launches_game_key", "game_launches", ["game_key"])
    op.create_index("ix_game_launches_started_at", "game_launches", ["started_at"])


def downgrade() -> None:
    op.drop_index("ix_game_launches_started_at", table_name="game_launches")
    op.drop_index("ix_game_launches_game_key", table_name="game_launches")
    op.drop_index("ix_game_launches_user_id", table_name="game_launches")
    op.drop_table("game_launches")
    op.drop_index("ix_app_sessions_user_id_started_at", table_name="app_sessions")
    op.drop_index("ix_app_sessions_user_id", table_name="app_sessions")
    op.drop_table("app_sessions")
