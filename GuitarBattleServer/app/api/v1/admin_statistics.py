"""Internal admin statistics: users, app time and game launches.

Every number is aggregated in SQL (``select`` + ``group_by`` / per-user
subqueries joined once), so the page stays fast as telemetry grows and the
database never issues an N+1 query per user.
"""
from datetime import datetime, timedelta, timezone
from typing import Literal

from fastapi import APIRouter, Depends, Query
from sqlalchemy import and_, func, or_, select
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.database import get_db
from app.models.app_session import AppSession, GameLaunch
from app.models.song import Song
from app.models.user import User
from app.schemas.statistics import (
    StatisticsGameItem,
    StatisticsGameList,
    StatisticsOverview,
    StatisticsUserItem,
    StatisticsUserList,
)

router = APIRouter(prefix="/admin/statistics", tags=["admin: statistics"])

# A session's duration is measured by the server; an unfinished session counts
# up to its last heartbeat, and a clock shift never produces negative time.
SESSION_SECONDS = func.greatest(
    func.extract(
        "epoch",
        func.coalesce(AppSession.ended_at, AppSession.last_seen_at)
        - AppSession.started_at,
    ),
    0,
)


def _window_start(days: int) -> datetime:
    return datetime.now(timezone.utc) - timedelta(days=days)


def _active_users_since(since: datetime):
    """Distinct users seen through a heartbeat or a game launch after ``since``."""
    return (
        select(AppSession.user_id)
        .where(AppSession.last_seen_at >= since)
        .union(select(GameLaunch.user_id).where(GameLaunch.started_at >= since))
        .subquery()
    )


def _search_filter(search: str):
    """Case-insensitive substring match over login, display name and email."""
    escaped = search.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
    pattern = f"%{escaped}%"
    return or_(
        User.username.ilike(pattern, escape="\\"),
        User.display_name.ilike(pattern, escape="\\"),
        User.email.ilike(pattern, escape="\\"),
    )


@router.get("/overview", response_model=StatisticsOverview, summary="Aggregate activity overview")
def statistics_overview(
    days: int = Query(default=30, ge=1, le=365, description="Window for sessions and launches"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> StatisticsOverview:
    now = datetime.now(timezone.utc)
    since = _window_start(days)
    since_24h = now - timedelta(hours=24)
    since_7d = now - timedelta(days=7)

    users_total = db.scalar(select(func.count(User.id))) or 0
    users_active_24h = (
        db.scalar(
            select(func.count()).select_from(_active_users_since(since_24h))
        )
        or 0
    )
    users_active_7d = (
        db.scalar(select(func.count()).select_from(_active_users_since(since_7d))) or 0
    )
    sessions_total = (
        db.scalar(
            select(func.count(AppSession.id)).where(AppSession.started_at >= since)
        )
        or 0
    )
    app_seconds_total = int(
        db.scalar(
            select(func.coalesce(func.sum(SESSION_SECONDS), 0)).where(
                AppSession.started_at >= since
            )
        )
        or 0
    )
    launches_total = (
        db.scalar(
            select(func.count(GameLaunch.id)).where(GameLaunch.started_at >= since)
        )
        or 0
    )
    launches_24h = (
        db.scalar(
            select(func.count(GameLaunch.id)).where(GameLaunch.started_at >= since_24h)
        )
        or 0
    )

    return StatisticsOverview(
        generated_at=now,
        users_total=int(users_total),
        users_active_24h=int(users_active_24h),
        users_active_7d=int(users_active_7d),
        sessions_total=int(sessions_total),
        app_seconds_total=app_seconds_total,
        avg_session_seconds=(
            round(app_seconds_total / int(sessions_total)) if sessions_total else 0
        ),
        launches_total=int(launches_total),
        launches_24h=int(launches_24h),
    )


@router.get("/users", response_model=StatisticsUserList, summary="Per-user statistics")
def statistics_users(
    search: str | None = Query(default=None, min_length=1, max_length=100),
    sort: Literal["last_seen", "app_seconds", "launches"] = Query(default="last_seen"),
    limit: int = Query(default=50, ge=1, le=200),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> StatisticsUserList:
    """One row per account with app time, session and launch aggregates.

    ``last_seen`` sorts by the newest of heartbeat and launch time, so players
    who only started a game still appear as recently active. Rows without any
    activity go last in every sort mode.
    """
    session_agg = (
        select(
            AppSession.user_id.label("user_id"),
            func.count(AppSession.id).label("sessions_count"),
            func.max(AppSession.last_seen_at).label("last_seen_at"),
            func.coalesce(func.sum(SESSION_SECONDS), 0).label("app_seconds_total"),
        )
        .group_by(AppSession.user_id)
        .subquery()
    )
    launch_agg = (
        select(
            GameLaunch.user_id.label("user_id"),
            func.count(GameLaunch.id).label("launches_count"),
            func.max(GameLaunch.started_at).label("last_launch_at"),
            func.count(func.distinct(GameLaunch.game_key)).label("games_distinct"),
        )
        .group_by(GameLaunch.user_id)
        .subquery()
    )

    last_activity = func.greatest(
        session_agg.c.last_seen_at, launch_agg.c.last_launch_at
    )
    app_seconds = func.coalesce(session_agg.c.app_seconds_total, 0)
    launches = func.coalesce(launch_agg.c.launches_count, 0)

    if sort == "app_seconds":
        ordering = (app_seconds.desc(), last_activity.desc().nullslast(), User.id.asc())
    elif sort == "launches":
        ordering = (launches.desc(), last_activity.desc().nullslast(), User.id.asc())
    else:
        ordering = (last_activity.desc().nullslast(), User.username.asc())

    conditions = []
    if search:
        conditions.append(_search_filter(search.strip()))

    stmt = (
        select(
            User.id.label("user_id"),
            User.username,
            User.display_name,
            User.email,
            User.is_active,
            User.created_at,
            app_seconds.label("app_seconds_total"),
            func.coalesce(session_agg.c.sessions_count, 0).label("sessions_count"),
            session_agg.c.last_seen_at,
            launches.label("launches_count"),
            launch_agg.c.last_launch_at,
            func.coalesce(launch_agg.c.games_distinct, 0).label("games_distinct"),
        )
        .outerjoin(session_agg, session_agg.c.user_id == User.id)
        .outerjoin(launch_agg, launch_agg.c.user_id == User.id)
        .where(*conditions)
        .order_by(*ordering)
        .limit(limit)
        .offset(offset)
    )
    rows = db.execute(stmt).all()

    total = db.scalar(select(func.count(User.id)).where(*conditions)) or 0

    items = [
        StatisticsUserItem(
            user_id=row.user_id,
            username=row.username,
            display_name=row.display_name,
            email=row.email,
            is_active=row.is_active,
            created_at=row.created_at,
            app_seconds_total=int(row.app_seconds_total or 0),
            sessions_count=int(row.sessions_count or 0),
            last_seen_at=row.last_seen_at,
            launches_count=int(row.launches_count or 0),
            last_launch_at=row.last_launch_at,
            games_distinct=int(row.games_distinct or 0),
        )
        for row in rows
    ]
    return StatisticsUserList(items=items, total=int(total), limit=limit, offset=offset)


@router.get("/games", response_model=StatisticsGameList, summary="Per-game launch statistics")
def statistics_games(
    days: int = Query(default=30, ge=1, le=365),
    limit: int = Query(default=100, ge=1, le=500),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> StatisticsGameList:
    """Launches grouped by ``game_key``.

    A launch may point at a song; the title of the newest launch that has one
    names the group. Keys without a song (``daily_challenge``, ``practice``,
    ``local:<slug>``) keep the key itself as the title.
    """
    since = _window_start(days)

    newest_song_launch = (
        select(
            GameLaunch.game_key.label("game_key"),
            GameLaunch.song_id.label("song_id"),
            func.row_number()
            .over(
                partition_by=GameLaunch.game_key,
                order_by=(GameLaunch.started_at.desc(), GameLaunch.id.desc()),
            )
            .label("row_number"),
        )
        .where(GameLaunch.started_at >= since, GameLaunch.song_id.is_not(None))
        .subquery()
    )

    launch_count = func.count(GameLaunch.id).label("launches")
    stmt = (
        select(
            GameLaunch.game_key,
            launch_count,
            func.count(func.distinct(GameLaunch.user_id)).label("unique_users"),
            func.max(GameLaunch.started_at).label("last_launch_at"),
            Song.title.label("song_title"),
        )
        .select_from(GameLaunch)
        .outerjoin(
            newest_song_launch,
            and_(
                newest_song_launch.c.game_key == GameLaunch.game_key,
                newest_song_launch.c.row_number == 1,
            ),
        )
        .outerjoin(Song, Song.id == newest_song_launch.c.song_id)
        .where(GameLaunch.started_at >= since)
        .group_by(GameLaunch.game_key, Song.title)
        .order_by(launch_count.desc(), GameLaunch.game_key.asc())
        .limit(limit)
    )
    rows = db.execute(stmt).all()

    total = (
        db.scalar(
            select(func.count(func.distinct(GameLaunch.game_key))).where(
                GameLaunch.started_at >= since
            )
        )
        or 0
    )

    items = [
        StatisticsGameItem(
            game_key=row.game_key,
            title=row.song_title or row.game_key,
            launches=int(row.launches or 0),
            unique_users=int(row.unique_users or 0),
            last_launch_at=row.last_launch_at,
        )
        for row in rows
    ]
    return StatisticsGameList(items=items, total=int(total))
