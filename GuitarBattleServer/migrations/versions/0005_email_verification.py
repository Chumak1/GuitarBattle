"""Email verification, password reset codes and token invalidation."""
from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "0005_email_verification"
down_revision = "0004_competitions"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "users",
        sa.Column(
            "token_version",
            sa.Integer(),
            nullable=False,
            server_default="0",
        ),
    )

    op.create_table(
        "email_auth_codes",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column("purpose", sa.String(32), nullable=False),
        sa.Column("code_hash", sa.String(64), nullable=False),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column(
            "sent_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column(
            "attempts",
            sa.Integer(),
            nullable=False,
            server_default="0",
        ),
        sa.Column("consumed_at", sa.DateTime(timezone=True)),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
    )
    op.create_index(
        "ix_email_auth_codes_user_id",
        "email_auth_codes",
        ["user_id"],
    )
    op.create_index(
        "ix_email_auth_codes_purpose",
        "email_auth_codes",
        ["purpose"],
    )
    op.create_index(
        "ix_email_auth_codes_lookup",
        "email_auth_codes",
        ["user_id", "purpose", "consumed_at"],
    )

    # Accounts created before email verification existed are grandfathered in:
    # otherwise the first deployment of this migration would lock every current
    # player out of the client.
    op.execute("UPDATE users SET email_verified = TRUE WHERE email_verified = FALSE")


def downgrade() -> None:
    op.drop_index("ix_email_auth_codes_lookup", table_name="email_auth_codes")
    op.drop_index("ix_email_auth_codes_purpose", table_name="email_auth_codes")
    op.drop_index("ix_email_auth_codes_user_id", table_name="email_auth_codes")
    op.drop_table("email_auth_codes")
    op.drop_column("users", "token_version")
