"""Weekly Daily Challenge schedule: one song per weekday (UTC).

``weekday`` follows the Python convention — ``0`` is Monday, ``6`` is Sunday —
and matches ``datetime.date.weekday()`` used by the daily logic.

Two representations are kept in sync:

* ``daily_slots`` — the editable schedule (one row per weekday);
* ``songs.daily_weekday`` — a denormalised marker on the song itself, which is
  what the public catalogue, the global rating and the personal-best queries
  filter on.

Every write goes through this module so the two can never drift apart. Moving
a song to another weekday, replacing the song of an occupied slot and clearing
a slot all clean up the other side of the relation.
"""
from __future__ import annotations

from datetime import datetime, timezone
from typing import Any

from sqlalchemy import delete, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.orm import Session

from app.models.progression import DailySlot
from app.models.song import Song

WEEKDAY_COUNT = 7
WEEKDAY_NAMES_RU: tuple[str, ...] = (
    "Понедельник",
    "Вторник",
    "Среда",
    "Четверг",
    "Пятница",
    "Суббота",
    "Воскресенье",
)


def is_valid_weekday(weekday: int) -> bool:
    return 0 <= int(weekday) < WEEKDAY_COUNT


def _touch_slot(db: Session, weekday: int, song_id: Any) -> DailySlot:
    """Upsert the slot row. ``songs.daily_weekday`` is updated by the caller."""
    db.execute(
        pg_insert(DailySlot.__table__)
        .values(
            weekday=weekday,
            song_id=song_id,
            updated_at=datetime.now(timezone.utc),
        )
        .on_conflict_do_update(
            index_elements=["weekday"],
            set_={"song_id": song_id, "updated_at": datetime.now(timezone.utc)},
        )
    )
    db.flush()
    slot = db.get(DailySlot, weekday, populate_existing=True)
    if slot is None:  # pragma: no cover - defensive, the upsert must land
        raise RuntimeError("Could not store the daily slot")
    return slot


def set_slot(db: Session, weekday: int, song: Song) -> DailySlot:
    """Assign ``song`` to ``weekday``.

    Conflict policy (chosen on purpose, see README): the assignment **moves**
    the song. If it already occupied another weekday, that slot is cleared; if
    the target weekday was occupied by a different song, that song loses its
    slot. Nothing is rejected with ``409`` — an administrator replacing the
    schedule should not have to clear half of it manually.
    """
    if not is_valid_weekday(weekday):
        raise ValueError("weekday must be in 0..6")
    weekday = int(weekday)

    # The song that currently owns the target slot loses it (its own
    # ``daily_weekday`` must not keep pointing at a slot it no longer owns).
    current = db.get(DailySlot, weekday, with_for_update=True)
    if current is not None and current.song_id != song.id:
        previous_owner = db.get(Song, current.song_id)
        if previous_owner is not None and previous_owner.daily_weekday == weekday:
            previous_owner.daily_weekday = None
            db.flush()

    # A song may be scheduled only once: drop every other slot of this song.
    db.execute(
        delete(DailySlot.__table__).where(
            DailySlot.song_id == song.id, DailySlot.weekday != weekday
        )
    )
    song.daily_weekday = weekday
    db.flush()
    return _touch_slot(db, weekday, song.id)


def clear_slot(db: Session, weekday: int) -> Song | None:
    """Remove the song from ``weekday``. Returns the song that was scheduled."""
    if not is_valid_weekday(weekday):
        raise ValueError("weekday must be in 0..6")
    weekday = int(weekday)

    slot = db.get(DailySlot, weekday, with_for_update=True)
    if slot is None:
        return None
    song = db.get(Song, slot.song_id)
    db.delete(slot)
    db.flush()
    if song is not None and song.daily_weekday == weekday:
        song.daily_weekday = None
        db.flush()
    return song


def apply_song_weekday(db: Session, song: Song) -> None:
    """Reconcile ``daily_slots`` after ``song.daily_weekday`` was changed.

    Called by the admin song create/update endpoints. ``None`` removes every
    slot that points at the song, a value moves the song into that weekday.
    """
    if song.daily_weekday is None:
        removed = db.execute(
            delete(DailySlot.__table__).where(DailySlot.song_id == song.id)
        )
        if removed.rowcount:
            db.flush()
        return
    set_slot(db, int(song.daily_weekday), song)


def slot_song_rows(db: Session) -> dict[int, Song]:
    """Scheduled song per weekday; weekdays without a song are absent."""
    rows = db.execute(
        select(DailySlot.weekday, Song).join(Song, Song.id == DailySlot.song_id)
    ).all()
    return {int(row.weekday): row.Song for row in rows}


def scheduled_song(db: Session, weekday: int) -> Song | None:
    """The song of one weekday, or ``None`` when the slot is free.

    A slot may point at a draft: the schedule is independent of publication so
    an administrator can prepare it in advance. Callers that serve players must
    still require the song to be published.
    """
    if not is_valid_weekday(weekday):
        return None
    return db.scalar(
        select(Song)
        .join(DailySlot, DailySlot.song_id == Song.id)
        .where(DailySlot.weekday == int(weekday))
    )
