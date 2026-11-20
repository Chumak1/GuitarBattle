"""Progression, achievements and daily challenge endpoints."""
from __future__ import annotations

from datetime import date
from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query
from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app.api.v1.auth import get_current_user
from app.database import get_db
from app.models.progression import Achievement, SongBestScore, UserAchievement
from app.models.song import Song
from app.models.user import User
from app.schemas.progression import (
    PublicAchievementItem,
    PublicProfileResponse,
    AchievementCatalogResponse,
    AchievementItem,
    DailyHistoryItem,
    DailyHistoryResponse,
    DailyLeaderboardEntry,
    DailyLeaderboardResponse,
    DailyResultItem,
    DailySongInfo,
    DailyTodayResponse,
    GenreMasteryItem,
    MasteryBySongItem,
    PersonalBestItem,
    PersonalBestPage,
    PlayerStatsPayload,
    ProfileProgressionResponse,
)
from app.services import progression
from app.services.achievement_catalog import catalog_rows

router = APIRouter(tags=["progression"])

#: Where ``app.main`` mounts the bundled achievement SVG files.
ACHIEVEMENT_ICON_URL_PREFIX = "/media/achievements"


def achievement_icon_url(icon: str | None) -> str | None:
    """Public URL of a bundled achievement icon, or ``None`` without an icon."""
    if not icon:
        return None
    return f"{ACHIEVEMENT_ICON_URL_PREFIX}/{icon}"


@router.get(
    "/profile/users/{user_id}",
    response_model=PublicProfileResponse,
    summary="Public player card: level, XP and achievements",
)
def public_profile(
    user_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(get_current_user),
) -> PublicProfileResponse:
    target = db.scalar(
        select(User).where(User.id == user_id, User.is_active.is_(True))
    )
    if target is None:
        raise HTTPException(status_code=404, detail="User not found")

    rows = db.execute(
        select(
            Achievement.code,
            Achievement.title,
            Achievement.description,
            Achievement.category,
            Achievement.xp_bonus,
            Achievement.icon,
            UserAchievement.awarded_at,
        )
        .join(UserAchievement, UserAchievement.achievement_code == Achievement.code)
        .where(UserAchievement.user_id == user_id)
        .order_by(UserAchievement.awarded_at.desc())
    ).all()

    songs_mastered = db.scalar(
        select(func.count()).select_from(SongBestScore).where(
            SongBestScore.user_id == user_id
        )
    ) or 0

    return PublicProfileResponse(
        user_id=target.id,
        username=target.username,
        display_name=target.display_name or target.username,
        avatar_image_url=target.avatar_image_url,
        level=target.level,
        xp_total=target.xp_total,
        songs_mastered=int(songs_mastered),
        achievements_unlocked=len(rows),
        achievements=[
            PublicAchievementItem(
                code=row.code,
                title=row.title,
                description=row.description,
                category=row.category,
                xp_bonus=row.xp_bonus,
                icon=row.icon,
                icon_url=achievement_icon_url(row.icon),
                awarded_at=row.awarded_at,
            )
            for row in rows
        ],
    )


def _personal_best_payload(best: SongBestScore, song: Song) -> PersonalBestItem:
    return PersonalBestItem(
        song_id=song.id,
        song_slug=song.slug,
        song_title=song.title,
        artist=song.artist,
        best_score=int(best.best_score),
        best_accuracy=float(best.best_accuracy),
        best_max_combo=int(best.best_max_combo),
        best_avg_timing_error_ms=best.best_avg_timing_error_ms,
        attempts=int(best.attempts),
        mastery=progression.mastery_payload(best.best_accuracy, song.difficulty),
        updated_at=best.updated_at,
    )


@router.get("/profile/progression", response_model=ProfileProgressionResponse)
def profile_progression(
    db: Session = Depends(get_db), user: User = Depends(get_current_user)
) -> ProfileProgressionResponse:
    """XP/level, mastery per song and per genre, and the play-style statistics.

    Also the place where deferred awards (first place of a finished daily
    challenge) are granted, because this deployment has no background worker.
    """
    progression.award_deferred_daily_first_place(db, user)

    snapshot = progression.build_snapshot(db, user.id)
    level_info = progression.level_progress(int(user.xp_total or 0))
    achievements_total = int(
        db.scalar(
            select(func.count(Achievement.code)).where(Achievement.is_active.is_(True))
        )
        or 0
    )
    achievements_unlocked = int(
        db.scalar(
            select(func.count(UserAchievement.id)).where(
                UserAchievement.user_id == user.id
            )
        )
        or 0
    )

    mastery_by_song = [
        MasteryBySongItem(
            song_id=song.id,
            song_slug=song.slug,
            song_title=song.title,
            genre=song.genre,
            difficulty=int(song.difficulty),
            **progression.mastery_payload(best.best_accuracy, song.difficulty),
            best_accuracy=float(best.best_accuracy),
        )
        for best, song in snapshot.personal_bests
    ]

    return ProfileProgressionResponse(
        **level_info,
        songs_mastered=snapshot.pb_songs,
        achievements_unlocked=achievements_unlocked,
        achievements_total=achievements_total,
        personal_bests=[
            _personal_best_payload(best, song)
            for best, song in snapshot.personal_bests[:10]
        ],
        mastery_by_song=mastery_by_song,
        genre_mastery=[
            GenreMasteryItem(**row) for row in snapshot.genre_mastery
        ],
        stats=PlayerStatsPayload(**progression.player_stats(db, user.id)),
    )


@router.get("/achievements", response_model=AchievementCatalogResponse)
def achievements_catalog(
    db: Session = Depends(get_db), user: User = Depends(get_current_user)
) -> AchievementCatalogResponse:
    """Full catalog plus this player's progress and unlock state."""
    progression.award_deferred_daily_first_place(db, user)

    snapshot = progression.build_snapshot(db, user.id)
    progress_by_code = progression.achievement_progress(db, user.id, snapshot)
    awarded = {
        row.achievement_code: row
        for row in db.scalars(
            select(UserAchievement).where(UserAchievement.user_id == user.id)
        ).all()
    }

    items: list[AchievementItem] = []
    for achievement in catalog_rows(db):
        note = progression.genre_achievement_note(
            achievement.code, achievement.goal, snapshot.total_published_genres
        )
        unlocked_row = awarded.get(achievement.code)
        unlocked = unlocked_row is not None
        # Live progress first; an unlocked achievement reports at least the
        # progress it was granted with (the awarding attempt is not replayed).
        progress = max(
            int(progress_by_code.get(achievement.code, 0)),
            int(unlocked_row.progress) if unlocked_row is not None else 0,
            1 if unlocked else 0,
        )
        items.append(
            AchievementItem(
                code=achievement.code,
                title=achievement.title,
                description=achievement.description + (note or ""),
                category=achievement.category,
                xp_bonus=int(achievement.xp_bonus),
                goal=achievement.goal,
                progress=progress,
                unlocked=unlocked,
                awarded_at=unlocked_row.awarded_at if unlocked_row else None,
                icon=achievement.icon,
                icon_url=achievement_icon_url(achievement.icon),
            )
        )
    return AchievementCatalogResponse(
        items=items, total=len(items), unlocked=len(awarded)
    )


@router.get("/profile/personal-bests", response_model=PersonalBestPage)
def personal_bests(
    limit: int = Query(default=10, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> PersonalBestPage:
    """Personal bests of every song the player has finished, newest first.

    Hidden songs stay in the list: their results, mastery and rating are never
    thrown away when a song leaves the catalogue.
    """
    total = int(
        db.scalar(
            select(func.count(SongBestScore.id)).where(
                SongBestScore.user_id == user.id
            )
        )
        or 0
    )
    rows = progression.load_personal_bests(db, user.id, limit=limit, offset=offset)
    return PersonalBestPage(
        items=[_personal_best_payload(best, song) for best, song in rows],
        total=total,
        limit=limit,
        offset=offset,
    )


def _daily_song_payload(song: Song) -> DailySongInfo:
    return DailySongInfo.model_validate(song)


@router.get("/daily/today", response_model=DailyTodayResponse)
def daily_today(
    db: Session = Depends(get_db), user: User = Depends(get_current_user)
) -> DailyTodayResponse:
    """Today's challenge (UTC by weekday schedule), official attempt and top 5.

    Every weekday has its own scheduled song. A day without a published song in
    its slot is a valid answer with ``song: null`` — the client shows "no daily
    challenge today" instead of an error.
    """
    today = progression.utc_today()
    weekday = today.weekday()
    challenge = progression.ensure_daily_challenge(db, today)
    if challenge is None:
        return DailyTodayResponse(
            date=today,
            weekday=weekday,
            song=None,
            rules_version=progression.DAILY_RULES_VERSION,
            official_attempt_used=False,
            official_result=None,
            leaderboard_top=[],
        )
    db.commit()

    song = db.get(Song, challenge.song_id)
    if song is None or song.archived_at is not None:
        # The song of a frozen day was archived afterwards. Its history (and the
        # results already submitted) stay in the database, but the assets are no
        # longer public, so today's card reports "no daily challenge".
        return DailyTodayResponse(
            date=today,
            weekday=weekday,
            song=None,
            rules_version=progression.DAILY_RULES_VERSION,
            official_attempt_used=False,
            official_result=None,
            leaderboard_top=[],
        )

    board = progression.daily_leaderboard(db, today)
    official = next(
        (row for row in board if row["user_id"] == user.id), None
    )
    result = None
    if official is not None:
        result = DailyResultItem(
            date=today,
            score_points=official["score_points"],
            accuracy_percent=official["accuracy_percent"],
            avg_timing_error_ms=official["avg_timing_error_ms"],
            rank=official["rank"],
        )

    return DailyTodayResponse(
        date=today,
        weekday=weekday,
        song=_daily_song_payload(song),
        rules_version=int(challenge.rules_version),
        official_attempt_used=official is not None,
        official_result=result,
        leaderboard_top=[DailyLeaderboardEntry(**row) for row in board[:5]],
    )


@router.get("/daily/history", response_model=DailyHistoryResponse)
def daily_history(
    limit: int = Query(default=20, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DailyHistoryResponse:
    """The player's official daily attempts, newest first, with the day rank."""
    items, total = progression.daily_history(
        db, user.id, limit=limit, offset=offset
    )
    return DailyHistoryResponse(
        items=[DailyHistoryItem(**item) for item in items],
        total=total,
        limit=limit,
        offset=offset,
    )


@router.get("/daily/leaderboard", response_model=DailyLeaderboardResponse)
def daily_leaderboard(
    date: date | None = Query(default=None, description="UTC day, defaults to today"),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> DailyLeaderboardResponse:
    """Official results of one day: score desc, then accuracy desc."""
    progression.award_deferred_daily_first_place(db, user)
    day = date or progression.utc_today()
    rows = progression.daily_leaderboard(db, day)
    return DailyLeaderboardResponse(
        date=day,
        items=[DailyLeaderboardEntry(**row) for row in rows],
        total=len(rows),
    )
