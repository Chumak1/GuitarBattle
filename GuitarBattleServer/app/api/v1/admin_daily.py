"""Admin API for the weekly Daily Challenge schedule.

Endpoints (both require ``is_admin``):

* ``GET /admin/daily`` — all seven weekdays with the scheduled song or ``null``;
* ``PUT /admin/daily/{weekday}`` — assign (``{"song_id": "<uuid>"}``) or clear
  (``{"song_id": null}``) one weekday.

Conflict policy: assigning a song that already occupies another weekday *moves*
it (the old slot is cleared). Assigning a song to a weekday that already has a
song replaces it; the previous song stops being a daily song
(``daily_weekday = NULL``) and returns to the ordinary catalogue.
"""
from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException, Path, status
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.database import get_db
from app.models.progression import DailySlot
from app.models.song import Song
from app.models.user import User
from app.schemas.admin_daily import (
    AdminDailyResponse,
    AdminDailySlot,
    AdminDailySlotUpdate,
    AdminDailySong,
)
from app.services import daily_slots
from app.services.daily_slots import WEEKDAY_COUNT, WEEKDAY_NAMES_RU

router = APIRouter(prefix="/admin/daily", tags=["admin: daily"])


def _slot_payload(
    db: Session, weekday: int, slot: DailySlot | None = None
) -> AdminDailySlot:
    if slot is None:
        slot = db.get(DailySlot, weekday)
    song = db.get(Song, slot.song_id) if slot is not None else None
    return AdminDailySlot(
        weekday=weekday,
        weekday_name=WEEKDAY_NAMES_RU[weekday],
        song=AdminDailySong.model_validate(song) if song is not None else None,
        updated_at=slot.updated_at if slot is not None else None,
    )


@router.get("", response_model=AdminDailyResponse, summary="Weekly daily-challenge schedule")
def list_daily_slots(
    db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> AdminDailyResponse:
    scheduled = daily_slots.slot_song_rows(db)
    slots = [db.get(DailySlot, weekday) for weekday in range(WEEKDAY_COUNT)]
    items = []
    for weekday in range(WEEKDAY_COUNT):
        song = scheduled.get(weekday)
        slot = slots[weekday]
        items.append(
            AdminDailySlot(
                weekday=weekday,
                weekday_name=WEEKDAY_NAMES_RU[weekday],
                song=AdminDailySong.model_validate(song) if song is not None else None,
                updated_at=slot.updated_at if slot is not None else None,
            )
        )
    return AdminDailyResponse(items=items, total=len(items))


@router.put(
    "/{weekday}",
    response_model=AdminDailySlot,
    summary="Assign or clear the song of one weekday",
)
def set_daily_slot(
    payload: AdminDailySlotUpdate,
    weekday: int = Path(ge=0, le=6, description="0 = Monday ... 6 = Sunday (UTC)"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminDailySlot:
    if payload.song_id is None:
        daily_slots.clear_slot(db, weekday)
        try:
            db.commit()
        except IntegrityError:  # pragma: no cover - no unique conflict on delete
            db.rollback()
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT, detail="Daily slot conflict"
            )
        return _slot_payload(db, weekday)

    song = db.get(Song, payload.song_id)
    if song is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Song not found")

    slot = daily_slots.set_slot(db, weekday, song)
    try:
        db.commit()
    except IntegrityError:
        db.rollback()
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail="Daily slot conflict"
        )
    db.refresh(slot)
    return _slot_payload(db, weekday, slot)
