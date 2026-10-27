"""Privacy-policy consent at registration and persistent refresh sessions.

Both changes are purely additive:

* ``users.privacy_consent_at`` stays NULL for every existing account, so no
  user row is modified by this migration.
* ``refresh_sessions`` is a new table. Only a sha256 hash of the refresh token
  is stored; ``replaced_by_id`` links a rotated token to its successor so reuse
  can be detected and the whole chain revoked.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0014_privacy_refresh"
down_revision = "0013_app_audio"
branch_labels = None
depends_on = None

REFRESH_TABLE = "refresh_sessions"


def upgrade() -> None:
    op.add_column(
        "users",
        sa.Column("privacy_consent_at", sa.DateTime(timezone=True), nullable=True),
    )

    op.create_table(
        REFRESH_TABLE,
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        # sha256 hex digest: the raw token is never persisted.
        sa.Column("token_hash", sa.String(length=64), nullable=False),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("revoked_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column(
            "replaced_by_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey(f"{REFRESH_TABLE}.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.Column("user_agent", sa.String(length=512), nullable=True),
    )
    op.create_index(
        "ix_refresh_sessions_user_id",
        REFRESH_TABLE,
        ["user_id"],
        unique=False,
    )
    op.create_index(
        "ix_refresh_sessions_token_hash",
        REFRESH_TABLE,
        ["token_hash"],
        unique=True,
    )


def downgrade() -> None:
    # Only the new session table and the new column are removed; users,
    # passwords, scores, ratings and achievements are untouched.
    op.drop_index("ix_refresh_sessions_token_hash", table_name=REFRESH_TABLE)
    op.drop_index("ix_refresh_sessions_user_id", table_name=REFRESH_TABLE)
    op.drop_table(REFRESH_TABLE)
    op.drop_column("users", "privacy_consent_at")
