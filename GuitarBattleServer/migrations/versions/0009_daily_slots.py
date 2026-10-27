"""Daily challenge by weekday: ``songs.daily_weekday`` and ``daily_slots``.

The schedule is now explicit instead of derived from a hash of the date:
administrators assign one song to each weekday, and the first request for a
date freezes that song into ``daily_challenges`` (historical rows stay
untouched, so already played days never change).
"""
from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "0009_daily_slots"
down_revision = "0008_progression"
branch_labels = None
depends_on = None


def upgrade() -> None:
    # ``NULL`` = ordinary catalogue song; 0..6 = Monday..Sunday (UTC), and the
    # song is hidden from the catalogue/global rating while it holds a slot.
    op.add_column("songs", sa.Column("daily_weekday", sa.SmallInteger(), nullable=True))
    op.create_check_constraint(
        "ck_songs_daily_weekday_range",
        "songs",
        "daily_weekday IS NULL OR (daily_weekday >= 0 AND daily_weekday <= 6)",
    )
    op.create_index("ix_songs_daily_weekday", "songs", ["daily_weekday"])

    op.create_table(
        "daily_slots",
        sa.Column("weekday", sa.SmallInteger(), primary_key=True),
        sa.Column(
            "song_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("songs.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column(
            "updated_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.func.now(),
        ),
        sa.CheckConstraint(
            "weekday >= 0 AND weekday <= 6", name="ck_daily_slots_weekday_range"
        ),
    )
    op.create_index("ix_daily_slots_song_id", "daily_slots", ["song_id"])

    # Keep the two representations consistent for databases that already have
    # ``daily_challenges`` rows: backfill nothing, because the hash-based picker
    # was not an administrator decision. Existing rows are historical facts and
    # stay as they are.

    # A slot and the song marker must agree from the very first write, so the
    # table starts empty and admins fill it in through /admin/daily.


def downgrade() -> None:
    op.drop_index("ix_daily_slots_song_id", table_name="daily_slots")
    op.drop_table("daily_slots")
    op.drop_index("ix_songs_daily_weekday", table_name="songs")
    op.drop_constraint("ck_songs_daily_weekday_range", "songs", type_="check")
    op.drop_column("songs", "daily_weekday")
