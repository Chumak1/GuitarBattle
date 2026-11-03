"""Country of registration and sign-in: five nullable ``users`` columns.

The admin statistics table shows where players come from, but the desktop and
web clients never send a country: the server resolves it from the IP that
created or last signed in to the account.

Every column is nullable and there is no backfill. Historical IPs were never
stored, so existing accounts keep ``NULL`` and the admin panel renders "—";
the next successful sign-in of such an account fills ``last_login_ip`` and
retries the country lookup in the background.

* ``registration_ip`` / ``last_login_ip`` — ``INET``, public addresses only
  (private, loopback and link-local peers are dropped before they reach the
  database).
* ``country_code`` — ISO 3166-1 alpha-2 in upper case, indexed for the admin
  country filter.
* ``country_name`` — the provider's English name, for display.
* ``country_checked_at`` — when the country was resolved. A failed lookup
  leaves it ``NULL`` on purpose, so a later sign-in retries.

Downgrade drops the index and the five columns; the stored IPs and countries
are lost, which only affects the new admin columns.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects import postgresql

revision = "0019_user_country"
down_revision = "0018_game_build_artifact"
branch_labels = None
depends_on = None

TABLE = "users"
INDEX = "ix_users_country_code"

# Dropped in reverse order of creation so the downgrade mirrors the upgrade.
COLUMNS = (
    "registration_ip",
    "last_login_ip",
    "country_code",
    "country_name",
    "country_checked_at",
)


def upgrade() -> None:
    op.add_column(TABLE, sa.Column("registration_ip", postgresql.INET(), nullable=True))
    op.add_column(TABLE, sa.Column("last_login_ip", postgresql.INET(), nullable=True))
    op.add_column(TABLE, sa.Column("country_code", sa.String(2), nullable=True))
    op.add_column(TABLE, sa.Column("country_name", sa.String(64), nullable=True))
    op.add_column(
        TABLE, sa.Column("country_checked_at", sa.DateTime(timezone=True), nullable=True)
    )
    op.create_index(INDEX, TABLE, ["country_code"])


def downgrade() -> None:
    op.drop_index(INDEX, table_name=TABLE)
    for column in reversed(COLUMNS):
        op.drop_column(TABLE, column)
