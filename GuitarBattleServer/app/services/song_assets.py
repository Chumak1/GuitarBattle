"""Shared song-asset helpers.

The public and admin APIs must agree on which files make a song playable and
which files make it fully publishable.  Keep the role lists and the readiness
calculation in one place so a new role cannot be added to only one endpoint.

Only files with ``is_active = TRUE`` are published.  ``song_files`` keeps every
upload as a version history; exactly one file per ``(song_id, role)`` is active
at a time and the admin panel can switch the active version.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from uuid import UUID

from sqlalchemy import select, update
from sqlalchemy.orm import Session

from app.models.song import SongFile
from app.schemas.songs import SongReadinessResponse

# Order is part of the API: clients and admins see roles in this order.
ASSET_ROLES = ("midi", "drums", "bass", "guitar", "others")
AUDIO_STEM_ROLES = ("drums", "bass", "guitar", "others")


def load_active_files_by_song(
    db: Session,
    song_ids: Iterable[UUID],
) -> dict[UUID, dict[str, SongFile]]:
    """Load the active file for every role of the given songs in one query."""
    ids = list(dict.fromkeys(song_ids))
    if not ids:
        return {}

    rows = db.scalars(
        select(SongFile)
        .where(SongFile.song_id.in_(ids), SongFile.is_active.is_(True))
        .order_by(
            SongFile.song_id.asc(),
            SongFile.role.asc(),
            SongFile.created_at.desc(),
            SongFile.id.desc(),
        )
    ).all()

    grouped: dict[UUID, dict[str, SongFile]] = {song_id: {} for song_id in ids}
    for stored_file in rows:
        grouped[stored_file.song_id].setdefault(stored_file.role, stored_file)
    return grouped


def deactivate_role_files(
    db: Session,
    song_id: UUID,
    role: str,
    *,
    keep_file_id: UUID | None = None,
) -> None:
    """Archive the other versions of ``(song_id, role)`` in this transaction.

    ``keep_file_id`` is the version that must stay active (the newly uploaded
    file or the one selected in the admin panel).  Passing ``None`` archives
    every current version of the role, which is what an upload does before it
    inserts its own record.
    """
    statement = update(SongFile).where(
        SongFile.song_id == song_id,
        SongFile.role == role,
        SongFile.is_active.is_(True),
    )
    if keep_file_id is not None:
        statement = statement.where(SongFile.id != keep_file_id)
    db.execute(statement.values(is_active=False))


def build_readiness(files_by_role: Mapping[str, SongFile]) -> SongReadinessResponse:
    """Describe what a song can do with the assets currently attached to it.

    * ``is_playable`` — the client has a MIDI file and can start a session.
    * ``is_complete`` / ``publish_ready`` — MIDI and all four audio stems exist.
    * MIDI-only songs stay playable and are not treated as broken.
    """
    available_roles = [role for role in ASSET_ROLES if role in files_by_role]
    missing_roles = [role for role in ASSET_ROLES if role not in files_by_role]
    has_midi = "midi" in files_by_role
    has_all_stems = all(role in files_by_role for role in AUDIO_STEM_ROLES)
    is_complete = has_midi and has_all_stems

    has_any_stem = any(role in files_by_role for role in AUDIO_STEM_ROLES)
    if not available_roles:
        state = "empty"
    elif is_complete:
        state = "complete"
    elif has_midi and not has_any_stem:
        state = "midi_only"
    else:
        state = "partial"

    return SongReadinessResponse(
        state=state,
        available_roles=available_roles,
        missing_roles=missing_roles,
        has_midi=has_midi,
        has_all_stems=has_all_stems,
        is_playable=has_midi,
        is_complete=is_complete,
        publish_ready=is_complete,
    )
