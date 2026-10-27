"""Global app audio assets: the background music track.

A single table keyed by role holds the *current* file. Replacement updates the
row in place, so the public URL is stable; the previous file is unlinked only
after the new record is committed.

``song_files`` is intentionally not reused: background music is not attached to
a song, it is not versioned per song and it must never appear in the catalogue,
in readiness or in publish requirements.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0013_app_audio"
down_revision = "0012_song_archive"
branch_labels = None
depends_on = None

TABLE = "app_audio_assets"
ROLE_INDEX = "ix_app_audio_assets_role"


def upgrade() -> None:
    op.create_table(
        TABLE,
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("role", sa.String(length=32), nullable=False),
        sa.Column("object_key", sa.String(length=1024), nullable=False),
        sa.Column("original_name", sa.String(length=255), nullable=True),
        sa.Column("content_type", sa.String(length=120), nullable=False),
        sa.Column("size_bytes", sa.BigInteger(), nullable=False),
        sa.Column("sha256", sa.String(length=64), nullable=False),
        sa.Column(
            "uploaded_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.text("now()"),
        ),
    )
    # Unique per role: one current background track, no duplicate rows.
    op.create_index(ROLE_INDEX, TABLE, ["role"], unique=True)


def downgrade() -> None:
    # Only uploaded app audio is dropped; songs, stems, MIDI and user data are
    # untouched. Re-running ``upgrade`` recreates the empty table.
    op.drop_index(ROLE_INDEX, table_name=TABLE)
    op.drop_table(TABLE)
