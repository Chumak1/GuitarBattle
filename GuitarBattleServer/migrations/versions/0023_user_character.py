"""Player character selection and its player-facing integer id.

Additive migration on top of ``0022_gameplay_visuals``:

* ``ui_characters.public_id`` — a stable integer identifier for the desktop
  client. The internal primary key stays a UUID so ``songs.character_id``,
  ``stage_levels.character_id`` and the admin API keep their existing contract.
  PostgreSQL fills existing rows while adding the identity column.
* ``users.character_id`` — optional selection, ``ON DELETE SET NULL``: deleting a
  character in the admin panel clears the choice instead of failing or touching
  the account.

Downgrade drops the user column and the public identifier; no other table is
touched.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0023_user_character"
down_revision = "0022_gameplay_visuals"
branch_labels = None
depends_on = None

CHARACTERS = "ui_characters"
USERS = "users"

PUBLIC_ID_KEY = "uq_ui_characters_public_id"
USER_CHARACTER_KEY = "ix_users_character_id"
USER_CHARACTER_FK = "fk_users_character_id_ui_characters"


def upgrade() -> None:
    op.add_column(
        CHARACTERS,
        sa.Column("public_id", sa.BigInteger(), sa.Identity(), nullable=False),
    )
    op.create_unique_constraint(PUBLIC_ID_KEY, CHARACTERS, ["public_id"])

    op.add_column(
        USERS,
        sa.Column("character_id", postgresql.UUID(as_uuid=True), nullable=True),
    )
    op.create_foreign_key(
        USER_CHARACTER_FK,
        USERS,
        CHARACTERS,
        ["character_id"],
        ["id"],
        ondelete="SET NULL",
    )
    op.create_index(USER_CHARACTER_KEY, USERS, ["character_id"], unique=False)


def downgrade() -> None:
    op.drop_index(USER_CHARACTER_KEY, table_name=USERS)
    op.drop_constraint(USER_CHARACTER_FK, USERS, type_="foreignkey")
    op.drop_column(USERS, "character_id")

    op.drop_constraint(PUBLIC_ID_KEY, CHARACTERS, type_="unique")
    op.drop_column(CHARACTERS, "public_id")
