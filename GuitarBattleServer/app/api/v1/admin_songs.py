import hashlib
import os
import uuid
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
from app.schemas.admin_songs import AdminSongCreate, AdminSongFileResponse, AdminSongListResponse, AdminSongResponse, AdminSongUpdate

router = APIRouter(prefix="/admin/songs", tags=["admin: songs"])

def require_admin(current_user: User = Depends(get_current_user)) -> User:
    if not current_user.is_admin:
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="Administrator access required")
    return current_user

@router.get("", response_model=AdminSongListResponse, summary="List all songs, including drafts")
def list_all_songs(
    search: str | None = Query(default=None, min_length=1, max_length=100),
    published: bool | None = Query(default=None, description="Filter by publication status"),
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
    total = db.scalar(select(func.count()).select_from(Song).where(*filters)) or 0
    items = db.scalars(select(Song).where(*filters).order_by(Song.updated_at.desc(), Song.id.asc()).limit(limit).offset(offset)).all()
    return AdminSongListResponse(items=items, total=total, limit=limit, offset=offset)

@router.post("", response_model=AdminSongResponse, status_code=status.HTTP_201_CREATED, summary="Create a song draft")
def create_song(payload: AdminSongCreate, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> Song:
    song = Song(**payload.model_dump(), is_published=False, version=1)
    db.add(song)
    try:
        db.commit()
        db.refresh(song)
    except IntegrityError:
        db.rollback()
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="Song slug already exists")
    return song

@router.get("/{song_id}", response_model=AdminSongResponse, summary="Get a song, including drafts")
def get_admin_song(song_id: UUID, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> Song:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    return song

@router.patch("/{song_id}", response_model=AdminSongResponse, summary="Update song metadata")
def update_song(song_id: UUID, payload: AdminSongUpdate, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> Song:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    changes = payload.model_dump(exclude_unset=True)
    if not changes:
        return song
    non_nullable_fields = {"slug", "title", "genre", "bpm", "difficulty", "duration_seconds"}
    if any(key in non_nullable_fields and value is None for key, value in changes.items()):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="Required song fields cannot be null")
    for key, value in changes.items():
        setattr(song, key, value)
    song.version += 1
    try:
        db.commit()
        db.refresh(song)
    except IntegrityError:
        db.rollback()
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="Song slug already exists")
    return song

@router.post("/{song_id}/publish", response_model=AdminSongResponse, summary="Publish a song")
def publish_song(song_id: UUID, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> Song:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    song.is_published = True
    db.commit()
    db.refresh(song)
    return song

@router.post("/{song_id}/unpublish", response_model=AdminSongResponse, summary="Unpublish a song")
def unpublish_song(song_id: UUID, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> Song:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    song.is_published = False
    db.commit()
    db.refresh(song)
    return song

@router.delete("/{song_id}", status_code=status.HTTP_204_NO_CONTENT, summary="Delete a song")
def delete_song(song_id: UUID, db: Session = Depends(get_db), _: User = Depends(require_admin)) -> Response:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")
    db.delete(song)
    try:
        db.commit()
    except IntegrityError:
        db.rollback()
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Song cannot be deleted because it is referenced by existing game sessions",
        )
    return Response(status_code=status.HTTP_204_NO_CONTENT)


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
    )
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
    summary="Upload an MP3 instrumental stem for a song",
)
async def upload_song_audio_stem(
    song_id: UUID,
    role: str,
    file: UploadFile = File(..., description="MP3 instrumental stem (.mp3)"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> SongFile:
    song = db.get(Song, song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    role = role.strip().lower()
    if role not in {"drums", "bass", "guitar", "others"}:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Audio role must be one of: drums, bass, guitar, others",
        )

    original_name = Path(file.filename or "").name
    if Path(original_name).suffix.lower() != ".mp3":
        raise HTTPException(
            status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE,
            detail="Only .mp3 audio files are accepted for instrumental stems",
        )

    data = await file.read(settings.max_audio_upload_bytes + 1)
    if len(data) > settings.max_audio_upload_bytes:
        raise HTTPException(
            status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
            detail="MP3 file exceeds the configured 100 MiB upload limit",
        )
    # Accept common MP3 forms: an ID3 tag or an MPEG audio frame sync.
    has_id3 = data.startswith(b"ID3")
    has_frame_sync = len(data) >= 2 and data[0] == 0xFF and (data[1] & 0xE0) == 0xE0
    if len(data) < 4 or not (has_id3 or has_frame_sync):
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
    )
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
        select(SongFile).where(SongFile.song_id == song_id).order_by(SongFile.created_at.desc())
    ).all()


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
