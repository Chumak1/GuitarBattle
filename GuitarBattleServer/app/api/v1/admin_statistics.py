"""Internal admin statistics: users, app time and game launches.

Every number is aggregated in SQL (``select`` + ``group_by`` / per-user
subqueries joined once), so the page stays fast as telemetry grows and the
database never issues an N+1 query per user.
"""
from datetime import datetime, timedelta, timezone
import logging
from typing import Literal
from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query, status
from sqlalchemy import and_, func, or_, select
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.database import get_db
from app.models.app_session import AppSession, GameLaunch
from app.models.game_session import GameSession, Score
from app.models.song import Song
from app.models.user import User
from app.schemas.statistics import (
    StatisticsCountryItem,
    StatisticsGameItem,
    StatisticsGameList,
    StatisticsOverview,
    StatisticsUserExclusion,
    StatisticsUserExclusionUpdate,
    StatisticsUserItem,
    StatisticsUserList,
    UserPlatform,
)
from app.services.geolocation import country_flag
from app.services.platforms import normalize_platform
from app.services.admin_user_stats import (
    battle_summary_by_user,
    online_summary_by_user,
)

logger = logging.getLogger(__name__)

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


def _included_user_ids():
    """Ids of the accounts that count towards the internal statistics.

    One scalar subquery, reused by every aggregate: ``IN (SELECT id ...)`` on
    the primary key keeps the excluded accounts (friends, admin and test
    players) out of the numbers without a query per row and without a join that
    could multiply the counts.
    """
    return select(User.id).where(User.statistics_excluded.is_(False))


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


def _platforms_by_user(
    db: Session, user_ids: list[UUID]
) -> dict[UUID, list[UserPlatform]]:
    """Group the app sessions of one page of users by reported OS name.

    A single ``GROUP BY`` query for the whole page keeps the endpoint free of
    N+1 queries: every heartbeat collapses into one row per
    (user, raw platform), newest first. ``platform IS NULL`` marks sessions
    written before the client sent an OS name and is skipped, as is an empty
    string from an older heartbeat.
    """
    if not user_ids:
        return {}

    last_seen = func.max(AppSession.last_seen_at).label("last_seen_at")
    session_count = func.count(AppSession.id).label("sessions")
    rows = db.execute(
        select(
            AppSession.user_id,
            AppSession.platform,
            last_seen,
            session_count,
        )
        .where(
            AppSession.user_id.in_(user_ids),
            AppSession.platform.is_not(None),
            AppSession.platform != "",
        )
        .group_by(AppSession.user_id, AppSession.platform)
        .order_by(
            AppSession.user_id,
            last_seen.desc(),
            AppSession.platform.asc(),
        )
    ).all()

    platforms: dict[UUID, list[UserPlatform]] = {}
    for row in rows:
        if not row.platform or not row.platform.strip():
            continue
        platforms.setdefault(row.user_id, []).append(
            UserPlatform(
                name=row.platform,
                family=normalize_platform(row.platform),
                last_seen_at=row.last_seen_at,
                sessions=int(row.sessions or 0),
            )
        )
    return platforms


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
    # Accounts flagged "excluded" are friends, admin and test players: they are
    # left out of every counter below so the numbers describe real users.
    included = _included_user_ids()

    users_total = (
        db.scalar(
            select(func.count(User.id)).where(User.statistics_excluded.is_(False))
        )
        or 0
    )
    active_24h = _active_users_since(since_24h)
    users_active_24h = (
        db.scalar(
            select(func.count())
            .select_from(active_24h)
            .where(active_24h.c.user_id.in_(included))
        )
        or 0
    )
    active_7d = _active_users_since(since_7d)
    users_active_7d = (
        db.scalar(
            select(func.count())
            .select_from(active_7d)
            .where(active_7d.c.user_id.in_(included))
        )
        or 0
    )
    sessions_total = (
        db.scalar(
            select(func.count(AppSession.id)).where(
                AppSession.started_at >= since,
                AppSession.user_id.in_(included),
            )
        )
        or 0
    )
    app_seconds_total = int(
        db.scalar(
            select(func.coalesce(func.sum(SESSION_SECONDS), 0)).where(
                AppSession.started_at >= since,
                AppSession.user_id.in_(included),
            )
        )
        or 0
    )
    launches_total = (
        db.scalar(
            select(func.count(GameLaunch.id)).where(
                GameLaunch.started_at >= since,
                GameLaunch.user_id.in_(included),
            )
        )
        or 0
    )
    launches_24h = (
        db.scalar(
            select(func.count(GameLaunch.id)).where(
                GameLaunch.started_at >= since_24h,
                GameLaunch.user_id.in_(included),
            )
        )
        or 0
    )

    # One grouped query for the whole top list: no per-country round trip.
    # Grouping by the code alone keeps a provider rename from splitting one
    # country into two rows.
    country_count = func.count(User.id).label("users")
    country_rows = db.execute(
        select(
            User.country_code,
            func.max(User.country_name).label("country_name"),
            country_count,
        )
        .where(
            User.country_code.is_not(None),
            User.statistics_excluded.is_(False),
        )
        .group_by(User.country_code)
        .order_by(country_count.desc(), User.country_code.asc())
        .limit(10)
    ).all()

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
        countries=[
            StatisticsCountryItem(
                code=row.country_code,
                name=row.country_name,
                users=int(row.users or 0),
            )
            for row in country_rows
        ],
    )


@router.get("/users", response_model=StatisticsUserList, summary="Per-user statistics")
def statistics_users(
    search: str | None = Query(default=None, min_length=1, max_length=100),
    sort: Literal[
        "last_seen",
        "username",
        "display_name",
        "email",
        "created_at",
        "app_seconds",
        "sessions_count",
        "launches_count",
        "scores_count",
        "level",
        "xp_total",
    ] = Query(default="last_seen", description="Column to sort by"),
    order: Literal["asc", "desc"] | None = Query(
        default=None,
        description="Sort direction; defaults to asc for names, desc for numbers and dates",
    ),
    limit: int = Query(default=50, ge=1, le=200),
    offset: int = Query(default=0, ge=0),
    country: str | None = Query(
        default=None,
        min_length=2,
        max_length=2,
        pattern=r"^[A-Za-z]{2}$",
        description="ISO 3166-1 alpha-2 country code of the registration (RU)",
    ),
    include: Literal["all", "included", "excluded"] = Query(
        default="all",
        description=(
            "Filter by the statistics flag: every account, only the ones counted "
            "in the aggregates, or only the excluded ones"
        ),
    ),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> StatisticsUserList:
    """One row per account with app time, session, launch and score aggregates.

    ``last_seen`` sorts by the newest of heartbeat and launch time, so players
    who only started a game still appear as recently active. Text columns are
    compared case-insensitively, empty values go last in both directions, and
    ``username``/``id`` break ties so paging is stable.

    ``platforms`` is filled by one grouped query for the users on the current
    page (see :func:`_platforms_by_user`), so it adds a single round trip no
    matter how many rows are returned. ``country`` filters on the country of
    registration and is a plain equality on the indexed ``country_code``.

    ``include`` filters on ``users.statistics_excluded``: ``all`` (default)
    lists every account so the flag can always be switched back, ``included``
    only the accounts that feed the aggregates, ``excluded`` only the ones that
    are left out. The filter is part of ``total``, sorting and paging.

    The six compact game-mode fields (``online_*``/``battle_*``) are additive:
    they are filled for the current page by one grouped query per mode group
    (see :mod:`app.services.admin_user_stats`), so older admin clients that
    ignore them keep working and the query count never depends on the page
    size.
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
    # Scores belong to a game session, so the user is reached through it.
    score_agg = (
        select(
            GameSession.user_id.label("user_id"),
            func.count(Score.id).label("scores_count"),
            func.max(Score.score_points).label("best_score"),
        )
        .join(Score, Score.session_id == GameSession.id)
        .group_by(GameSession.user_id)
        .subquery()
    )

    last_activity = func.greatest(
        session_agg.c.last_seen_at, launch_agg.c.last_launch_at
    )
    app_seconds = func.coalesce(session_agg.c.app_seconds_total, 0)
    sessions = func.coalesce(session_agg.c.sessions_count, 0)
    launches = func.coalesce(launch_agg.c.launches_count, 0)
    scores = func.coalesce(score_agg.c.scores_count, 0)

    text_sorts = {"username", "display_name", "email"}
    sort_columns = {
        "username": User.username,
        "display_name": User.display_name,
        "email": User.email,
        "created_at": User.created_at,
        "app_seconds": app_seconds,
        "sessions_count": sessions,
        "launches_count": launches,
        "scores_count": scores,
        "level": User.level,
        "xp_total": User.xp_total,
        "last_seen": last_activity,
    }
    column = sort_columns[sort]
    if sort in text_sorts:
        column = func.lower(column)

    direction = order or ("asc" if sort in text_sorts else "desc")
    primary = column.asc() if direction == "asc" else column.desc()
    ordering = [primary.nullslast()]

    # Stable tiebreaker: same-name accounts keep a deterministic order across
    # pages, and every other sort falls back to the login name.
    if sort != "username":
        ordering.append(func.lower(User.username).asc())
    ordering.append(User.id.asc())

    conditions = []
    if search:
        conditions.append(_search_filter(search.strip()))
    if country:
        # Stored upper-case; accept lower-case input from the admin URL.
        conditions.append(User.country_code == country.strip().upper())
    if include == "included":
        conditions.append(User.statistics_excluded.is_(False))
    elif include == "excluded":
        conditions.append(User.statistics_excluded.is_(True))

    stmt = (
        select(
            User.id.label("user_id"),
            User.username,
            User.display_name,
            User.email,
            User.is_active,
            User.created_at,
            User.country_code,
            User.country_name,
            User.registration_ip,
            User.last_login_ip,
            User.level,
            User.xp_total,
            User.statistics_excluded,
            app_seconds.label("app_seconds_total"),
            sessions.label("sessions_count"),
            session_agg.c.last_seen_at,
            launches.label("launches_count"),
            launch_agg.c.last_launch_at,
            func.coalesce(launch_agg.c.games_distinct, 0).label("games_distinct"),
            scores.label("scores_count"),
            score_agg.c.best_score,
        )
        .outerjoin(session_agg, session_agg.c.user_id == User.id)
        .outerjoin(launch_agg, launch_agg.c.user_id == User.id)
        .outerjoin(score_agg, score_agg.c.user_id == User.id)
        .where(*conditions)
        .order_by(*ordering)
        .limit(limit)
        .offset(offset)
    )
    rows = db.execute(stmt).all()

    total = db.scalar(select(func.count(User.id)).where(*conditions)) or 0

    # One extra query for the whole page: the platforms of every listed user.
    platforms_by_user = _platforms_by_user(db, [row.user_id for row in rows])

    # Two more grouped statements for the whole page: duel and tower summaries
    # (one query per group, never one per row).
    user_ids = [row.user_id for row in rows]
    online_by_user = online_summary_by_user(db, user_ids)
    battle_by_user = battle_summary_by_user(db, user_ids)

    items = []
    for row in rows:
        platforms = platforms_by_user.get(row.user_id, [])
        online = online_by_user.get(row.user_id, {})
        battle = battle_by_user.get(row.user_id, {})
        items.append(
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
                scores_count=int(row.scores_count or 0),
                best_score=int(row.best_score) if row.best_score is not None else None,
                level=int(row.level or 1),
                xp_total=int(row.xp_total or 0),
                platforms=platforms,
                last_platform=platforms[0].name if platforms else None,
                country_code=row.country_code,
                country_name=row.country_name,
                country_flag=country_flag(row.country_code),
                registration_ip=row.registration_ip,
                last_login_ip=row.last_login_ip,
                online_rating=online.get("online_rating"),
                online_matches=int(online.get("online_matches", 0)),
                online_wins=int(online.get("online_wins", 0)),
                online_losses=int(online.get("online_losses", 0)),
                battle_towers_completed=int(battle.get("battle_towers_completed", 0)),
                battle_stages_cleared=int(battle.get("battle_stages_cleared", 0)),
                statistics_excluded=bool(row.statistics_excluded),
            )
        )
    return StatisticsUserList(items=items, total=int(total), limit=limit, offset=offset)


@router.patch(
    "/users/{user_id}",
    response_model=StatisticsUserExclusion,
    summary="Include or exclude an account from the statistics",
)
def update_user_statistics_exclusion(
    user_id: UUID,
    payload: StatisticsUserExclusionUpdate,
    db: Session = Depends(get_db),
    current_user: User = Depends(require_admin),
) -> StatisticsUserExclusion:
    """Switch ``users.statistics_excluded`` for one account.

    Idempotent: sending the current value writes nothing and answers with the
    stored flag, so a double click or a retried request is harmless. The
    account, its telemetry and its public leaderboard place are untouched; only
    the aggregates of this tab skip it from now on.
    """
    user = db.get(User, user_id)
    if user is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="user_not_found"
        )

    if user.statistics_excluded != payload.statistics_excluded:
        user.statistics_excluded = payload.statistics_excluded
        db.commit()
        logger.info(
            "admin %s set statistics_excluded=%s for user %s",
            current_user.id,
            payload.statistics_excluded,
            user.id,
        )

    return StatisticsUserExclusion(
        user_id=user.id, statistics_excluded=bool(user.statistics_excluded)
    )


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

    Launches of accounts flagged ``statistics_excluded`` are skipped everywhere
    — the counters, the distinct players, the newest-song title and ``total`` —
    so the tab reports real players only.
    """
    since = _window_start(days)
    included = _included_user_ids()

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
        .where(
            GameLaunch.started_at >= since,
            GameLaunch.song_id.is_not(None),
            GameLaunch.user_id.in_(included),
        )
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
        .where(
            GameLaunch.started_at >= since,
            GameLaunch.user_id.in_(included),
        )
        .group_by(GameLaunch.game_key, Song.title)
        .order_by(launch_count.desc(), GameLaunch.game_key.asc())
        .limit(limit)
    )
    rows = db.execute(stmt).all()

    total = (
        db.scalar(
            select(func.count(func.distinct(GameLaunch.game_key))).where(
                GameLaunch.started_at >= since,
                GameLaunch.user_id.in_(included),
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
