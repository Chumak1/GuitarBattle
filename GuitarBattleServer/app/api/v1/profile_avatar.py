"""Player avatar: upload and removal.

The image is stored under ``/media/avatars/avatar-<uuid>.<ext>`` and the
origin-relative URL is written to ``users.avatar_image_url``. Validation is the
same as for the administrator artwork slots: the bytes decide the type
(PNG/JPEG/WebP), the size limit is enforced while streaming, and the previous
file is removed only after the new URL is committed.
"""
from __future__ import annotations

import uuid

from fastapi import APIRouter, Depends, File, HTTPException, UploadFile, status
from sqlalchemy.orm import Session

from app.api.v1.admin_site import (
    _remove_media_file,
    _safe_upload_path,
    _write_upload,
)
from app.api.v1.auth import get_current_user
from app.config import settings
from app.database import get_db
from app.models.user import User
from app.schemas.artwork import AvatarResponse
from app.services.artwork import (
    AVATAR_URL_PREFIX,
    ImageUploadError,
    avatar_path,
    validate_extension,
)

router = APIRouter(prefix="/profile", tags=["profile"])


@router.post(
    "/avatar",
    response_model=AvatarResponse,
    status_code=status.HTTP_201_CREATED,
    summary="Upload or replace the avatar of the signed-in player",
)
async def upload_avatar(
    file: UploadFile = File(..., description="PNG, JPEG or WebP image"),
    db: Session = Depends(get_db),
    current_user: User = Depends(get_current_user),
) -> AvatarResponse:
    """Store a new avatar and return its origin-relative URL.

    Replacing an avatar produces a new URL and deletes the previous file, so a
    client that caches images by URL picks the new picture up.
    """
    try:
        suffix, matches_signature = validate_extension(file.filename, label="Avatar")
    except ImageUploadError as exc:
        raise HTTPException(status_code=exc.status_code, detail=exc.detail) from None

    limit = int(settings.max_avatar_bytes)
    declared = getattr(file, "size", None)
    if declared is not None and declared > limit:
        raise HTTPException(
            status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
            detail="Uploaded file exceeds the configured size limit",
        )

    try:
        head = await file.read(64)
        if not matches_signature(head):
            raise HTTPException(
                status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE,
                detail="File content does not match its extension",
            )

        stored_name = f"avatar-{uuid.uuid4()}{suffix}"
        target = _safe_upload_path(settings.avatar_media_dir, stored_name)
        await _write_upload(file, target, limit, prefix=head)

        previous_path = avatar_path(current_user.avatar_image_url)
        current_user.avatar_image_url = f"{AVATAR_URL_PREFIX}{stored_name}"
        try:
            db.commit()
        except Exception:
            db.rollback()
            _remove_media_file(target)
            raise
        db.refresh(current_user)
        _remove_media_file(previous_path)
        return AvatarResponse(avatar_image_url=current_user.avatar_image_url)
    finally:
        await file.close()


@router.delete(
    "/avatar",
    response_model=AvatarResponse,
    summary="Remove the avatar of the signed-in player",
)
def delete_avatar(
    db: Session = Depends(get_db),
    current_user: User = Depends(get_current_user),
) -> AvatarResponse:
    """Clear the column and delete the stored file. Idempotent."""
    previous_path = avatar_path(current_user.avatar_image_url)
    current_user.avatar_image_url = None
    db.commit()
    _remove_media_file(previous_path)
    return AvatarResponse(avatar_image_url=None)
