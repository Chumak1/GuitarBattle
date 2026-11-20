"""Administrator artwork slots and player avatars.

Additive on top of ``0035_guitar_skills``; nothing existing is rewritten.

* ``artwork_assets`` — one row per public artwork slot (``hero-battle``,
  ``hero-duel``, ``default-avatar``) holding the origin-relative URL of the
  uploaded image. The slot is unique, so an upload replaces the row.
* ``users.avatar_image_url`` — the origin-relative URL of the player-uploaded
  avatar. NULL (every existing account) keeps the initial-letter placeholder.

The downgrade drops the new table and the new column only.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0036_artwork_avatars"
down_revision = "0035_guitar_skills"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "users",
        sa.Column("avatar_image_url", sa.String(length=500), nullable=True),
    )

    op.create_table(
        "artwork_assets",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("slot", sa.String(length=32), nullable=False, unique=True),
        sa.Column("url", sa.String(length=500), nullable=False),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
    )


def downgrade() -> None:
    op.drop_table("artwork_assets")
    op.drop_column("users", "avatar_image_url")
