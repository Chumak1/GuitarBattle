from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query, status
from fastapi.responses import FileResponse
from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app.config import settings
from app.database import get_db
from app.models.song import Song, SongFile
from app.schemas.songs import (
    SongAssetsResponse,
    SongDetailResponse,
    SongFileResponse,
    SongListResponse,
    SongStemsResponse,
    SongSummaryResponse,
)
from app.services.song_assets import (
    ASSET_ROLES,
    build_readiness,
    load_newest_files_by_song,
    newest_files_by_role,
)

router = APIRouter(prefix="/songs", tags=["songs"])


def _public_file_response(song: Song, stored_file: SongFile) -> SongFileResponse:
    return SongFileResponse(
        id=stored_file.id,
        role=stored_file.role,
        content_type=stored_file.content_type,
        size_bytes=stored_file.size_bytes,
        sha256=stored_file.sha256,
        download_url=(
            f"{settings.api_v1_prefix}/songs/{song.slug}/files/{stored_file.id}/download"
            if stored_file.role in ASSET_ROLES
            else None
        ),
    )


def _public_assets(song: Song, files_by_role: dict[str, SongFile]) -> SongAssetsResponse:
    def optional(role: str) -> SongFileResponse | None:
        stored_file = files_by_role.get(role)
        return _public_file_response(song, stored_file) if stored_file is not None else None

    return SongAssetsResponse(
        midi=optional("midi"),
        stems=SongStemsResponse(
            drums=optional("drums"),
            bass=optional("bass"),
            guitar=optional("guitar"),
            others=optional("others"),
        ),
    )


def _public_summary(song: Song, files_by_role: dict[str, SongFile]) -> SongSummaryResponse:
    result = SongSummaryResponse.model_validate(song)
    result.readiness = build_readiness(files_by_role)
    return result


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
    files_by_song = load_newest_files_by_song(db, (song.id for song in songs))
    items = [_public_summary(song, files_by_song.get(song.id, {})) for song in songs]
    return SongListResponse(items=items, total=total, limit=limit, offset=offset)


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
    files_by_role = newest_files_by_role(all_files)

    result = SongDetailResponse.model_validate(song)
    result.readiness = build_readiness(files_by_role)
    result.files = [
        _public_file_response(song, files_by_role[role])
        for role in ASSET_ROLES
        if role in files_by_role
    ]
    result.assets = _public_assets(song, files_by_role)
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
