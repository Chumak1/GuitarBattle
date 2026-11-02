import hashlib
import os
import uuid
from collections.abc import Sequence
from pathlib import Path
from app.models.song_midi import SongMidiAnalysis, SongMidiNote, SongMidiTempoChange
from app.schemas.midi_analysis import MidiAnalysisResponse, MidiNoteResponse, MidiTempoChangeResponse
from app.services.midi_parser import MidiParseError, parse_midi
from uuid import UUID

from fastapi import APIRouter, Depends, File, HTTPException, Query, Response, UploadFile, status
from fastapi.responses import FileResponse
from sqlalchemy import func, or_, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session
from app.api.v1.auth import get_current_user
from app.config import settings
from app.database import get_db
from app.models.song import Song, SongFile
from app.models.user import User
from app.schemas.admin_songs import (
    AdminSongAssetsResponse,
    AdminSongCreate,
    AdminSongDetailResponse,
    AdminSongFileResponse,
    AdminSongListResponse,
    AdminSongResponse,
    AdminSongStemsResponse,
    AdminSongUpdate,
)
from app.services.daily_slots import apply_song_weekday
from app.services.mp3_upload import has_mp3_extension, looks_like_mp3
from app.services.song_archive import SongUsage, archive_song, restore_song, song_usage
from app.services.song_assets import (
    PREVIEW_ROLE,
    build_readiness,
    deactivate_role_files,
    load_active_files_by_song,
)

router = APIRouter(prefix="/admin/songs", tags=["admin: songs"])

def require_admin(current_user: User = Depends(get_current_user)) -> User:
    if not current_user.is_admin:
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="Administrator access required")
    return current_user


def _admin_file_response(song_id: UUID, stored_file: SongFile) -> AdminSongFileResponse:
    result = AdminSongFileResponse.model_validate(stored_file)
    result.download_url = (
        f"{settings.api_v1_prefix}/admin/songs/{song_id}/files/{stored_file.id}/download"
    )
    return result


def _admin_song_response(song: Song, files_by_role: dict[str, SongFile]) -> AdminSongResponse:
    result = AdminSongResponse.model_validate(song)
    result.readiness = build_readiness(files_by_role)
    preview = files_by_role.get(PREVIEW_ROLE)
    # Additive field for the web admin player. The URL is the admin-only
    # download route, so it never exposes the preview outside ``/admin``.
    result.preview_url = (
        _admin_file_response(song.id, preview).download_url
        if preview is not None
        else None
    )
    return result


def _usage_conflict_detail(usage: SongUsage) -> str:
    """409 explanation with counts, so support can see what blocks the delete."""
    counts = ", ".join(f"{name}={value}" for name, value in usage.as_detail().items())
    return (
        "Song cannot be deleted because player results reference it "
        f"({counts}); archive it instead"
    )


def _admin_song_detail(
    song: Song,
    files_by_role: dict[str, SongFile],
    all_files: Sequence[SongFile] | None = None,
) -> AdminSongDetailResponse:
    result = AdminSongDetailResponse.model_validate(song)
    result.readiness = build_readiness(files_by_role)

    def optional(role: str) -> AdminSongFileResponse | None:
        stored_file = files_by_role.get(role)
        return _admin_file_response(song.id, stored_file) if stored_file is not None else None

    result.assets = AdminSongAssetsResponse(
        midi=optional("midi"),
        stems=AdminSongStemsResponse(
            drums=optional("drums"),
            bass=optional("bass"),
            guitar=optional("guitar"),
            others=optional("others"),
        ),
        preview=optional(PREVIEW_ROLE),
    )
    # Mirrors ``assets.preview.download_url`` so the web admin can start the
    # player from either the list or the detail response.
    result.preview_url = result.assets.preview.download_url if result.assets.preview else None
    result.files = [
        _admin_file_response(song.id, stored_file) for stored_file in (all_files or [])
    ]
    return result

@router.get("", response_model=AdminSongListResponse, summary="List all songs, including drafts and archived")
def list_all_songs(
    search: str | None = Query(default=None, min_length=1, max_length=100),
    published: bool | None = Query(default=None, description="Filter by publication status"),
    archived: bool | None = Query(
        default=None,
        description="Filter by archive state: true = only archived, false = only active",
    ),
    limit: int = Query(default=20, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminSongListResponse:
    filters = []
    if search:
        term = f"%{search.strip()}%"
        filters.append(or_(Song.title.ilike(term), Song.slug.ilike(term), Song.artist.ilike(term)))
    if published is not None:
        filters.append(Song.is_published.is_(published))
    if archived is not None:
        filters.append(
            Song.archived_at.is_not(None) if archived else Song.archived_at.is_(None)
        )
    total = db.scalar(select(func.count()).select_from(Song).where(*filters)) or 0
    items = db.scalars(select(Song).where(*filters).order_by(Song.updated_at.desc(), Song.id.asc()).limit(limit).offset(offset)).all()
    files_by_song = load_active_files_by_song(db, (song.id for song in items))
    responses = [_admin_song_response(song, files_by_song.get(song.id, {})) for song in items]
    return AdminSongListResponse(items=responses, total=total, limit=limit, offset=offset)

@router.post("", response_model=AdminSongResponse, status_code=status.HTTP_201_CREATED, summary="Create a song draft")
def create_song(payload: AdminSongCreate, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> AdminSongResponse:
    song = Song(**payload.model_dump(), is_published=False, version=1)
    db.add(song)
    try:
        db.flush()
        # ``daily_weekday`` is kept in sync with ``daily_slots`` in the same
        # transaction, so the song is scheduled (or not) as soon as it exists.
        apply_song_weekday(db, song)
        db.commit()
        db.refresh(song)
    except IntegrityError:
        db.rollback()
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="Song slug already exists")
    return _admin_song_response(song, {})

@router.get("/{song_id}", response_model=AdminSongDetailResponse, summary="Get a song, including drafts")
def get_admin_song(song_id: UUID, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> AdminSongDetailResponse:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    files_by_role = load_active_files_by_song(db, [song.id]).get(song.id, {})
    all_files = db.scalars(
        select(SongFile)
        .where(SongFile.song_id == song.id)
        .order_by(SongFile.created_at.desc(), SongFile.id.asc())
    ).all()
    return _admin_song_detail(song, files_by_role, all_files)

@router.patch("/{song_id}", response_model=AdminSongResponse, summary="Update song metadata")
def update_song(song_id: UUID, payload: AdminSongUpdate, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> AdminSongResponse:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    changes = payload.model_dump(exclude_unset=True)
    if not changes:
        files_by_role = load_active_files_by_song(db, [song.id]).get(song.id, {})
        return _admin_song_response(song, files_by_role)
    non_nullable_fields = {"slug", "title", "genre", "bpm", "difficulty", "duration_seconds"}
    if any(key in non_nullable_fields and value is None for key, value in changes.items()):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="Required song fields cannot be null")
    if changes.get("daily_weekday") is not None and song.archived_at is not None:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Archived songs cannot be scheduled for the Daily Challenge",
        )
    for key, value in changes.items():
        setattr(song, key, value)
    song.version += 1
    try:
        if "daily_weekday" in changes:
            # Setting the weekday schedules the song (moving it out of its old
            # slot if needed); setting it to null removes it from every slot.
            apply_song_weekday(db, song)
        db.commit()
        db.refresh(song)
    except IntegrityError:
        db.rollback()
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="Song slug already exists")
    files_by_role = load_active_files_by_song(db, [song.id]).get(song.id, {})
    return _admin_song_response(song, files_by_role)

@router.post("/{song_id}/publish", response_model=AdminSongResponse, summary="Publish a song")
def publish_song(song_id: UUID, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> AdminSongResponse:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    if song.archived_at is not None:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Restore the song from the archive before publishing it",
        )
    song.is_published = True
    db.commit()
    db.refresh(song)
    files_by_role = load_active_files_by_song(db, [song.id]).get(song.id, {})
    return _admin_song_response(song, files_by_role)

@router.post("/{song_id}/unpublish", response_model=AdminSongResponse, summary="Unpublish a song")
def unpublish_song(song_id: UUID, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> AdminSongResponse:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    song.is_published = False
    db.commit()
    db.refresh(song)
    files_by_role = load_active_files_by_song(db, [song.id]).get(song.id, {})
    return _admin_song_response(song, files_by_role)

@router.delete(
    "/{song_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Archive a song (or delete it permanently when it was never played)",
)
def delete_song(
    song_id: UUID,
    permanent: bool = Query(
        default=False,
        description=(
            "Physically delete the row. Allowed only when the song has no user "
            "results; otherwise the request fails with 409 and nothing is removed."
        ),
    ),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> Response:
    """Never destroys player data.

    By default the song is *archived*: it keeps its ``id``, its results, its
    personal bests and its daily history, while disappearing from the public
    catalogue and from new game sessions. ``permanent=true`` is the explicit
    escape hatch for a song that was never played (a mistyped draft); it is
    refused as soon as any result references the song.
    """
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    if not permanent:
        archive_song(db, song)
        db.commit()
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    usage = song_usage(db, song.id)
    if usage.has_user_results:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=_usage_conflict_detail(usage),
        )

    # Scheduling is the only non-result reference removed with the song; the
    # RESTRICT foreign keys still refuse the delete if anything was missed.
    archive_song(db, song)
    db.delete(song)
    try:
        db.commit()
    except IntegrityError:
        db.rollback()
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=(
                "Song cannot be deleted because player results reference it; "
                "archive it instead"
            ),
        )
    return Response(status_code=status.HTTP_204_NO_CONTENT)


@router.post(
    "/{song_id}/archive",
    response_model=AdminSongResponse,
    summary="Hide a song from the catalogue, keeping every result",
)
def archive_song_endpoint(
    song_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminSongResponse:
    """Explicit archive route used by the web admin; ``DELETE`` does the same."""
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    archive_song(db, song)
    db.commit()
    db.refresh(song)
    files_by_role = load_active_files_by_song(db, [song.id]).get(song.id, {})
    return _admin_song_response(song, files_by_role)


@router.post(
    "/{song_id}/restore",
    response_model=AdminSongResponse,
    summary="Bring an archived song back as a draft",
)
def restore_song_endpoint(
    song_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminSongResponse:
    """Clears ``archived_at``. The song stays unpublished until it is published."""
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    restore_song(db, song)
    db.commit()
    db.refresh(song)
    files_by_role = load_active_files_by_song(db, [song.id]).get(song.id, {})
    return _admin_song_response(song, files_by_role)


@router.post(
    "/{song_id}/files/midi",
    response_model=AdminSongFileResponse,
    status_code=status.HTTP_201_CREATED,
    summary="Upload a MIDI file for a song",
)
async def upload_song_midi(
    song_id: UUID,
    file: UploadFile = File(..., description="Standard MIDI file (.mid or .midi)"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> SongFile:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    original_name = Path(file.filename or "").name
    if Path(original_name).suffix.lower() not in {".mid", ".midi"}:
        raise HTTPException(status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE, detail="Only .mid and .midi files are accepted")

    # Read at most the configured limit plus one byte to reject oversized uploads safely.
    data = await file.read(settings.max_midi_upload_bytes + 1)
    if len(data) > settings.max_midi_upload_bytes:
        raise HTTPException(status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE, detail="MIDI file exceeds the 20 MiB upload limit")
    if len(data) < 14 or data[:4] != b"MThd":
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="File does not have a valid Standard MIDI header")
    header_length = int.from_bytes(data[4:8], "big")
    if header_length < 6 or len(data) < 8 + header_length:
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="Invalid MIDI header length")

    file_id = uuid.uuid4()
    relative_key = f"{song_id}/{file_id}.mid"
    storage_root = settings.song_files_dir.resolve()
    target = (storage_root / relative_key).resolve()
    if not target.is_relative_to(storage_root):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="Invalid storage path")

    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as destination:
            destination.write(data)
    except OSError as exc:
        raise HTTPException(status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail="Could not store uploaded MIDI file") from exc

    record = SongFile(
        id=file_id,
        song_id=song_id,
        role="midi",
        object_key=relative_key,
        content_type="audio/midi",
        size_bytes=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
        is_active=True,
    )
    # The new upload wins, but it must be the only active MIDI version: the
    # previous one stays downloadable as an archive.
    deactivate_role_files(db, song_id, "midi", keep_file_id=file_id)
    db.add(record)
    # Song.version is the content/cache version clients use for offline assets.
    # Bump it in the same transaction as the new file record.
    song.version += 1
    try:
        db.commit()
        db.refresh(record)
    except Exception:
        db.rollback()
        try:
            target.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    finally:
        await file.close()
    return record


@router.post(
    "/{song_id}/files/audio/{role}",
    response_model=AdminSongFileResponse,
    status_code=status.HTTP_201_CREATED,
    summary="Upload an MP3 stem or the song preview",
)
async def upload_song_audio_stem(
    song_id: UUID,
    role: str,
    file: UploadFile = File(..., description="MP3 file (.mp3): a stem or the preview mix"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> SongFile:
    """Upload one MP3 asset of a song.

    ``role`` is one of the four gameplay stems or ``preview`` — the optional
    full mix used by non-gameplay song screens. Previews share the stem rules
    (MP3 only, same size limit, versioning: the new upload becomes the active
    file and the previous one stays archived), but they are excluded from
    readiness and publication requirements.
    """
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    role = role.strip().lower()
    if role not in {"drums", "bass", "guitar", "others", PREVIEW_ROLE}:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Audio role must be one of: drums, bass, guitar, others, preview",
        )

    original_name = Path(file.filename or "").name
    if not has_mp3_extension(original_name):
        raise HTTPException(
            status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE,
            detail="Only .mp3 audio files are accepted for stems and previews",
        )

    data = await file.read(settings.max_audio_upload_bytes + 1)
    if len(data) > settings.max_audio_upload_bytes:
        raise HTTPException(
            status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
            detail="MP3 file exceeds the configured 100 MiB upload limit",
        )
    if not looks_like_mp3(data):
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="File does not appear to contain MP3 audio",
        )

    file_id = uuid.uuid4()
    relative_key = f"{song_id}/{file_id}.mp3"
    storage_root = settings.song_files_dir.resolve()
    target = (storage_root / relative_key).resolve()
    if not target.is_relative_to(storage_root):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="Invalid storage path")

    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as destination:
            destination.write(data)
    except OSError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail="Could not store uploaded MP3 file",
        ) from exc

    record = SongFile(
        id=file_id,
        song_id=song_id,
        role=role,
        object_key=relative_key,
        content_type="audio/mpeg",
        size_bytes=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
        is_active=True,
    )
    deactivate_role_files(db, song_id, role, keep_file_id=file_id)
    db.add(record)
    # ``Song.version`` drives the client cache of *gameplay* assets (MIDI and
    # stems). A preview is not part of that download set, so bumping the version
    # would force every player to re-download unchanged stems. Stems and MIDI
    # still bump it in the same transaction as their file record.
    if role != PREVIEW_ROLE:
        song.version += 1
    try:
        db.commit()
        db.refresh(record)
    except Exception:
        db.rollback()
        try:
            target.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    finally:
        await file.close()
    return record


@router.delete(
    "/{song_id}/files/preview",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete the song preview",
)
def delete_song_preview(
    song_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> Response:
    """Remove every stored preview version of the song.

    Idempotent: a song without a preview (or without any song at all being
    requested twice) answers ``204`` and changes nothing. Only ``preview`` rows
    are removed; MIDI, stems, scores, XP, ratings and results are untouched.
    ``Song.version`` is left alone: it versions the gameplay asset set, and a
    preview is not part of it.
    """
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    previews = db.scalars(
        select(SongFile).where(
            SongFile.song_id == song_id,
            SongFile.role == PREVIEW_ROLE,
        )
    ).all()
    if not previews:
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    object_keys = [record.object_key for record in previews]
    for record in previews:
        db.delete(record)
    db.commit()

    # Files are removed after the transaction: a failed commit leaves the rows
    # and their files consistent. A missing file never blocks the endpoint.
    storage_root = settings.song_files_dir.resolve()
    for object_key in object_keys:
        target = (storage_root / object_key).resolve()
        if not target.is_relative_to(storage_root):
            continue
        try:
            target.unlink(missing_ok=True)
        except OSError:
            pass
    return Response(status_code=status.HTTP_204_NO_CONTENT)


@router.get(
    "/{song_id}/files",
    response_model=list[AdminSongFileResponse],
    summary="List files attached to a song",
)
def list_song_files(
    song_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> list[SongFile]:
    if db.get(Song, song_id) is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    return db.scalars(
        select(SongFile)
        .where(SongFile.song_id == song_id)
        .order_by(SongFile.created_at.desc(), SongFile.id.asc())
    ).all()


@router.post(
    "/{song_id}/files/{file_id}/activate",
    response_model=AdminSongFileResponse,
    summary="Make a stored file the published version of its role",
)
def activate_song_file(
    song_id: UUID,
    file_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminSongFileResponse:
    """Switch the active version of ``(song, role)`` to ``file_id``.

    Every other file of the same role is archived in the same transaction, so
    the public API always serves exactly one version per role. ``Song.version``
    is bumped because clients cache downloadable assets by that value.
    """
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    record = db.scalar(
        select(SongFile).where(SongFile.id == file_id, SongFile.song_id == song_id)
    )
    if record is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song file not found")

    deactivate_role_files(db, song_id, record.role, keep_file_id=record.id)
    record.is_active = True
    # Only gameplay assets are covered by the client's version cache; switching
    # a preview version must not force a stem re-download.
    if record.role != PREVIEW_ROLE:
        song.version += 1
    db.commit()
    db.refresh(record)
    return _admin_file_response(song_id, record)


@router.get(
    "/{song_id}/files/{file_id}/download",
    summary="Download a song file",
)
def download_song_file(
    song_id: UUID,
    file_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> FileResponse:
    record = db.scalar(
        select(SongFile).where(SongFile.id == file_id, SongFile.song_id == song_id)
    )
    if record is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song file not found")
    storage_root = settings.song_files_dir.resolve()
    target = (storage_root / record.object_key).resolve()
    if not target.is_relative_to(storage_root) or not target.is_file():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Stored file not found")
    return FileResponse(
        path=target,
        media_type=record.content_type,
        filename=target.name,
    )


@router.get(
    "/{song_id}/files/{file_id}/analysis",
    response_model=MidiAnalysisResponse,
    summary="Read saved MIDI analysis without parsing the file again",
)
def get_saved_song_midi_analysis(
    song_id: UUID,
    file_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> MidiAnalysisResponse:
    record = db.scalar(
        select(SongFile).where(
            SongFile.id == file_id,
            SongFile.song_id == song_id,
            SongFile.role == "midi",
        )
    )
    if record is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="MIDI file not found")

    analysis = db.scalar(
        select(SongMidiAnalysis).where(SongMidiAnalysis.song_file_id == file_id)
    )
    if analysis is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="MIDI has not been analyzed yet")

    max_returned_notes = 2000
    response_notes = db.scalars(
        select(SongMidiNote)
        .where(SongMidiNote.song_file_id == file_id)
        .order_by(SongMidiNote.start_tick, SongMidiNote.track_index, SongMidiNote.id)
        .limit(max_returned_notes)
    ).all()
    tempo_changes = db.scalars(
        select(SongMidiTempoChange)
        .where(SongMidiTempoChange.song_file_id == file_id)
        .order_by(SongMidiTempoChange.tick)
    ).all()
    return MidiAnalysisResponse(
        song_file_id=file_id,
        midi_format=analysis.midi_format,
        track_count=analysis.track_count,
        ticks_per_beat=analysis.ticks_per_beat,
        bpm=analysis.bpm,
        duration_seconds=analysis.duration_seconds,
        note_count=analysis.note_count,
        notes_returned=len(response_notes),
        notes_truncated=analysis.note_count > len(response_notes),
        tempo_changes=[MidiTempoChangeResponse(tick=t.tick, bpm=t.bpm) for t in tempo_changes],
        notes=[MidiNoteResponse.model_validate(note) for note in response_notes],
        parsed_at=analysis.parsed_at,
    )


@router.post(
    "/{song_id}/files/{file_id}/analyze",
    response_model=MidiAnalysisResponse,
    summary="Parse a stored MIDI file and save its notes",
)
def analyze_song_midi(
    song_id: UUID,
    file_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> MidiAnalysisResponse:
    record = db.scalar(
        select(SongFile).where(
            SongFile.id == file_id,
            SongFile.song_id == song_id,
            SongFile.role == "midi",
        )
    )
    if record is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="MIDI file not found")

    storage_root = settings.song_files_dir.resolve()
    target = (storage_root / record.object_key).resolve()
    if not target.is_relative_to(storage_root) or not target.is_file():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Stored MIDI file not found")

    try:
        parsed = parse_midi(target.read_bytes())
    except MidiParseError as exc:
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail=str(exc)) from exc
    except OSError as exc:
        raise HTTPException(status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail="Could not read stored MIDI file") from exc

    analysis = db.scalar(
        select(SongMidiAnalysis).where(SongMidiAnalysis.song_file_id == file_id)
    )
    if analysis is None:
        analysis = SongMidiAnalysis(song_file_id=file_id)
        db.add(analysis)
    analysis.midi_format = parsed["midi_format"]
    analysis.track_count = parsed["track_count"]
    analysis.ticks_per_beat = parsed["ticks_per_beat"]
    analysis.bpm = parsed["bpm"]
    analysis.duration_seconds = parsed["duration_seconds"]
    analysis.note_count = parsed["note_count"]

    # Re-running analysis replaces the previous saved notes and tempo map.
    db.query(SongMidiNote).filter(SongMidiNote.song_file_id == file_id).delete(synchronize_session=False)
    db.query(SongMidiTempoChange).filter(SongMidiTempoChange.song_file_id == file_id).delete(synchronize_session=False)
    note_rows = [SongMidiNote(song_file_id=file_id, **note) for note in parsed["notes"]]
    tempo_rows = [
        SongMidiTempoChange(song_file_id=file_id, **tempo)
        for tempo in parsed["tempo_changes"]
    ]
    db.add_all(note_rows)
    db.add_all(tempo_rows)
    db.commit()
    db.refresh(analysis)

    max_returned_notes = 2000
    response_notes = db.scalars(
        select(SongMidiNote)
        .where(SongMidiNote.song_file_id == file_id)
        .order_by(SongMidiNote.start_tick, SongMidiNote.track_index, SongMidiNote.id)
        .limit(max_returned_notes)
    ).all()
    return MidiAnalysisResponse(
        song_file_id=file_id,
        midi_format=analysis.midi_format,
        track_count=analysis.track_count,
        ticks_per_beat=analysis.ticks_per_beat,
        bpm=analysis.bpm,
        duration_seconds=analysis.duration_seconds,
        note_count=analysis.note_count,
        notes_returned=len(response_notes),
        notes_truncated=analysis.note_count > len(response_notes),
        tempo_changes=[
            MidiTempoChangeResponse(tick=tempo.tick, bpm=tempo.bpm)
            for tempo in db.scalars(
                select(SongMidiTempoChange)
                .where(SongMidiTempoChange.song_file_id == file_id)
                .order_by(SongMidiTempoChange.tick)
            ).all()
        ],
        notes=[MidiNoteResponse.model_validate(note) for note in response_notes],
        parsed_at=analysis.parsed_at,
    )
