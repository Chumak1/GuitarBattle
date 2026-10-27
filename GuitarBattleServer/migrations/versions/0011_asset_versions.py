"""Asset versions: ``song_files.is_active`` + the 27.10 19:26-19:31 rollback.

The public API used to serve the newest stored file per role.  That made a bad
re-upload immediately visible to every player even though the previous files
were still stored.  ``is_active`` marks exactly one stored file per
``(song_id, role)`` as the published version; everything else stays on disk as
an archive and can be restored from the admin panel.

The same migration repairs the damage: on 2026-10-27 between 19:26 and 19:31
two songs ("Придумай светлый мир" and "Metal song") received wrong / truncated
stem sets and MIDI files.  All uploads from 19:00 UTC that day are archived and
the earliest stored file of every role becomes the active version again.  For
the two affected songs this restores the 21.10 and 22.10 asset sets.

``songs.version`` is bumped for every song whose published asset set changes,
so clients invalidate their cached downloads (``version.txt``).
"""
from __future__ import annotations

from datetime import datetime, timezone

import sqlalchemy as sa
from alembic import op

revision = "0011_asset_versions"
down_revision = "0010_achievement_icons"
branch_labels = None
depends_on = None

INDEX_NAME = "ix_song_files_song_id_role_is_active"

#: Uploads at or after this instant are the known-bad 27.10 batch.
BAD_UPLOAD_CUTOFF = datetime(2026, 10, 27, 19, 0, tzinfo=timezone.utc)


def upgrade() -> None:
    op.add_column(
        "song_files",
        sa.Column(
            "is_active",
            sa.Boolean(),
            nullable=False,
            server_default=sa.true(),
        ),
    )
    op.create_index(INDEX_NAME, "song_files", ["song_id", "role", "is_active"])
    rollback_asset_versions(op.get_bind())


def rollback_asset_versions(bind: sa.engine.Connection) -> None:
    """Archive the 27.10 uploads and restore the earliest file of each role.

    Kept as a separate function so the data repair can be exercised by the
    test-suite against seeded rows (the migration itself runs only once).
    """
    # 1. Every file uploaded during the bad batch becomes an archived version.
    bind.execute(
        sa.text(
            "UPDATE song_files SET is_active = FALSE "
            "WHERE created_at >= :cutoff"
        ),
        {"cutoff": BAD_UPLOAD_CUTOFF},
    )

    # 2. Exactly one file per (song, role) stays active: the earliest upload
    #    that predates the bad batch.  Rows sharing a timestamp fall back to the
    #    lowest id so the result is deterministic.
    bind.execute(
        sa.text(
            """
            WITH ranked AS (
                SELECT id,
                       row_number() OVER (
                           PARTITION BY song_id, role
                           ORDER BY created_at ASC, id ASC
                       ) AS position
                FROM song_files
                WHERE is_active
            )
            UPDATE song_files AS target
            SET is_active = FALSE
            FROM ranked
            WHERE target.id = ranked.id
              AND ranked.position > 1
            """
        )
    )

    # 3. Clients cache song assets by ``songs.version``; every song whose
    #    published file selection changed must be re-downloaded.
    bind.execute(
        sa.text(
            """
            WITH active AS (
                SELECT DISTINCT ON (song_id, role) song_id, role, id
                FROM song_files
                WHERE is_active
                ORDER BY song_id, role, created_at ASC, id ASC
            ), newest AS (
                SELECT DISTINCT ON (song_id, role) song_id, role, id
                FROM song_files
                ORDER BY song_id, role, created_at DESC, id DESC
            )
            UPDATE songs
            SET version = version + 1
            WHERE id IN (
                SELECT song_id FROM song_files WHERE created_at >= :cutoff
                UNION
                SELECT active.song_id
                FROM active
                JOIN newest USING (song_id, role)
                WHERE active.id <> newest.id
            )
            """
        ),
        {"cutoff": BAD_UPLOAD_CUTOFF},
    )


def downgrade() -> None:
    op.drop_index(INDEX_NAME, table_name="song_files")
    op.drop_column("song_files", "is_active")
