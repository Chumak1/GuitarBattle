"""Song archive: keep player results when a song leaves the catalogue.

Hiding a song used to mean either flipping ``is_published`` (safe) or deleting
the row (destructive: ``song_best_scores.song_id`` was ``ON DELETE CASCADE``, so
a delete wiped personal bests, and the mastery/rating derived from them).

This migration separates *availability* from *result retention*:

* ``songs.archived_at`` — a nullable timestamp. ``NULL`` means the song is a
  working catalogue entry; a value means it was archived (hidden from the
  public catalogue and from new game sessions) while every reference to its
  stable ``id`` stays valid.
* ``song_best_scores.song_id`` — ``CASCADE`` -> ``RESTRICT``: user data can no
  longer be removed as a side effect of deleting a song.
* ``daily_slots.song_id`` — ``CASCADE`` -> ``RESTRICT``: the schedule is not
  user data, but a hard delete must not silently rewrite it.

``daily_challenges.song_id`` (0008) and ``game_sessions.song_id`` (0001) were
already ``RESTRICT``; they keep protecting daily results and attempt history.
``game_launches.song_id`` stays ``ON DELETE SET NULL`` so the activity history
remains readable after a (result-free) physical delete.

No data is rewritten: existing songs get ``archived_at = NULL``, i.e. they stay
exactly as available as they were before.
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0012_song_archive"
down_revision = "0011_asset_versions"
branch_labels = None
depends_on = None

ARCHIVE_INDEX = "ix_songs_archived_at"

#: Tables whose ``song_id`` must never cascade a song delete into user data.
PROTECTED_FOREIGN_KEYS: tuple[tuple[str, str], ...] = (
    ("song_best_scores", "song_id"),
    ("daily_slots", "song_id"),
)


def _foreign_key_name(bind: sa.engine.Connection, table: str, column: str) -> str | None:
    """Actual name of the FK on ``table.column`` — never assume the PG default."""
    return bind.execute(
        sa.text(
            """
            SELECT c.conname
            FROM pg_constraint c
            JOIN pg_class t ON t.oid = c.conrelid
            JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = ANY (c.conkey)
            WHERE c.contype = 'f'
              AND t.relname = :table
              AND a.attname = :column
            ORDER BY c.conname
            LIMIT 1
            """
        ),
        {"table": table, "column": column},
    ).scalar()


def _recreate_song_fk(table: str, column: str, ondelete: str) -> None:
    bind = op.get_bind()
    name = _foreign_key_name(bind, table, column)
    if name is not None:
        op.drop_constraint(name, table, type_="foreignkey")
    op.create_foreign_key(
        f"{table}_{column}_fkey",
        table,
        "songs",
        [column],
        ["id"],
        ondelete=ondelete,
    )


def upgrade() -> None:
    op.add_column(
        "songs", sa.Column("archived_at", sa.DateTime(timezone=True), nullable=True)
    )
    op.create_index(ARCHIVE_INDEX, "songs", ["archived_at"])

    for table, column in PROTECTED_FOREIGN_KEYS:
        _recreate_song_fk(table, column, "RESTRICT")


def downgrade() -> None:
    # The data is untouched: dropping ``archived_at`` only loses the archive
    # flag (archived songs become ordinary rows again), and the old cascades are
    # restored for schema parity. Re-running ``upgrade`` is safe.
    for table, column in PROTECTED_FOREIGN_KEYS:
        _recreate_song_fk(table, column, "CASCADE")

    op.drop_index(ARCHIVE_INDEX, table_name="songs")
    op.drop_column("songs", "archived_at")
