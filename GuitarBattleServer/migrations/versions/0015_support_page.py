"""Support page CMS: draft/published document, versions, methods, counters.

The tables are additive. Initial content is *not* inserted here: the defaults
live in ``app/services/support_content.py`` and are seeded idempotently on
startup (and on the first support request), so a fresh install, a restored
backup and the test suite all get the same document without a data migration
that could overwrite edited content.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0015_support_page"
down_revision = "0014_privacy_refresh"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "support_page",
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column(
            "draft_content",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
        sa.Column(
            "published_content",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
        sa.Column(
            "revision", sa.Integer(), nullable=False, server_default=sa.text("1")
        ),
        sa.Column(
            "published_revision",
            sa.Integer(),
            nullable=False,
            server_default=sa.text("0"),
        ),
        sa.Column(
            "draft_updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column(
            "draft_updated_by",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.Column("published_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column(
            "published_by",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.Column(
            "seed_version", sa.Integer(), nullable=False, server_default=sa.text("0")
        ),
    )

    op.create_table(
        "support_page_versions",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column(
            "content",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
        sa.Column("revision", sa.Integer(), nullable=False),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column(
            "created_by",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.Column(
            "comment", sa.String(length=500), nullable=False, server_default=""
        ),
    )

    op.create_table(
        "support_methods",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("key", sa.String(length=40), nullable=False, unique=True),
        sa.Column(
            "title", sa.String(length=120), nullable=False, server_default=""
        ),
        sa.Column(
            "button_label", sa.String(length=160), nullable=False, server_default=""
        ),
        sa.Column("url", sa.String(length=2048), nullable=False),
        sa.Column(
            "description", sa.String(length=500), nullable=False, server_default=""
        ),
        sa.Column(
            "is_active", sa.Boolean(), nullable=False, server_default=sa.text("true")
        ),
        sa.Column(
            "open_in_new_tab",
            sa.Boolean(),
            nullable=False,
            server_default=sa.text("true"),
        ),
        sa.Column(
            "sort_order", sa.Integer(), nullable=False, server_default=sa.text("0")
        ),
        sa.Column(
            "style", sa.String(length=20), nullable=False, server_default="primary"
        ),
        sa.Column(
            "sections",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'[]'::jsonb"),
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
    )

    op.create_table(
        "support_event_counters",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("event", sa.String(length=32), nullable=False),
        sa.Column(
            "method_key", sa.String(length=40), nullable=False, server_default=""
        ),
        sa.Column("day", sa.Date(), nullable=False),
        sa.Column(
            "count", sa.Integer(), nullable=False, server_default=sa.text("0")
        ),
        sa.UniqueConstraint(
            "event",
            "method_key",
            "day",
            name="uq_support_events_event_method_day",
        ),
    )


def downgrade() -> None:
    op.drop_table("support_event_counters")
    op.drop_table("support_methods")
    op.drop_table("support_page_versions")
    op.drop_table("support_page")
