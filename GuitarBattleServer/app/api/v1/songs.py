from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query, Request, status
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
    PREVIEW_ROLE,
    PUBLIC_DOWNLOAD_ROLES,
    build_readiness,
    load_active_files_by_song,
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
            if stored_file.role in PUBLIC_DOWNLOAD_ROLES
            else None
        ),
    )


def _preview_url(
    request: Request, song: Song, files_by_role: dict[str, SongFile]
) -> str | None:
    """Absolute URL of the active preview, or ``None`` when it is not uploaded.

    ``request.url_for`` honours uvicorn's ``--proxy-headers``, so behind Caddy
    the client receives the public ``https://fretfight.online/...`` address
    rather than the internal container host.
    """
    preview = files_by_role.get(PREVIEW_ROLE)
    if preview is None:
        return None
    return str(
        request.url_for(
            "download_published_song_file",
            slug=song.slug,
            file_id=str(preview.id),
        )
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


def _public_summary(
    song: Song, files_by_role: dict[str, SongFile], request: Request
) -> SongSummaryResponse:
    result = SongSummaryResponse.model_validate(song)
    result.preview_url = _preview_url(request, song, files_by_role)
    result.readiness = build_readiness(files_by_role)
    return result


@router.get("", response_model=SongListResponse, summary="List published songs")
def list_songs(
    request: Request,
    search: str | None = Query(default=None, min_length=1, max_length=100, description="Search title, artist, or slug"),
    genre: str | None = Query(default=None, min_length=1, max_length=80),
    difficulty: int | None = Query(default=None, ge=1, le=10),
    limit: int = Query(default=20, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
) -> SongListResponse:
    """Return only published songs; drafts remain private to the admin workflow.

    Songs holding a Daily Challenge slot (``daily_weekday IS NOT NULL``) are not
    part of the ordinary catalogue. They stay reachable by slug — see
    :func:`get_song` — because the client downloads their assets to play the
    official daily attempt. Archived songs (``archived_at IS NOT NULL``) are
    never served, but their results stay in the database.
    """
    filters = [
        Song.is_published.is_(True),
        Song.daily_weekday.is_(None),
        Song.archived_at.is_(None),
    ]
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
    files_by_song = load_active_files_by_song(db, (song.id for song in songs))
    items = [
        _public_summary(song, files_by_song.get(song.id, {}), request)
        for song in songs
    ]
    return SongListResponse(items=items, total=total, limit=limit, offset=offset)


@router.get("/{slug}", response_model=SongDetailResponse, summary="Get a published song")
def get_song(slug: str, request: Request, db: Session = Depends(get_db)) -> SongDetailResponse:
    # A daily-challenge song is hidden from the catalogue list but must stay
    # resolvable here: the client calls this endpoint (without a token) to get
    # the asset URLs of the song of the day. An archived song is not public at
    # all — its results remain, but its assets are no longer distributed.
    song = db.scalar(
        select(Song).where(
            Song.slug == slug,
            Song.is_published.is_(True),
            Song.archived_at.is_(None),
        )
    )
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    # Serve only the active version of each role. Archived uploads stay
    # downloadable by id but never replace the published asset set; an admin
    # can switch versions with ``POST /admin/songs/{id}/files/{file_id}/activate``.
    files_by_role = load_active_files_by_song(db, [song.id]).get(song.id, {})

    result = SongDetailResponse.model_validate(song)
    result.preview_url = _preview_url(request, song, files_by_role)
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
    song = db.scalar(
        select(Song).where(
            Song.slug == slug,
            Song.is_published.is_(True),
            Song.archived_at.is_(None),
        )
    )
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    record = db.scalar(
        select(SongFile).where(
            SongFile.id == file_id,
            SongFile.song_id == song.id,
            SongFile.role.in_(PUBLIC_DOWNLOAD_ROLES),
        )
    )
    if record is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song file not found")

    storage_root = settings.song_files_dir.resolve()
    target = (storage_root / record.object_key).resolve()
    if not target.is_relative_to(storage_root) or not target.is_file():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Stored song file not found")

    return FileResponse(
        path=target,
        media_type=record.content_type,
        filename=f"{song.slug}{'.mid' if record.role == 'midi' else '.mp3'}",
    )
