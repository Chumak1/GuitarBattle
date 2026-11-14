"""UI sounds: the fixed event registry, uploaded clips and user preferences.

Additive migration on top of ``0031_social_friends``.

``ui_sound_events`` is the single registry of UI sound events. It is seeded
here from :data:`app.services.ui_sound_registry.UI_SOUND_REGISTRY` (the same
list the JUCE client compiles in), because the catalog is product data, not
user data: rows are created once, never by the API. Every row starts active,
with gain 1.0, version 0 and no file. The admin panel uploads files later; a
replacement bumps ``version`` in place so the client cache is invalidated.

``user_ui_sound_settings`` is 1:1 with an account and defaults to "sounds on,
volume 0.8, every category on". A missing row means exactly that, so existing
accounts need no backfill.

Only these two tables are created; no existing table is altered.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

from app.services.ui_sound_registry import UI_SOUND_REGISTRY

revision = "0032_ui_sounds"
down_revision = "0031_social_friends"
branch_labels = None
depends_on = None

EVENTS_TABLE = "ui_sound_events"
SETTINGS_TABLE = "user_ui_sound_settings"
KEY_INDEX = "ix_ui_sound_events_key"


def upgrade() -> None:
    op.create_table(
        EVENTS_TABLE,
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("key", sa.String(length=64), nullable=False),
        sa.Column("name", sa.String(length=120), nullable=False),
        sa.Column("category", sa.String(length=16), nullable=False),
        sa.Column(
            "active", sa.Boolean(), nullable=False, server_default=sa.true()
        ),
        sa.Column("gain", sa.Float(), nullable=False, server_default="1"),
        sa.Column("stored_name", sa.String(length=255), nullable=True),
        sa.Column("original_name", sa.String(length=255), nullable=True),
        sa.Column("content_type", sa.String(length=120), nullable=True),
        sa.Column("size_bytes", sa.BigInteger(), nullable=True),
        sa.Column(
            "version", sa.Integer(), nullable=False, server_default="0"
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
        sa.CheckConstraint(
            "category IN ('navigation', 'action', 'system')",
            name="ck_ui_sound_events_category",
        ),
        sa.CheckConstraint(
            "gain >= 0 AND gain <= 1", name="ck_ui_sound_events_gain"
        ),
        sa.CheckConstraint("version >= 0", name="ck_ui_sound_events_version"),
    )
    # One row per event key; the client and the admin panel both address rows
    # by key, never by the internal id.
    op.create_index(KEY_INDEX, EVENTS_TABLE, ["key"], unique=True)

    registry = sa.table(
        EVENTS_TABLE,
        sa.column("key", sa.String),
        sa.column("name", sa.String),
        sa.column("category", sa.String),
        sa.column("active", sa.Boolean),
        sa.column("gain", sa.Float),
        sa.column("version", sa.Integer),
    )
    op.bulk_insert(
        registry,
        [
            {
                "key": item.key,
                "name": item.name,
                "category": item.category,
                "active": True,
                "gain": 1.0,
                "version": 0,
            }
            for item in UI_SOUND_REGISTRY
        ],
    )

    op.create_table(
        SETTINGS_TABLE,
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            primary_key=True,
        ),
        sa.Column(
            "enabled", sa.Boolean(), nullable=False, server_default=sa.true()
        ),
        sa.Column(
            "volume", sa.Float(), nullable=False, server_default="0.8"
        ),
        sa.Column(
            "navigation",
            sa.Boolean(),
            nullable=False,
            server_default=sa.true(),
        ),
        sa.Column(
            "action", sa.Boolean(), nullable=False, server_default=sa.true()
        ),
        sa.Column("system", sa.Boolean(), nullable=False, server_default=sa.true()),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.CheckConstraint(
            "volume >= 0 AND volume <= 1",
            name="ck_user_ui_sound_settings_volume",
        ),
    )


def downgrade() -> None:
    op.drop_table(SETTINGS_TABLE)
    op.drop_index(KEY_INDEX, table_name=EVENTS_TABLE)
    op.drop_table(EVENTS_TABLE)
