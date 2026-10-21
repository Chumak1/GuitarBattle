from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query, status
from fastapi.responses import FileResponse
from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app.config import settings
from app.database import get_db
from app.models.song import Song, SongFile
from app.schemas.songs import SongDetailResponse, SongFileResponse, SongListResponse, SongSummaryResponse

router = APIRouter(prefix="/songs", tags=["songs"])


@router.get("", response_model=SongListResponse, summary="List published songs")
def list_songs(
    search: str | None = Query(default=None, min_length=1, max_length=100, description="Search title, artist, or slug"),
    genre: str | None = Query(default=None, min_length=1, max_length=80),
    difficulty: int | None = Query(default=None, ge=1, le=10),
    limit: int = Query(default=20, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
) -> SongListResponse:
    """Return only published songs; drafts remain private to the admin workflow."""
    filters = [Song.is_published.is_(True)]
    if search:
        term = f"%{search.strip()}%"
        filters.append((Song.title.ilike(term)) | (Song.slug.ilike(term)) | (Song.artist.ilike(term)))
    if genre:
        filters.append(func.lower(Song.genre) == genre.strip().lower())
    if difficulty is not None:
        filters.append(Song.difficulty == difficulty)

    total = db.scalar(select(func.count()).select_from(Song).where(*filters)) or 0
    songs = db.scalars(
        select(Song)
        .where(*filters)
        .order_by(Song.title.asc(), Song.id.asc())
        .limit(limit)
        .offset(offset)
    ).all()
    return SongListResponse(items=songs, total=total, limit=limit, offset=offset)


@router.get("/{slug}", response_model=SongDetailResponse, summary="Get a published song")
def get_song(slug: str, db: Session = Depends(get_db)) -> SongDetailResponse:
    song = db.scalar(select(Song).where(Song.slug == slug, Song.is_published.is_(True)))
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    # Return only the newest version of each role. This also keeps old uploaded
    # replacements from being downloaded by clients after a song is updated.
    all_files = db.scalars(
        select(SongFile)
        .where(SongFile.song_id == song.id)
        .order_by(SongFile.role.asc(), SongFile.created_at.desc(), SongFile.id.desc())
    ).all()
    files = []
    seen_roles: set[str] = set()
    for stored_file in all_files:
        if stored_file.role not in seen_roles:
            files.append(stored_file)
            seen_roles.add(stored_file.role)

    result = SongDetailResponse.model_validate(song)
    downloadable_roles = {"midi", "drums", "bass", "guitar", "others"}
    result.files = [
        SongFileResponse(
            id=file.id,
            role=file.role,
            content_type=file.content_type,
            size_bytes=file.size_bytes,
            sha256=file.sha256,
            download_url=(
                f"{settings.api_v1_prefix}/songs/{song.slug}/files/{file.id}/download"
                if file.role in downloadable_roles
                else None
            ),
        )
        for file in files
    ]
    return result


@router.get(
    "/{slug}/files/{file_id}/download",
    summary="Download a file belonging to a published song",
)
def download_published_song_file(
    slug: str,
    file_id: UUID,
    db: Session = Depends(get_db),
) -> FileResponse:
    song = db.scalar(select(Song).where(Song.slug == slug, Song.is_published.is_(True)))
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    record = db.scalar(
        select(SongFile).where(
            SongFile.id == file_id,
            SongFile.song_id == song.id,
            SongFile.role.in_(["midi", "drums", "bass", "guitar", "others"]),
        )
    )
    if record is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song file not found")

    storage_root = settings.song_files_dir.resolve()
    target = (storage_root / record.object_key).resolve()
    if not target.is_relative_to(storage_root) or not target.is_file():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Stored MIDI file not found")

    return FileResponse(
        path=target,
        media_type=record.content_type,
        filename=f"{song.slug}{'.mid' if record.role == 'midi' else '.mp3'}",
    )
