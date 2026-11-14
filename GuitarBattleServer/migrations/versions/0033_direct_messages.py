"""Direct messages: one table, additive migration on ``0032_ui_sounds``.

Only ``direct_messages`` is created; no existing table is altered, so the
downgrade simply drops it. ``read_at`` stays NULL until the recipient opens
the thread, and the two indexes serve the thread query and the unread badge.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

from app.models.messages import MAX_BODY_LENGTH

revision = "0033_direct_messages"
down_revision = "0032_ui_sounds"
branch_labels = None
depends_on = None

TABLE = "direct_messages"


def upgrade() -> None:
    op.create_table(
        TABLE,
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column(
            "sender_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "recipient_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column("body", sa.Text(), nullable=False),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column("read_at", sa.DateTime(timezone=True), nullable=True),
        sa.CheckConstraint(
            "sender_id <> recipient_id",
            name="ck_direct_messages_not_self",
        ),
        sa.CheckConstraint(
            f"char_length(body) >= 1 AND char_length(body) <= {MAX_BODY_LENGTH}",
            name="ck_direct_messages_body_length",
        ),
    )
    # "my messages to that peer, newest first" for both directions of a thread.
    op.create_index(
        "ix_direct_messages_sender_recipient_created",
        TABLE,
        ["sender_id", "recipient_id", "created_at"],
    )
    # Unread badge and the read marker of one thread.
    op.create_index(
        "ix_direct_messages_recipient_read",
        TABLE,
        ["recipient_id", "read_at"],
    )


def downgrade() -> None:
    op.drop_index("ix_direct_messages_recipient_read", table_name=TABLE)
    op.drop_index(
        "ix_direct_messages_sender_recipient_created", table_name=TABLE
    )
    op.drop_table(TABLE)
