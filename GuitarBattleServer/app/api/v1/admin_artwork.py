"""Administrator artwork slots: HOME hero backgrounds and the default avatar.

Every route requires an administrator JWT (the same dependency as the rest of
the admin API). One image per slot: uploading replaces the previous file, so a
cached URL always points at the current picture.

The public counterparts are ``GET /site/config`` (URLs used by the desktop
client) and the static mount ``/media/artwork`` in ``app/main.py``.
"""
from __future__ import annotations

import uuid
from typing import Literal

from fastapi import APIRouter, Depends, File, HTTPException, UploadFile, status
from sqlalchemy import select
from sqlalchemy.orm import Session

from app.api.v1.admin_site import (
    _remove_media_file,
    _safe_upload_path,
    _write_upload,
)
from app.api.v1.admin_songs import require_admin
from app.database import get_db
from app.models.artwork import ArtworkAsset
from app.models.user import User
from app.schemas.artwork import ArtworkSlotItem, ArtworkSlotListResponse
from app.services.artwork import (
    ARTWORK_SLOTS,
    SLOT_LABELS,
    ImageUploadError,
    artwork_limit,
    artwork_path,
    serialize_slot,
    slot_assets,
    slot_storage_dir,
    url_prefix_for_slot,
    validate_extension,
)

router = APIRouter(prefix="/admin/artwork", tags=["admin: artwork"])

#: Path parameter of the upload/delete routes. An unknown value is a 422.
SlotParam = Literal["hero-battle", "hero-duel", "default-avatar"]


def _require_slot(slot: str) -> str:
    if slot not in ARTWORK_SLOTS:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Unknown artwork slot",
        )
    return slot


@router.get(
    "",
    response_model=ArtworkSlotListResponse,
    summary="Current image of every artwork slot",
)
def list_artwork(
    db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> ArtworkSlotListResponse:
    """All three slots in panel order; ``url`` is ``null`` while a slot is empty."""
    assets = slot_assets(db)
    return ArtworkSlotListResponse(
        items=[serialize_slot(slot, assets.get(slot)) for slot in ARTWORK_SLOTS]
    )


@router.post(
    "/{slot}/image",
    response_model=ArtworkSlotItem,
    status_code=status.HTTP_201_CREATED,
    summary="Upload or replace the image of one slot",
)
async def upload_artwork(
    slot: SlotParam,
    file: UploadFile = File(..., description="PNG, JPEG or WebP image"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> ArtworkSlotItem:
    """Store the image under ``/media/artwork/<slot>-<uuid>.<ext>``.

    The bytes decide the type: only PNG, JPEG and WebP are accepted (415
    otherwise) and the configured limit is enforced while streaming (413).
    Replacing an image deletes the previous file after the new URL is
    committed.
    """
    slot = _require_slot(slot)
    label = SLOT_LABELS[slot]
    try:
        suffix, matches_signature = validate_extension(file.filename, label=label)
    except ImageUploadError as exc:
        raise HTTPException(status_code=exc.status_code, detail=exc.detail) from None

    limit = artwork_limit(slot)
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

        stored_name = f"{slot}-{uuid.uuid4()}{suffix}"
        target = _safe_upload_path(slot_storage_dir(slot), stored_name)
        await _write_upload(file, target, limit, prefix=head)

        asset = db.scalar(select(ArtworkAsset).where(ArtworkAsset.slot == slot))
        previous_path = artwork_path(asset.url) if asset is not None else None
        new_url = f"{url_prefix_for_slot(slot)}{stored_name}"
        if asset is None:
            asset = ArtworkAsset(slot=slot, url=new_url)
            db.add(asset)
        else:
            asset.url = new_url
        try:
            db.commit()
        except Exception:
            db.rollback()
            _remove_media_file(target)
            raise
        db.refresh(asset)
        # Remove the replaced local file only after the new URL is committed.
        _remove_media_file(previous_path)
        return serialize_slot(slot, asset)
    finally:
        await file.close()


@router.delete(
    "/{slot}/image",
    response_model=ArtworkSlotItem,
    summary="Remove the image of one slot",
)
def delete_artwork(
    slot: SlotParam,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> ArtworkSlotItem:
    """Delete the stored file and the slot row. Idempotent."""
    slot = _require_slot(slot)
    asset = db.scalar(select(ArtworkAsset).where(ArtworkAsset.slot == slot))
    previous_path = artwork_path(asset.url) if asset is not None else None
    if asset is not None:
        db.delete(asset)
        db.commit()
    _remove_media_file(previous_path)
    return ArtworkSlotItem(slot=slot)
