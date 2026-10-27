"""Global application audio: the background music track.

Background music is deliberately *not* a song asset. It is one application-wide
MP3 for non-gameplay screens, stored in ``settings.app_audio_dir`` and described
by a single ``app_audio_assets`` row per role.

Public routes (no authentication):

* ``GET /api/v1/app/background-music``      — descriptor, always 200
* ``GET /api/v1/app/background-music/file`` — the current MP3 stream

Admin routes (``require_admin``):

* ``GET    /api/v1/admin/app/background-music`` — status for the admin panel
* ``POST   /api/v1/admin/app/background-music`` — upload/replace (multipart ``file``)
* ``DELETE /api/v1/admin/app/background-music`` — remove, idempotent

The public URL is a stable route that resolves the current row, so replacing the
track never leaves a dangling link for clients that cached the descriptor.

The upload is atomic from the client's point of view: the new file is written
under a fresh generated name, the row is committed, and only then is the
previous file unlinked. A failed commit keeps the old row and old file intact.
"""
from __future__ import annotations

import hashlib
import uuid
from datetime import datetime, timezone
from pathlib import Path

from fastapi import APIRouter, Depends, File, HTTPException, Request, Response, UploadFile, status
from fastapi.responses import FileResponse
from sqlalchemy import select
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.config import settings
from app.database import get_db
from app.models.app_audio import AppAudioAsset
from app.models.user import User
from app.schemas.app_audio import (
    AdminBackgroundMusicResponse,
    BackgroundMusicResponse,
)
from app.services.mp3_upload import MP3_CONTENT_TYPE, has_mp3_extension, looks_like_mp3

BACKGROUND_MUSIC_ROLE = "background_music"

public_router = APIRouter(prefix="/app", tags=["app audio"])
admin_router = APIRouter(prefix="/admin/app", tags=["admin: app audio"])


def _get_asset(db: Session) -> AppAudioAsset | None:
    return db.scalar(
        select(AppAudioAsset).where(AppAudioAsset.role == BACKGROUND_MUSIC_ROLE)
    )


def _storage_root() -> Path:
    return settings.app_audio_dir.resolve()


def _safe_path(object_key: str) -> Path | None:
    """Resolve ``object_key`` inside the app audio directory, never outside it."""
    storage_root = _storage_root()
    target = (storage_root / object_key).resolve()
    if not target.is_relative_to(storage_root):
        return None
    return target


def _stored_file(asset: AppAudioAsset | None) -> Path | None:
    if asset is None:
        return None
    target = _safe_path(asset.object_key)
    if target is None or not target.is_file():
        return None
    return target


def _public_url(request: Request) -> str:
    # ``request.url_for`` honours the proxy headers, so the client sees the
    # public origin (https://fretfight.online) rather than the container host.
    return str(request.url_for("download_background_music"))


def _public_response(
    request: Request, asset: AppAudioAsset | None
) -> BackgroundMusicResponse:
    if asset is None or _stored_file(asset) is None:
        return BackgroundMusicResponse()
    return BackgroundMusicResponse(
        url=_public_url(request),
        content_type=asset.content_type,
        size_bytes=asset.size_bytes,
    )


def _admin_response(
    request: Request, asset: AppAudioAsset | None
) -> AdminBackgroundMusicResponse:
    public = _public_response(request, asset)
    if asset is None:
        return AdminBackgroundMusicResponse()
    return AdminBackgroundMusicResponse(
        **public.model_dump(),
        uploaded_at=asset.uploaded_at,
        original_name=asset.original_name,
        sha256=asset.sha256,
    )


def _remove_stored_file(object_key: str | None) -> None:
    """Best-effort unlink; a stale file must never break the API response."""
    if not object_key:
        return
    target = _safe_path(object_key)
    if target is None:
        return
    try:
        target.unlink(missing_ok=True)
    except OSError:
        pass


@public_router.get(
    "/background-music",
    response_model=BackgroundMusicResponse,
    summary="Current background music descriptor",
)
def get_background_music(
    request: Request, db: Session = Depends(get_db)
) -> BackgroundMusicResponse:
    """Always 200. Without an uploaded track every field is empty/zero."""
    return _public_response(request, _get_asset(db))


@public_router.get(
    "/background-music/file",
    summary="Stream the current background music",
)
def download_background_music(db: Session = Depends(get_db)) -> FileResponse:
    asset = _get_asset(db)
    target = _stored_file(asset)
    if asset is None or target is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail="Background music is not uploaded",
        )
    # The URL is stable across replacements, so the client must revalidate
    # instead of reusing a cached copy of the previous track.
    return FileResponse(
        path=target,
        media_type=asset.content_type,
        headers={"Cache-Control": "no-cache, must-revalidate"},
    )


@admin_router.get(
    "/background-music",
    response_model=AdminBackgroundMusicResponse,
    summary="Background music status for the admin panel",
)
def get_admin_background_music(
    request: Request, db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> AdminBackgroundMusicResponse:
    return _admin_response(request, _get_asset(db))


@admin_router.post(
    "/background-music",
    response_model=AdminBackgroundMusicResponse,
    status_code=status.HTTP_201_CREATED,
    summary="Upload or replace the background music",
)
async def upload_background_music(
    request: Request,
    response: Response,
    file: UploadFile = File(..., description="Background music MP3 (.mp3)"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminBackgroundMusicResponse:
    if not has_mp3_extension(file.filename):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Background music must be an .mp3 file",
        )

    # Never trust the declared content type; enforce the same limit as stems.
    data = await file.read(settings.max_audio_upload_bytes + 1)
    if len(data) > settings.max_audio_upload_bytes:
        raise HTTPException(
            status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
            detail="MP3 file exceeds the configured 100 MiB upload limit",
        )
    if not looks_like_mp3(data):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="File does not appear to contain MP3 audio",
        )

    storage_root = _storage_root()
    stored_name = f"background-music-{uuid.uuid4().hex}.mp3"
    target = _safe_path(stored_name)
    if target is None:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST, detail="Invalid storage path"
        )

    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as destination:
            destination.write(data)
    except OSError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail="Could not store the background music file",
        ) from exc

    asset = _get_asset(db)
    created = asset is None
    previous_key: str | None = None
    if asset is None:
        asset = AppAudioAsset(
            role=BACKGROUND_MUSIC_ROLE,
            object_key=stored_name,
            original_name=Path(file.filename or "").name or None,
            content_type=MP3_CONTENT_TYPE,
            size_bytes=len(data),
            sha256=hashlib.sha256(data).hexdigest(),
            uploaded_at=datetime.now(timezone.utc),
        )
        db.add(asset)
    else:
        previous_key = asset.object_key
        asset.object_key = stored_name
        asset.original_name = Path(file.filename or "").name or None
        asset.content_type = MP3_CONTENT_TYPE
        asset.size_bytes = len(data)
        asset.sha256 = hashlib.sha256(data).hexdigest()
        asset.uploaded_at = datetime.now(timezone.utc)

    try:
        db.commit()
    except Exception:
        db.rollback()
        _remove_stored_file(stored_name)
        raise
    finally:
        await file.close()

    # The database now points to the new file; removing the old one cannot
    # create a broken link. A replace answers 200, a first upload 201.
    if previous_key and previous_key != stored_name:
        _remove_stored_file(previous_key)
    if not created:
        response.status_code = status.HTTP_200_OK
    return _admin_response(request, asset)


@admin_router.delete(
    "/background-music",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Remove the background music",
)
def delete_background_music(
    db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> Response:
    """Idempotent: removing an already absent track still answers ``204``."""
    asset = _get_asset(db)
    if asset is None:
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    object_key = asset.object_key
    db.delete(asset)
    db.commit()
    _remove_stored_file(object_key)
    return Response(status_code=status.HTTP_204_NO_CONTENT)
