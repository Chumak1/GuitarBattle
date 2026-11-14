"""Campaign cover images and descriptions.

Two additive columns on ``campaigns``: the origin-relative URL of the
administrator-uploaded cover (``/media/campaigns/<name>``) and a short
description. Both are nullable, so every existing tower keeps working with the
client's placeholder. The downgrade only drops the two columns; no other table
is touched and no data is rewritten.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0034_campaign_cover"
down_revision = "0033_direct_messages"
branch_labels = None
depends_on = None

TABLE = "campaigns"


def upgrade() -> None:
    op.add_column(
        TABLE,
        sa.Column("cover_image_url", sa.String(length=500), nullable=True),
    )
    op.add_column(
        TABLE,
        sa.Column("description", sa.String(length=400), nullable=True),
    )


def downgrade() -> None:
    op.drop_column(TABLE, "description")
    op.drop_column(TABLE, "cover_image_url")
