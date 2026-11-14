"""Duel rooms: two-step "start the duel?" confirmation.

Additive migration on top of ``0026_campaigns``:

* ``duel_lobbies.start_requested_by`` — the host that asked to start; ``NULL``
  means there is no pending request (a room reset, a decline, a cancel or a
  player leaving clears it).
* ``duel_lobbies.start_host_confirmed`` / ``start_guest_confirmed`` — the two
  confirmations. The match is created only when both are ``TRUE``; until then
  the room stays ``waiting`` so the song download happens *after* the
  confirmation.

Existing rooms get ``NULL``/``FALSE``/``FALSE``, i.e. "no pending request",
which is exactly the pre-migration behaviour.

Downgrade drops the three columns, losing only pending (not confirmed) requests.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0027_duel_lobby_start_confirm"
down_revision = "0026_campaigns"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "duel_lobbies",
        sa.Column(
            "start_requested_by",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="SET NULL"),
            nullable=True,
        ),
    )
    op.add_column(
        "duel_lobbies",
        sa.Column(
            "start_host_confirmed",
            sa.Boolean(),
            nullable=False,
            server_default=sa.false(),
        ),
    )
    op.add_column(
        "duel_lobbies",
        sa.Column(
            "start_guest_confirmed",
            sa.Boolean(),
            nullable=False,
            server_default=sa.false(),
        ),
    )


def downgrade() -> None:
    op.drop_column("duel_lobbies", "start_guest_confirmed")
    op.drop_column("duel_lobbies", "start_host_confirmed")
    op.drop_column("duel_lobbies", "start_requested_by")
