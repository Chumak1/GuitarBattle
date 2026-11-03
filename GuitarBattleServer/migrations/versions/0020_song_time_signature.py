"""Song time signature: ``4/4`` by default, editable in the admin panel.

The client needs the meter to place the metronome and the bar lines of the
tablature (``7/8`` means seven eighth-note clicks per bar). Until now every
song was implicitly ``4/4``, so both columns are added with ``server_default
'4'`` and ``NOT NULL``: PostgreSQL fills the existing rows in the same
statement and no separate backfill is required.

* ``time_signature_numerator`` — beats per bar, ``1..32``.
* ``time_signature_denominator`` — note value of one beat. MIDI time
  signatures only allow powers of two, so the check mirrors the schema
  validator: ``1, 2, 4, 8, 16, 32``.

Downgrade drops both checks before the columns; the meter information is
lost, which only affects the client metronome.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0020_song_time_signature"
down_revision = "0019_user_country"
branch_labels = None
depends_on = None

TABLE = "songs"
NUMERATOR_CHECK = "ck_songs_time_signature_numerator"
DENOMINATOR_CHECK = "ck_songs_time_signature_denominator"


def upgrade() -> None:
    op.add_column(
        TABLE,
        sa.Column(
            "time_signature_numerator",
            sa.SmallInteger(),
            nullable=False,
            server_default="4",
        ),
    )
    op.add_column(
        TABLE,
        sa.Column(
            "time_signature_denominator",
            sa.SmallInteger(),
            nullable=False,
            server_default="4",
        ),
    )
    op.create_check_constraint(
        NUMERATOR_CHECK,
        TABLE,
        "time_signature_numerator >= 1 AND time_signature_numerator <= 32",
    )
    op.create_check_constraint(
        DENOMINATOR_CHECK,
        TABLE,
        "time_signature_denominator IN (1, 2, 4, 8, 16, 32)",
    )


def downgrade() -> None:
    op.drop_constraint(DENOMINATOR_CHECK, TABLE, type_="check")
    op.drop_constraint(NUMERATOR_CHECK, TABLE, type_="check")
    op.drop_column(TABLE, "time_signature_denominator")
    op.drop_column(TABLE, "time_signature_numerator")
