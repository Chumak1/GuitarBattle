"""Friends, blocks, privacy, rivals, activity and social notifications.

Additive migration on top of ``0030_scoring_normalization``. It creates the
seven tables of the "Friends" slice only; the existing ``users``,
``duel_participants``, ``song_best_scores``, ``user_achievements``,
``user_campaign_progress`` and ``user_characters`` tables are read, never
altered. Ratings and levels are not duplicated: the public card aggregates them
from the duel and progression tables at request time.

* ``friendships`` — one canonical row per pair (``user_id < friend_id``), so a
  friendship is mutual and stored exactly once.
* ``friend_requests`` — request history; a partial unique index
  (``status = 'pending'``) allows only one pending request per ordered pair and
  the service rejects the reverse direction as well.
* ``user_blocks`` — directional; the service treats either direction as hidden.
* ``user_privacy`` — 1:1 settings, defaults everything visible. No backfill is
  needed because a missing row means "all defaults".
* ``user_rival`` — 1:1, must be a friend (enforced by the service).
* ``player_activity`` — the visible feed events.
* ``social_notifications`` — ``UNIQUE(user_id, dedupe_key)`` makes notification
  delivery idempotent for retried mutations.

Every foreign key references ``users.id`` with ``ON DELETE CASCADE``, and every
foreign key used for lookups has an index.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0031_social_friends"
down_revision = "0030_scoring_normalization"
branch_labels = None
depends_on = None


def _user_fk(name: str) -> sa.Column:
    return sa.Column(
        name,
        postgresql.UUID(as_uuid=True),
        sa.ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )


def upgrade() -> None:
    op.create_table(
        "friendships",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        _user_fk("user_id"),
        _user_fk("friend_id"),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.UniqueConstraint("user_id", "friend_id", name="uq_friendships_pair"),
        sa.CheckConstraint("user_id < friend_id", name="ck_friendships_order"),
    )
    op.create_index("ix_friendships_friend_id", "friendships", ["friend_id"])

    op.create_table(
        "friend_requests",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        _user_fk("from_user_id"),
        _user_fk("to_user_id"),
        sa.Column(
            "status",
            sa.String(16),
            nullable=False,
            server_default="pending",
        ),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column(
            "responded_at", sa.DateTime(timezone=True), nullable=True
        ),
        sa.CheckConstraint(
            "status IN ('pending', 'accepted', 'declined', 'cancelled')",
            name="ck_friend_requests_status",
        ),
        sa.CheckConstraint(
            "from_user_id <> to_user_id", name="ck_friend_requests_not_self"
        ),
    )
    op.create_index(
        "uq_friend_requests_pending_pair",
        "friend_requests",
        ["from_user_id", "to_user_id"],
        unique=True,
        postgresql_where=sa.text("status = 'pending'"),
    )
    op.create_index(
        "ix_friend_requests_from_user_id",
        "friend_requests",
        ["from_user_id", "status"],
    )
    op.create_index(
        "ix_friend_requests_to_user_id",
        "friend_requests",
        ["to_user_id", "status"],
    )

    op.create_table(
        "user_blocks",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        _user_fk("blocker_id"),
        _user_fk("blocked_id"),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.UniqueConstraint(
            "blocker_id", "blocked_id", name="uq_user_blocks_pair"
        ),
        sa.CheckConstraint(
            "blocker_id <> blocked_id", name="ck_user_blocks_not_self"
        ),
    )
    op.create_index("ix_user_blocks_blocked_id", "user_blocks", ["blocked_id"])

    op.create_table(
        "user_privacy",
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            primary_key=True,
        ),
        sa.Column(
            "request_policy",
            sa.String(16),
            nullable=False,
            server_default="everyone",
        ),
        sa.Column(
            "show_online", sa.Boolean(), nullable=False, server_default=sa.true()
        ),
        sa.Column(
            "show_profile", sa.Boolean(), nullable=False, server_default=sa.true()
        ),
        sa.Column(
            "show_stats", sa.Boolean(), nullable=False, server_default=sa.true()
        ),
        sa.Column(
            "show_activity", sa.Boolean(), nullable=False, server_default=sa.true()
        ),
        sa.Column(
            "allow_challenges",
            sa.Boolean(),
            nullable=False,
            server_default=sa.true(),
        ),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.CheckConstraint(
            "request_policy IN ('everyone', 'friends', 'nobody')",
            name="ck_user_privacy_request_policy",
        ),
    )

    op.create_table(
        "user_rival",
        sa.Column(
            "user_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            primary_key=True,
        ),
        _user_fk("rival_user_id"),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.CheckConstraint(
            "user_id <> rival_user_id", name="ck_user_rival_not_self"
        ),
    )
    op.create_index(
        "ix_user_rival_rival_user_id", "user_rival", ["rival_user_id"]
    )

    op.create_table(
        "player_activity",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        _user_fk("user_id"),
        sa.Column("kind", sa.String(16), nullable=False),
        sa.Column(
            "payload",
            postgresql.JSONB(),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column(
            "visible", sa.Boolean(), nullable=False, server_default=sa.true()
        ),
        sa.CheckConstraint(
            "kind IN ('record', 'tower', 'character', 'level', 'duel_win', "
            "'achievement')",
            name="ck_player_activity_kind",
        ),
    )
    op.create_index(
        "ix_player_activity_user_created",
        "player_activity",
        ["user_id", "created_at"],
    )

    op.create_table(
        "social_notifications",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        _user_fk("user_id"),
        sa.Column("kind", sa.String(32), nullable=False),
        sa.Column(
            "payload",
            postgresql.JSONB(),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
        sa.Column("read_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.Column("dedupe_key", sa.String(160), nullable=False),
        sa.UniqueConstraint(
            "user_id", "dedupe_key", name="uq_social_notifications_user_dedupe"
        ),
    )
    op.create_index(
        "ix_social_notifications_user_created",
        "social_notifications",
        ["user_id", "created_at"],
    )


def downgrade() -> None:
    op.drop_index(
        "ix_social_notifications_user_created",
        table_name="social_notifications",
    )
    op.drop_table("social_notifications")

    op.drop_index(
        "ix_player_activity_user_created", table_name="player_activity"
    )
    op.drop_table("player_activity")

    op.drop_index("ix_user_rival_rival_user_id", table_name="user_rival")
    op.drop_table("user_rival")

    op.drop_table("user_privacy")

    op.drop_index("ix_user_blocks_blocked_id", table_name="user_blocks")
    op.drop_table("user_blocks")

    op.drop_index(
        "ix_friend_requests_to_user_id", table_name="friend_requests"
    )
    op.drop_index(
        "ix_friend_requests_from_user_id", table_name="friend_requests"
    )
    op.drop_index(
        "uq_friend_requests_pending_pair", table_name="friend_requests"
    )
    op.drop_table("friend_requests")

    op.drop_index("ix_friendships_friend_id", table_name="friendships")
    op.drop_table("friendships")
