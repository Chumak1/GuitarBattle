"""Client activity telemetry used by the internal statistics page.

The desktop client reports a heartbeat every few minutes and records a launch
when the player enters a game mode. The server owns all time calculations:
a session's duration is ``coalesce(ended_at, last_seen_at) - started_at``,
so a client that crashes or is killed still produces a usable number.
"""
from datetime import datetime, timezone

from fastapi import APIRouter, Depends, status
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.api.v1.auth import get_current_user
from app.database import get_db
from app.models.app_session import AppSession, GameLaunch
from app.models.song import Song
from app.models.user import User
from app.schemas.statistics import (
    GameLaunchRequest,
    GameLaunchResponse,
    HeartbeatRequest,
    HeartbeatResponse,
)

router = APIRouter(prefix="/activity", tags=["activity"])


def _existing_session(
    db: Session, user_id, client_session_id: str
) -> AppSession | None:
    return db.scalar(
        select(AppSession).where(
            AppSession.user_id == user_id,
            AppSession.client_session_id == client_session_id,
        )
    )


@router.post(
    "/heartbeat",
    response_model=HeartbeatResponse,
    summary="Report that the app is running",
)
def heartbeat(
    payload: HeartbeatRequest,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> HeartbeatResponse:
    """Create or refresh the caller's app session.

    Repeated calls for the same ``client_session_id`` only move
    ``last_seen_at`` forward; ``final=true`` closes the session.
    """
    now = datetime.now(timezone.utc)
    session = _existing_session(db, user.id, payload.client_session_id)

    if session is None:
        session = AppSession(
            user_id=user.id,
            client_session_id=payload.client_session_id,
            started_at=now,
            last_seen_at=now,
            app_version=payload.app_version,
            platform=payload.platform,
        )
        db.add(session)
        try:
            db.commit()
        except IntegrityError:
            # Another heartbeat for the same client session won the race.
            db.rollback()
            session = _existing_session(db, user.id, payload.client_session_id)
            if session is None:
                raise
            db.refresh(session)
    else:
        if now > session.last_seen_at:
            session.last_seen_at = now
        if payload.app_version:
            session.app_version = payload.app_version
        if payload.platform:
            session.platform = payload.platform
        if payload.final:
            session.ended_at = now
        db.commit()
        db.refresh(session)

    if payload.final and session.ended_at is None:
        session.ended_at = now
        db.commit()
        db.refresh(session)

    return HeartbeatResponse(
        session_id=session.id,
        started_at=session.started_at,
        last_seen_at=session.last_seen_at,
    )


@router.post(
    "/game-launch",
    response_model=GameLaunchResponse,
    status_code=status.HTTP_201_CREATED,
    summary="Record a game launch",
)
def record_game_launch(
    payload: GameLaunchRequest,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> GameLaunchResponse:
    """Store one transition into a game mode.

    Telemetry must never fail the client, so an unknown ``song_id`` is dropped
    instead of raising a foreign-key error, and a missing app session is
    recorded as ``app_session_id = NULL``.
    """
    app_session_id = None
    if payload.client_session_id:
        app_session_id = db.scalar(
            select(AppSession.id).where(
                AppSession.user_id == user.id,
                AppSession.client_session_id == payload.client_session_id,
            )
        )

    song_id = payload.song_id
    if song_id is not None and db.scalar(select(Song.id).where(Song.id == song_id)) is None:
        song_id = None

    launch = GameLaunch(
        user_id=user.id,
        app_session_id=app_session_id,
        game_key=payload.game_key,
        song_id=song_id,
        mode=payload.mode,
        started_at=datetime.now(timezone.utc),
    )
    db.add(launch)
    db.commit()
    db.refresh(launch)
    return GameLaunchResponse(id=launch.id)
