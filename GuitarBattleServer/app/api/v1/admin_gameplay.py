"""Admin endpoints for gameplay visuals: UI-level characters and stages.

Two content collections are managed here:

* **UI LEVELS** — a character display name plus one transparent PNG spritesheet
  with eight poses in a 4x2 grid.
* **STAGE LEVELS** — a standalone stage background with a scene name. The form
  asks for exactly those two things; a stage is not tied to a character.

Every route uses :func:`app.api.v1.admin_songs.require_admin`, the same
administrator dependency as the songs panel. Files are validated by signature
(not by the client ``content_type``), stored under a generated name and served
publicly from ``/media/characters/`` and ``/media/stages/``.
"""
from __future__ import annotations

import hashlib
import uuid
from pathlib import Path

from fastapi import APIRouter, Depends, File, Form, HTTPException, Response, UploadFile, status
from sqlalchemy import func, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.config import settings
from app.database import get_db
from app.models.gameplay_visuals import StageLevel, UiCharacter
from app.models.user import User
from app.schemas.gameplay_visuals import (
    StageLevelListResponse,
    StageLevelResponse,
    UiCharacterListResponse,
    UiCharacterResponse,
)
from app.services.gameplay_media import (
    GameplayMediaError,
    character_sheet_url,
    characters_dir,
    delete_image,
    stage_background_url,
    stages_dir,
    store_image,
    validate_background,
    validate_spritesheet,
)

router = APIRouter(prefix="/admin", tags=["admin: gameplay visuals"])

MAX_NAME_LENGTH = 120


def _clean_name(value: str | None, *, field: str = "name") -> str:
    cleaned = (value or "").strip()
    if not cleaned:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=f"{field.capitalize()} must not be blank",
        )
    if len(cleaned) > MAX_NAME_LENGTH:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=f"{field.capitalize()} must be at most {MAX_NAME_LENGTH} characters",
        )
    return cleaned


async def _read_image(file: UploadFile) -> bytes:
    limit = settings.max_gameplay_media_bytes
    try:
        data = await file.read(limit + 1)
    finally:
        await file.close()
    if len(data) > limit:
        raise HTTPException(
            status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
            detail=(
                "Uploaded image exceeds the configured size limit "
                f"({limit // (1024 * 1024)} MiB)"
            ),
        )
    if not data:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Uploaded file is empty",
        )
    return data


def _media_error(exc: GameplayMediaError) -> HTTPException:
    return HTTPException(status_code=exc.status_code, detail=exc.message)


def _character_response(character: UiCharacter) -> UiCharacterResponse:
    return UiCharacterResponse(
        id=character.id,
        name=character.name,
        sheet_url=character_sheet_url(character.sheet_key),
        content_type=character.content_type,
        size_bytes=character.size_bytes,
        original_name=character.original_name,
        created_at=character.created_at,
        updated_at=character.updated_at,
    )


def _stage_response(stage: StageLevel) -> StageLevelResponse:
    return StageLevelResponse(
        id=stage.id,
        name=stage.name,
        background_url=stage_background_url(stage.background_key),
        content_type=stage.content_type,
        size_bytes=stage.size_bytes,
        original_name=stage.original_name,
        created_at=stage.created_at,
        updated_at=stage.updated_at,
    )


def _name_taken(db: Session, name: str, *, exclude_id: uuid.UUID | None = None) -> bool:
    query = select(UiCharacter.id).where(
        func.lower(UiCharacter.name) == name.lower()
    )
    if exclude_id is not None:
        query = query.where(UiCharacter.id != exclude_id)
    return db.scalar(query) is not None


# ---------------------------------------------------------------------------
# UI LEVELS (characters)
# ---------------------------------------------------------------------------
@router.get(
    "/ui-levels",
    response_model=UiCharacterListResponse,
    summary="List UI-level characters",
)
def list_ui_levels(
    db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> UiCharacterListResponse:
    characters = db.scalars(
        select(UiCharacter).order_by(UiCharacter.name.asc(), UiCharacter.id.asc())
    ).all()
    return UiCharacterListResponse(
        items=[_character_response(character) for character in characters]
    )


@router.post(
    "/ui-levels",
    response_model=UiCharacterResponse,
    status_code=status.HTTP_201_CREATED,
    summary="Create a UI-level character from a PNG spritesheet",
)
async def create_ui_level(
    name: str = Form(..., max_length=MAX_NAME_LENGTH, description="Unique display name"),
    file: UploadFile = File(..., description="PNG spritesheet with 8 poses (4x2)"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> UiCharacterResponse:
    cleaned_name = _clean_name(name)
    if _name_taken(db, cleaned_name):
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=f"Character '{cleaned_name}' already exists",
        )

    data = await _read_image(file)
    try:
        content_type = validate_spritesheet(data)
    except GameplayMediaError as exc:
        raise _media_error(exc) from exc

    sheet_key = store_image(characters_dir(), data, ".png")
    character = UiCharacter(
        name=cleaned_name,
        sheet_key=sheet_key,
        original_name=Path(file.filename or "").name or None,
        content_type=content_type,
        size_bytes=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
    )
    db.add(character)
    try:
        db.commit()
        db.refresh(character)
    except IntegrityError:
        db.rollback()
        delete_image(characters_dir(), sheet_key)
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=f"Character '{cleaned_name}' already exists",
        )
    except Exception:
        db.rollback()
        delete_image(characters_dir(), sheet_key)
        raise
    return _character_response(character)


@router.post(
    "/ui-levels/{character_id}/replace",
    response_model=UiCharacterResponse,
    summary="Replace the spritesheet and/or rename a character",
)
async def replace_ui_level(
    character_id: uuid.UUID,
    name: str | None = Form(default=None, max_length=MAX_NAME_LENGTH),
    file: UploadFile | None = File(default=None),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> UiCharacterResponse:
    character = db.get(UiCharacter, character_id)
    if character is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Character not found"
        )

    new_name: str | None = None
    if name is not None:
        new_name = _clean_name(name)
        if new_name != character.name and _name_taken(
            db, new_name, exclude_id=character.id
        ):
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail=f"Character '{new_name}' already exists",
            )

    data: bytes | None = None
    content_type: str | None = None
    if file is not None:
        data = await _read_image(file)
        try:
            content_type = validate_spritesheet(data)
        except GameplayMediaError as exc:
            raise _media_error(exc) from exc

    if new_name is None and data is None:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Provide a new name or a new spritesheet",
        )

    previous_key = character.sheet_key
    new_key: str | None = None
    if data is not None:
        new_key = store_image(characters_dir(), data, ".png")
        character.sheet_key = new_key
        character.content_type = content_type or "image/png"
        character.size_bytes = len(data)
        character.sha256 = hashlib.sha256(data).hexdigest()
        character.original_name = Path(file.filename or "").name or None
    if new_name is not None:
        character.name = new_name

    try:
        db.commit()
        db.refresh(character)
    except IntegrityError:
        db.rollback()
        delete_image(characters_dir(), new_key)
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Character name already exists",
        )
    except Exception:
        db.rollback()
        delete_image(characters_dir(), new_key)
        raise

    if new_key is not None:
        delete_image(characters_dir(), previous_key)
    return _character_response(character)


@router.delete(
    "/ui-levels/{character_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete a character",
)
def delete_ui_level(
    character_id: uuid.UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> Response:
    """Delete a character and clear every optional selection that uses it.

    ``songs.character_id`` and ``users.character_id`` are ``ON DELETE SET NULL``,
    so songs and player profiles keep working without the visual. Stage levels
    are standalone backgrounds and are not affected.
    """
    character = db.get(UiCharacter, character_id)
    if character is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Character not found"
        )

    sheet_key = character.sheet_key
    db.delete(character)
    db.commit()
    delete_image(characters_dir(), sheet_key)
    return Response(status_code=status.HTTP_204_NO_CONTENT)


# ---------------------------------------------------------------------------
# STAGE LEVELS (backgrounds)
# ---------------------------------------------------------------------------
@router.get(
    "/stage-levels",
    response_model=StageLevelListResponse,
    summary="List stage backgrounds",
)
def list_stage_levels(
    db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> StageLevelListResponse:
    stages = db.scalars(
        select(StageLevel).order_by(StageLevel.name.asc(), StageLevel.id.asc())
    ).all()
    return StageLevelListResponse(items=[_stage_response(stage) for stage in stages])


@router.post(
    "/stage-levels",
    response_model=StageLevelResponse,
    status_code=status.HTTP_201_CREATED,
    summary="Create a standalone stage background",
)
async def create_stage_level(
    name: str = Form(..., max_length=MAX_NAME_LENGTH, description="Scene display name"),
    file: UploadFile = File(..., description="Stage background (PNG or JPEG)"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> StageLevelResponse:
    cleaned_name = _clean_name(name)

    data = await _read_image(file)
    try:
        suffix, content_type = validate_background(data)
    except GameplayMediaError as exc:
        raise _media_error(exc) from exc

    background_key = store_image(stages_dir(), data, suffix)
    stage = StageLevel(
        name=cleaned_name,
        background_key=background_key,
        original_name=Path(file.filename or "").name or None,
        content_type=content_type,
        size_bytes=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
    )
    db.add(stage)
    try:
        db.commit()
        db.refresh(stage)
    except Exception:
        db.rollback()
        delete_image(stages_dir(), background_key)
        raise
    return _stage_response(stage)


@router.post(
    "/stage-levels/{stage_id}/replace",
    response_model=StageLevelResponse,
    summary="Replace the background and/or rename a stage",
)
async def replace_stage_level(
    stage_id: uuid.UUID,
    name: str | None = Form(default=None, max_length=MAX_NAME_LENGTH),
    file: UploadFile | None = File(default=None),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> StageLevelResponse:
    stage = db.get(StageLevel, stage_id)
    if stage is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Stage not found"
        )

    new_name = _clean_name(name) if name is not None else None

    data: bytes | None = None
    suffix = ".png"
    content_type = "image/png"
    if file is not None:
        data = await _read_image(file)
        try:
            suffix, content_type = validate_background(data)
        except GameplayMediaError as exc:
            raise _media_error(exc) from exc

    if new_name is None and data is None:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Provide a new name or a new background",
        )

    previous_key = stage.background_key
    new_key: str | None = None
    if data is not None:
        new_key = store_image(stages_dir(), data, suffix)
        stage.background_key = new_key
        stage.content_type = content_type
        stage.size_bytes = len(data)
        stage.sha256 = hashlib.sha256(data).hexdigest()
        stage.original_name = Path(file.filename or "").name or None
    if new_name is not None:
        stage.name = new_name

    try:
        db.commit()
        db.refresh(stage)
    except Exception:
        db.rollback()
        delete_image(stages_dir(), new_key)
        raise

    if new_key is not None:
        delete_image(stages_dir(), previous_key)
    return _stage_response(stage)


@router.delete(
    "/stage-levels/{stage_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete a stage background",
)
def delete_stage_level(
    stage_id: uuid.UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> Response:
    stage = db.get(StageLevel, stage_id)
    if stage is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Stage not found"
        )
    background_key = stage.background_key
    db.delete(stage)
    db.commit()
    delete_image(stages_dir(), background_key)
    return Response(status_code=status.HTTP_204_NO_CONTENT)
