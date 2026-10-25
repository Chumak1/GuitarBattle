"""Administrator endpoints for the landing page CMS and game builds.

Every route requires an administrator JWT (same dependency as
``/admin/songs``). Uploads are validated by extension and file signature, and
are stored under a generated name so the original client filename never becomes
a path on disk.
"""
from __future__ import annotations

import json
import uuid
from pathlib import Path
from typing import Any

from fastapi import (
    APIRouter,
    Depends,
    File,
    Form,
    HTTPException,
    Response,
    UploadFile,
    status,
)
from sqlalchemy import select, update
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.api.v1.site import (
    LANDING_MEDIA_URL_PREFIX,
    LANDING_SETTINGS_ID,
    blocks_from_content,
    build_file_path,
    get_landing_row,
    hero_background_from_content,
    landing_content,
    serialize_build,
)
from app.config import settings
from app.database import get_db
from app.models.site_content import GameBuild, LandingSettings
from app.models.user import User
from app.schemas.site import (
    AdminLandingResponse,
    AdminLandingUpdate,
    GameBuildItem,
    GameBuildListResponse,
    HeroBackground,
    HeroBackgroundResponse,
)

router = APIRouter(prefix="/admin/site", tags=["admin: site"])

# Landing CMS limits. They keep one JSONB document small enough to be sent to
# the browser in full on every visit and stop pathological nesting.
MAX_BLOCKS_BYTES = 256 * 1024
MAX_BLOCK_STRING_LENGTH = 5000
MAX_BLOCKS_DEPTH = 8

CHUNK_SIZE = 1024 * 1024

# extension -> (hero type, content type, signature check)
IMAGE_SIGNATURES = {
    ".jpg": ("image", "image/jpeg", lambda head: head.startswith(b"\xff\xd8\xff")),
    ".jpeg": ("image", "image/jpeg", lambda head: head.startswith(b"\xff\xd8\xff")),
    ".png": ("image", "image/png", lambda head: head.startswith(b"\x89PNG\r\n\x1a\n")),
    ".gif": ("image", "image/gif", lambda head: head[:6] in (b"GIF87a", b"GIF89a")),
    ".webp": (
        "image",
        "image/webp",
        lambda head: head[:4] == b"RIFF" and head[8:12] == b"WEBP",
    ),
    ".mp4": ("video", "video/mp4", lambda head: head[4:8] == b"ftyp"),
    ".webm": ("video", "video/webm", lambda head: head.startswith(b"\x1a\x45\xdf\xa3")),
}

GAME_BUILD_TYPES = {
    ".exe": "application/octet-stream",
    ".zip": "application/zip",
    ".msi": "application/octet-stream",
}


def _validate_blocks(blocks: dict[str, Any]) -> None:
    """Reject oversized or non-serializable landing blocks before storing."""
    try:
        serialized = json.dumps(
            blocks, ensure_ascii=False, separators=(",", ":"), allow_nan=False
        )
    except (TypeError, ValueError) as exc:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="blocks must be a JSON-serializable object",
        ) from exc

    if len(serialized.encode("utf-8")) > MAX_BLOCKS_BYTES:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="blocks exceed the 256 KiB limit",
        )
    if _json_depth(blocks) > MAX_BLOCKS_DEPTH:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="blocks nest deeper than 8 levels",
        )
    _check_string_lengths(blocks)


def _json_depth(value: Any) -> int:
    if isinstance(value, dict):
        return 1 + max((_json_depth(item) for item in value.values()), default=0)
    if isinstance(value, list):
        return 1 + max((_json_depth(item) for item in value), default=0)
    return 0


def _check_string_lengths(value: Any) -> None:
    if isinstance(value, str):
        if len(value) > MAX_BLOCK_STRING_LENGTH:
            raise HTTPException(
                status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                detail="every string in blocks must be at most 5000 characters",
            )
    elif isinstance(value, dict):
        for item in value.values():
            _check_string_lengths(item)
    elif isinstance(value, list):
        for item in value:
            _check_string_lengths(item)


def _local_media_path(url: str | None) -> Path | None:
    """Map a ``/media/landing/<name>`` URL to a file inside the upload dir."""
    if not url or not url.startswith(LANDING_MEDIA_URL_PREFIX):
        return None
    name = url[len(LANDING_MEDIA_URL_PREFIX):]
    if not name or "/" in name or "\\" in name or name in {".", ".."}:
        return None
    storage_root = settings.landing_media_dir.resolve()
    target = (storage_root / name).resolve()
    if not target.is_relative_to(storage_root):
        return None
    return target


def _remove_media_file(path: Path | None) -> None:
    if path is None:
        return
    try:
        path.unlink(missing_ok=True)
    except OSError:
        # A stale file must not block the CMS update.
        pass


async def _write_upload(
    file: UploadFile,
    target: Path,
    limit: int,
    *,
    prefix: bytes = b"",
) -> int:
    """Stream an upload to disk, enforcing ``limit`` and cleaning up on error."""
    size = len(prefix)
    if size > limit:
        raise HTTPException(
            status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
            detail="Uploaded file exceeds the configured size limit",
        )
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        with target.open("xb") as destination:
            if prefix:
                destination.write(prefix)
            while True:
                chunk = await file.read(CHUNK_SIZE)
                if not chunk:
                    break
                size += len(chunk)
                if size > limit:
                    raise HTTPException(
                        status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
                        detail="Uploaded file exceeds the configured size limit",
                    )
                destination.write(chunk)
    except BaseException:
        _remove_media_file(target)
        raise
    if size == 0:
        _remove_media_file(target)
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Uploaded file is empty",
        )
    return size


def _safe_upload_path(directory: Path, stored_name: str) -> Path:
    storage_root = directory.resolve()
    target = (storage_root / stored_name).resolve()
    if not target.is_relative_to(storage_root):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST, detail="Invalid storage path"
        )
    return target


def _save_landing_background(
    db: Session, background: HeroBackground
) -> HeroBackground:
    row = get_landing_row(db)
    if row is None:
        row = LandingSettings(id=LANDING_SETTINGS_ID, content={})
        db.add(row)
    content = dict(landing_content(row))
    content["hero_background"] = background.model_dump()
    row.content = content
    db.commit()
    db.refresh(row)
    return HeroBackground.model_validate(content["hero_background"])


@router.get("/landing", response_model=AdminLandingResponse, summary="Read landing CMS content")
def get_admin_landing(db: Session = Depends(get_db), _: User = Depends(require_admin)) -> AdminLandingResponse:
    content = landing_content(get_landing_row(db))
    return AdminLandingResponse(
        blocks=blocks_from_content(content),
        hero_background=hero_background_from_content(content),
    )


@router.put("/landing", response_model=AdminLandingResponse, summary="Replace landing CMS content")
def update_admin_landing(
    payload: AdminLandingUpdate,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminLandingResponse:
    _validate_blocks(payload.blocks)

    row = get_landing_row(db)
    if row is None:
        row = LandingSettings(id=LANDING_SETTINGS_ID, content={})
        db.add(row)
    content = dict(landing_content(row))
    content["blocks"] = payload.blocks
    if payload.hero_background is not None:
        content["hero_background"] = payload.hero_background.model_dump()
    row.content = content
    db.commit()
    db.refresh(row)

    content = landing_content(row)
    return AdminLandingResponse(
        blocks=blocks_from_content(content),
        hero_background=hero_background_from_content(content),
    )


@router.post(
    "/landing/background",
    response_model=HeroBackgroundResponse,
    status_code=status.HTTP_201_CREATED,
    summary="Upload a hero image or video",
)
async def upload_landing_background(
    file: UploadFile = File(..., description="Hero image or video"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> HeroBackgroundResponse:
    suffix = Path(file.filename or "").suffix.lower()
    media = IMAGE_SIGNATURES.get(suffix)
    if media is None:
        raise HTTPException(
            status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE,
            detail="Hero background must be JPEG, PNG, WebP, GIF, MP4 or WebM",
        )
    hero_type, content_type, matches_signature = media

    try:
        head = await file.read(64)
        if not matches_signature(head):
            raise HTTPException(
                status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE,
                detail="File content does not match its extension",
            )

        stored_name = f"hero-{uuid.uuid4()}{suffix}"
        target = _safe_upload_path(settings.landing_media_dir, stored_name)
        await _write_upload(
            file,
            target,
            settings.max_landing_media_bytes,
            prefix=head,
        )

        previous = hero_background_from_content(landing_content(get_landing_row(db)))
        previous_path = _local_media_path(previous.url)

        background = HeroBackground(
            type=hero_type,
            url=f"{LANDING_MEDIA_URL_PREFIX}{stored_name}",
            poster_url=None,
        )
        try:
            saved = _save_landing_background(db, background)
        except Exception:
            _remove_media_file(target)
            raise
        # Remove the replaced local file only after the new URL is committed.
        _remove_media_file(previous_path)
        return HeroBackgroundResponse(hero_background=saved)
    finally:
        await file.close()


@router.delete(
    "/landing/background",
    response_model=HeroBackgroundResponse,
    summary="Remove the hero image or video",
)
def delete_landing_background(
    db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> HeroBackgroundResponse:
    previous = hero_background_from_content(landing_content(get_landing_row(db)))
    saved = _save_landing_background(db, HeroBackground())
    _remove_media_file(_local_media_path(previous.url))
    return HeroBackgroundResponse(hero_background=saved)


@router.get("/game-builds", response_model=GameBuildListResponse, summary="List uploaded game builds")
def list_game_builds(db: Session = Depends(get_db), _: User = Depends(require_admin)) -> GameBuildListResponse:
    builds = db.scalars(
        select(GameBuild).order_by(GameBuild.uploaded_at.desc(), GameBuild.id.desc())
    ).all()
    return GameBuildListResponse(items=[serialize_build(build) for build in builds])


@router.post(
    "/game-builds",
    response_model=GameBuildItem,
    status_code=status.HTTP_201_CREATED,
    summary="Upload a game build",
)
async def upload_game_build(
    file: UploadFile = File(..., description="Desktop build (.exe, .zip or .msi)"),
    version: str | None = Form(default=None, max_length=64),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> GameBuildItem:
    original_name = Path(file.filename or "").name
    suffix = Path(original_name).suffix.lower()
    content_type = GAME_BUILD_TYPES.get(suffix)
    if content_type is None:
        raise HTTPException(
            status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE,
            detail="Only .exe, .zip and .msi builds are accepted",
        )

    stored_name = f"build-{uuid.uuid4()}{suffix}"
    target = _safe_upload_path(settings.game_builds_dir, stored_name)
    try:
        size = await _write_upload(file, target, settings.max_game_build_bytes)
        build = GameBuild(
            version=(version or "").strip(),
            filename=original_name,
            stored_name=stored_name,
            content_type=content_type,
            size_bytes=size,
            is_active=False,
        )
        db.add(build)
        try:
            db.commit()
            db.refresh(build)
        except Exception:
            db.rollback()
            _remove_media_file(target)
            raise
        return serialize_build(build)
    finally:
        await file.close()


@router.post(
    "/game-builds/{build_id}/activate",
    response_model=GameBuildItem,
    summary="Make a build the current download",
)
def activate_game_build(
    build_id: int, db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> GameBuildItem:
    build = db.get(GameBuild, build_id)
    if build is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Game build not found"
        )
    db.execute(update(GameBuild).values(is_active=False))
    build.is_active = True
    db.commit()
    db.refresh(build)
    return serialize_build(build)


@router.delete(
    "/game-builds/{build_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete a game build",
)
def delete_game_build(
    build_id: int, db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> Response:
    build = db.get(GameBuild, build_id)
    if build is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Game build not found"
        )
    target = build_file_path(build)
    db.delete(build)
    db.commit()
    _remove_media_file(target)
    return Response(status_code=status.HTTP_204_NO_CONTENT)
