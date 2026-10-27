"""Song availability: archive instead of delete, never lose player results.

A song row has a stable ``id`` that user-visible data points at
(``song_best_scores``, ``game_sessions``/``scores``, ``daily_challenges`` and
``daily_results``). Physical deletion therefore must not be the default way to
take a song out of the catalogue: it either cascaded into those rows or was
rejected by the database, both of which are bad administrator experiences.

``songs.archived_at`` separates *availability* from *retention*:

* ``NULL`` — a working catalogue entry;
* a timestamp — archived: hidden from the public catalogue, no new game
  sessions, no Daily Challenge slot, but every historical result stays attached
  to the song and keeps feeding the rating and the player's mastery.

Hard deletion stays available for songs that were never played (a mistyped
draft), and only for those: :func:`song_usage` is checked first and the
``RESTRICT`` foreign keys are the final guard.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

from sqlalchemy import delete, func, select
from sqlalchemy.orm import Session

from app.models.game_session import GameSession
from app.models.progression import DailyChallenge, DailySlot, SongBestScore
from app.models.song import Song


@dataclass(frozen=True)
class SongUsage:
    """How much user data points at one song."""

    sessions: int = 0
    best_scores: int = 0
    daily_challenges: int = 0

    @property
    def has_user_results(self) -> bool:
        return bool(self.sessions or self.best_scores or self.daily_challenges)

    def as_detail(self) -> dict[str, int]:
        return {
            "game_sessions": self.sessions,
            "song_best_scores": self.best_scores,
            "daily_challenges": self.daily_challenges,
        }


def _count(db: Session, table: Any, column: Any, value: Any) -> int:
    return int(
        db.scalar(select(func.count()).select_from(table).where(column == value)) or 0
    )


def song_usage(db: Session, song_id: Any) -> SongUsage:
    """Count the user results that reference ``song_id``.

    ``DailyResult`` has no direct song column: it belongs to a
    ``daily_challenges`` row, whose ``song_id`` is ``RESTRICT``, so counting the
    challenges covers the official daily attempts as well.
    """
    return SongUsage(
        sessions=_count(db, GameSession.__table__, GameSession.song_id, song_id),
        best_scores=_count(db, SongBestScore.__table__, SongBestScore.song_id, song_id),
        daily_challenges=_count(
            db, DailyChallenge.__table__, DailyChallenge.song_id, song_id
        ),
    )


def _clear_daily_slots(db: Session, song: Song) -> None:
    """Remove the song from the weekly schedule (scheduling is not user data)."""
    weekdays = list(
        db.scalars(select(DailySlot.weekday).where(DailySlot.song_id == song.id)).all()
    )
    if weekdays:
        db.execute(delete(DailySlot.__table__).where(DailySlot.song_id == song.id))
    song.daily_weekday = None
    if weekdays:
        db.flush()


def archive_song(
    db: Session, song: Song, *, now: datetime | None = None
) -> Song:
    """Hide ``song`` from players without touching a single result row.

    Idempotent: archiving an archived song only refreshes the timestamp when it
    was missing. The song also leaves the Daily Challenge rotation, because its
    assets are no longer served publicly and the official attempt could not be
    played; already frozen ``daily_challenges`` rows keep their history.
    """
    if song.archived_at is None:
        song.archived_at = now or datetime.now(timezone.utc)
    song.is_published = False
    _clear_daily_slots(db, song)
    db.flush()
    return song


def restore_song(db: Session, song: Song) -> Song:
    """Return an archived song to the working catalogue as a draft."""
    song.archived_at = None
    db.flush()
    return song
