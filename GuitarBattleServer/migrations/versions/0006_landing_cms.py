"""Landing page CMS settings and downloadable game builds."""
from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "0006_landing_cms"
down_revision = "0005_email_verification"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "landing_settings",
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column(
            "content",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
    )

    op.create_table(
        "game_builds",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column(
            "version",
            sa.String(64),
            nullable=False,
            server_default=sa.text("''"),
        ),
        sa.Column("filename", sa.String(255), nullable=False),
        sa.Column("stored_name", sa.String(255), nullable=False),
        sa.Column("content_type", sa.String(128), nullable=False),
        sa.Column("size_bytes", sa.BigInteger(), nullable=False),
        sa.Column(
            "is_active",
            sa.Boolean(),
            nullable=False,
            server_default=sa.text("false"),
        ),
        sa.Column(
            "uploaded_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
    )
    op.create_index("ix_game_builds_is_active", "game_builds", ["is_active"])


def downgrade() -> None:
    op.drop_index("ix_game_builds_is_active", table_name="game_builds")
    op.drop_table("game_builds")
    op.drop_table("landing_settings")
