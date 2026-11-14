"""Stage levels become standalone backgrounds.

A stage level is now just a scene name plus one uploaded background image. It
no longer belongs to a UI-level character, so the admin form asks for exactly
two things and the API no longer resolves a character name.

Upgrade drops ``stage_levels.character_id`` together with its index and the
``ON DELETE RESTRICT`` foreign key. ``ui_characters`` and ``songs.character_id``
are untouched: a song still picks its own character independently from its
stage, and ``GET /songs`` keeps returning ``character_name`` /
``character_sheet_url`` from the song's own character.

Downgrade recreates the column as **nullable** and restores the old foreign key
and index. The original per-stage assignments cannot be restored because they
are dropped with the column; every existing stage is recreated with
``character_id = NULL``. The column stays nullable in the downgraded schema so
those rows remain valid (the old application code treated a missing character
as an error, which is exactly the behaviour this migration removes).
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0025_stage_level_no_character"
down_revision = "0024_duel_lobbies"
branch_labels = None
depends_on = None

STAGES = "stage_levels"
CHARACTERS = "ui_characters"

STAGE_CHARACTER_KEY = "ix_stage_levels_character_id"
STAGE_CHARACTER_FK = "fk_stage_levels_character_id_ui_characters"


def upgrade() -> None:
    op.drop_constraint(STAGE_CHARACTER_FK, STAGES, type_="foreignkey")
    op.drop_index(STAGE_CHARACTER_KEY, table_name=STAGES)
    op.drop_column(STAGES, "character_id")


def downgrade() -> None:
    # Nullable because the old assignments are gone for good; new rows were
    # created without a character. Old values cannot be restored.
    op.add_column(
        STAGES,
        sa.Column(
            "character_id",
            postgresql.UUID(as_uuid=True),
            nullable=True,
        ),
    )
    op.create_foreign_key(
        STAGE_CHARACTER_FK,
        STAGES,
        CHARACTERS,
        ["character_id"],
        ["id"],
        ondelete="RESTRICT",
    )
    op.create_index(STAGE_CHARACTER_KEY, STAGES, ["character_id"], unique=False)
