"""Gameplay visuals: UI-level characters, stage backgrounds and per-song choice.

Additive migration. It creates two tables and adds two nullable columns to
``songs``; no existing row is rewritten.

* ``ui_characters`` — a unique display name and one transparent PNG spritesheet
  with eight poses (4x2 grid). The stored file name is generated, never the
  client's file name.
* ``stage_levels`` — a stage background and the character it belongs to.
  ``character_id`` is ``ON DELETE RESTRICT``: a character that still owns a
  stage cannot be removed (the admin endpoint returns a clear 409).
* ``songs.character_id`` / ``songs.stage_id`` — optional selection, both
  ``ON DELETE SET NULL``. Deleting a visual never breaks a song; the public
  catalogue simply reports the four visual fields as ``null``.

Downgrade drops the song columns (the selection is lost, songs stay), then the
stage table and finally the character table.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0022_gameplay_visuals"
down_revision = "0021_duels"
branch_labels = None
depends_on = None

CHARACTERS = "ui_characters"
STAGES = "stage_levels"
SONGS = "songs"

CHARACTER_SHEET_KEY = "ix_ui_characters_name"
STAGE_CHARACTER_KEY = "ix_stage_levels_character_id"
SONG_CHARACTER_KEY = "ix_songs_character_id"
SONG_STAGE_KEY = "ix_songs_stage_id"
SONG_CHARACTER_FK = "fk_songs_character_id_ui_characters"
SONG_STAGE_FK = "fk_songs_stage_id_stage_levels"


def upgrade() -> None:
    op.create_table(
        CHARACTERS,
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column("name", sa.String(length=120), nullable=False),
        sa.Column("sheet_key", sa.String(length=255), nullable=False),
        sa.Column("original_name", sa.String(length=255), nullable=True),
        sa.Column("content_type", sa.String(length=120), nullable=False),
        sa.Column("size_bytes", sa.Integer(), nullable=False),
        sa.Column("sha256", sa.String(length=64), nullable=False),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.text("now()"),
        ),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.text("now()"),
        ),
    )
    # Unique index serves both the uniqueness rule and the name lookup.
    op.create_index(CHARACTER_SHEET_KEY, CHARACTERS, ["name"], unique=True)

    op.create_table(
        STAGES,
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column("name", sa.String(length=120), nullable=False),
        sa.Column(
            "character_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey(
                "ui_characters.id",
                ondelete="RESTRICT",
                name="fk_stage_levels_character_id_ui_characters",
            ),
            nullable=False,
        ),
        sa.Column("background_key", sa.String(length=255), nullable=False),
        sa.Column("original_name", sa.String(length=255), nullable=True),
        sa.Column("content_type", sa.String(length=120), nullable=False),
        sa.Column("size_bytes", sa.Integer(), nullable=False),
        sa.Column("sha256", sa.String(length=64), nullable=False),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.text("now()"),
        ),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.text("now()"),
        ),
    )
    op.create_index(STAGE_CHARACTER_KEY, STAGES, ["character_id"], unique=False)

    op.add_column(
        SONGS,
        sa.Column("character_id", postgresql.UUID(as_uuid=True), nullable=True),
    )
    op.add_column(
        SONGS,
        sa.Column("stage_id", postgresql.UUID(as_uuid=True), nullable=True),
    )
    op.create_foreign_key(
        SONG_CHARACTER_FK,
        SONGS,
        CHARACTERS,
        ["character_id"],
        ["id"],
        ondelete="SET NULL",
    )
    op.create_foreign_key(
        SONG_STAGE_FK,
        SONGS,
        STAGES,
        ["stage_id"],
        ["id"],
        ondelete="SET NULL",
    )
    op.create_index(SONG_CHARACTER_KEY, SONGS, ["character_id"], unique=False)
    op.create_index(SONG_STAGE_KEY, SONGS, ["stage_id"], unique=False)


def downgrade() -> None:
    op.drop_index(SONG_STAGE_KEY, table_name=SONGS)
    op.drop_index(SONG_CHARACTER_KEY, table_name=SONGS)
    op.drop_constraint(SONG_STAGE_FK, SONGS, type_="foreignkey")
    op.drop_constraint(SONG_CHARACTER_FK, SONGS, type_="foreignkey")
    op.drop_column(SONGS, "stage_id")
    op.drop_column(SONGS, "character_id")

    op.drop_index(STAGE_CHARACTER_KEY, table_name=STAGES)
    op.drop_table(STAGES)

    op.drop_index(CHARACTER_SHEET_KEY, table_name=CHARACTERS)
    op.drop_table(CHARACTERS)
