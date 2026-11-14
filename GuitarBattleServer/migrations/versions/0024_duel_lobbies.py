"""Duel lobbies: shared chat rooms that start a match.

Additive migration on top of ``0023_user_character``:

* ``duel_lobbies`` — a room of two players with a name, an optional password
  hash, the host-chosen song and a ``waiting``/``playing``/``closed`` status.
* ``duel_lobby_messages`` — the chat, with a monotonic ``BIGINT`` id so the
  client can poll incrementally with ``after_id``.
* ``duel_matches.lobby_id`` — nullable link used by *rematch* and
  *return-to-lobby*; ``SET NULL`` keeps the match history when a lobby is
  removed. Existing quick-queue and bot matches keep ``NULL``.

Downgrade drops the link first, then the messages and the rooms.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0024_duel_lobbies"
down_revision = "0023_user_character"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "duel_lobbies",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column("name", sa.String(40), nullable=False),
        # Argon2 hash, never the plaintext password.
        sa.Column("password_hash", sa.String(255), nullable=True),
        sa.Column(
            "host_user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "guest_user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="SET NULL"),
            nullable=True,
        ),
        sa.Column(
            "song_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("songs.id", ondelete="RESTRICT"),
            nullable=True,
        ),
        sa.Column(
            "status", sa.String(16), nullable=False, server_default="waiting"
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
            "status IN ('waiting', 'playing', 'closed')",
            name="ck_duel_lobbies_status",
        ),
        sa.CheckConstraint(
            "char_length(name) >= 1 AND char_length(name) <= 40",
            name="ck_duel_lobbies_name",
        ),
        sa.CheckConstraint(
            "guest_user_id IS NULL OR guest_user_id <> host_user_id",
            name="ck_duel_lobbies_guest_not_host",
        ),
    )
    op.create_index(
        "ix_duel_lobbies_status_created",
        "duel_lobbies",
        ["status", "created_at"],
    )
    op.create_index(
        "ix_duel_lobbies_host_user_id", "duel_lobbies", ["host_user_id"]
    )
    op.create_index(
        "ix_duel_lobbies_guest_user_id", "duel_lobbies", ["guest_user_id"]
    )

    op.create_table(
        "duel_lobby_messages",
        sa.Column("id", sa.BigInteger(), primary_key=True, autoincrement=True),
        sa.Column(
            "lobby_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("duel_lobbies.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column("text", sa.String(500), nullable=False),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.CheckConstraint(
            "char_length(text) >= 1 AND char_length(text) <= 500",
            name="ck_duel_lobby_messages_text",
        ),
    )
    op.create_index(
        "ix_duel_lobby_messages_lobby_id_id",
        "duel_lobby_messages",
        ["lobby_id", "id"],
    )

    op.add_column(
        "duel_matches",
        sa.Column(
            "lobby_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("duel_lobbies.id", ondelete="SET NULL"),
            nullable=True,
        ),
    )
    op.create_index("ix_duel_matches_lobby_id", "duel_matches", ["lobby_id"])


def downgrade() -> None:
    op.drop_index("ix_duel_matches_lobby_id", table_name="duel_matches")
    op.drop_column("duel_matches", "lobby_id")
    op.drop_index(
        "ix_duel_lobby_messages_lobby_id_id",
        table_name="duel_lobby_messages",
    )
    op.drop_table("duel_lobby_messages")
    op.drop_index("ix_duel_lobbies_guest_user_id", table_name="duel_lobbies")
    op.drop_index("ix_duel_lobbies_host_user_id", table_name="duel_lobbies")
    op.drop_index("ix_duel_lobbies_status_created", table_name="duel_lobbies")
    op.drop_table("duel_lobbies")
