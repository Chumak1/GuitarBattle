"""Admin endpoints for the Guitar Skills rating.

Only the single tuning row, its append-only history, a manual recalculation and
a status block are exposed. Public ranking lives in ``games.py``.
"""
from __future__ import annotations

from uuid import UUID

from fastapi import APIRouter, BackgroundTasks, Depends, HTTPException, Query, status
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.database import get_db
from app.models.user import User
from app.schemas.guitar_skills import (
    GuitarSkillsConfigHistoryResponse,
    GuitarSkillsConfigResponse,
    GuitarSkillsConfigUpdate,
    GuitarSkillsRecalculateRequest,
    GuitarSkillsRecalculateResponse,
    GuitarSkillsStatusResponse,
)
from app.services import guitar_skills

router = APIRouter(prefix="/admin/guitar-skills", tags=["admin: guitar skills"])


def _config_response(row) -> GuitarSkillsConfigResponse:
    return GuitarSkillsConfigResponse.model_validate(
        guitar_skills.config_payload(row)
    )


@router.get(
    "/config",
    response_model=GuitarSkillsConfigResponse,
    summary="Read the active Guitar Skills tuning",
)
def read_config(
    db: Session = Depends(get_db),
    _admin: User = Depends(require_admin),
) -> GuitarSkillsConfigResponse:
    row = guitar_skills.get_config(db)
    db.commit()
    db.refresh(row)
    return _config_response(row)


@router.put(
    "/config",
    response_model=GuitarSkillsConfigResponse,
    summary="Update the tuning and schedule a full recalculation",
)
def update_config(
    payload: GuitarSkillsConfigUpdate,
    background: BackgroundTasks,
    db: Session = Depends(get_db),
    admin: User = Depends(require_admin),
) -> GuitarSkillsConfigResponse:
    values = payload.model_dump(exclude_unset=True)
    recalculate = bool(values.pop("recalculate", True))
    if not values:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="No configuration fields supplied",
        )
    try:
        row = guitar_skills.update_config(
            db, values, admin_user_id=admin.id
        )
    except ValueError as exc:
        db.rollback()
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail=str(exc)
        ) from exc
    if recalculate:
        background.add_task(
            guitar_skills.run_recalculation_job, "config_change", False
        )
    return _config_response(row)


@router.get(
    "/config/history",
    response_model=GuitarSkillsConfigHistoryResponse,
    summary="Append-only history of the tuning",
)
def read_config_history(
    limit: int = Query(default=50, ge=1, le=200),
    db: Session = Depends(get_db),
    _admin: User = Depends(require_admin),
) -> GuitarSkillsConfigHistoryResponse:
    return GuitarSkillsConfigHistoryResponse(
        items=guitar_skills.config_history(db, limit=limit)
    )


@router.post(
    "/recalculate",
    response_model=GuitarSkillsRecalculateResponse,
    summary="Recalculate every active player (optionally as a dry run)",
)
def recalculate(
    payload: GuitarSkillsRecalculateRequest,
    db: Session = Depends(get_db),
    _admin: User = Depends(require_admin),
) -> GuitarSkillsRecalculateResponse:
    if guitar_skills.recalculation_running():
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="A recalculation is already running",
        )
    summary = guitar_skills.recalculate_all(
        db, reason=payload.reason, dry_run=payload.dry_run
    )
    return GuitarSkillsRecalculateResponse(
        users=int(summary["users"]),
        changed=int(summary["changed"]),
        items=summary["items"],
    )


@router.get(
    "/status",
    response_model=GuitarSkillsStatusResponse,
    summary="Recalculation state and coverage counters",
)
def read_status(
    db: Session = Depends(get_db),
    _admin: User = Depends(require_admin),
) -> GuitarSkillsStatusResponse:
    return GuitarSkillsStatusResponse.model_validate(
        guitar_skills.status(db)
    )
