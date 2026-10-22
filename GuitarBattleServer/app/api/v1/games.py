from datetime import datetime, timezone
from uuid import UUID
from fastapi import APIRouter, Depends, HTTPException, Query, status
from sqlalchemy import func, select
from sqlalchemy.orm import Session
from app.api.v1.auth import get_current_user
from app.database import get_db
from app.models.game_session import GameSession, Score
from app.models.song import Song
from app.models.user import User
from app.schemas.games import (
    GameHistoryItem,
    GameSessionCreate,
    GameSessionResponse,
    PlayerRatingResponse,
    ScoreResponse,
    ScoreSubmit,
    SongBestScore,
)

router = APIRouter(tags=["games"])


def _best_score_per_song_subquery():
    """One row per (user, published song) holding that player's best attempt.

    The global rating must not grow with every replay: only the best score a
    player has on a song contributes to the rating. Aggregating on read keeps
    every historical attempt in the database untouched.
    """
    return (
        select(
            GameSession.user_id.label("user_id"),
            GameSession.song_id.label("song_id"),
            func.max(Score.score_points).label("best_score"),
            func.max(Score.accuracy_percent).label("best_accuracy"),
        )
        .join(Score, Score.session_id == GameSession.id)
        .where(GameSession.status == "finished")
        .group_by(GameSession.user_id, GameSession.song_id)
        .subquery()
    )


@router.post("/games/sessions", response_model=GameSessionResponse, status_code=status.HTTP_201_CREATED)
def create_session(payload: GameSessionCreate, db: Session = Depends(get_db),
                   user: User = Depends(get_current_user)) -> GameSession:
    song = db.scalar(select(Song).where(Song.id == payload.song_id, Song.is_published.is_(True)))
    if song is None:
        raise HTTPException(status_code=404, detail="Published song not found")
    session = GameSession(user_id=user.id, song_id=song.id, song_version=song.version,
                          speed_percent=payload.speed_percent, started_at=datetime.now(timezone.utc),
                          status="started")
    db.add(session)
    db.commit()
    db.refresh(session)
    return session

@router.post("/games/sessions/{session_id}/finish", response_model=ScoreResponse)
def finish_session(session_id: UUID, payload: ScoreSubmit, db: Session = Depends(get_db),
                   user: User = Depends(get_current_user)) -> Score:
    session = db.scalar(select(GameSession).where(GameSession.id == session_id, GameSession.user_id == user.id))
    if session is None:
        raise HTTPException(status_code=404, detail="Game session not found")
    if session.status != "started":
        raise HTTPException(status_code=409, detail="Game session is already finished")
    if payload.perfect_count + payload.good_count + payload.miss_count > 1_000_000:
        raise HTTPException(status_code=422, detail="Invalid note counters")
    score = Score(session_id=session.id, **payload.model_dump())
    session.status = "finished"
    session.finished_at = datetime.now(timezone.utc)
    db.add(score)
    db.commit()
    db.refresh(score)
    return score

@router.get("/games/history", response_model=list[GameHistoryItem])
def game_history(limit: int = Query(default=20, ge=1, le=100), offset: int = Query(default=0, ge=0),
                 db: Session = Depends(get_db), user: User = Depends(get_current_user)):
    rows = db.execute(
        select(GameSession, Song.title, Song.slug, Score.score_points, Score.accuracy_percent, Score.max_combo)
        .join(Song, Song.id == GameSession.song_id)
        .outerjoin(Score, Score.session_id == GameSession.id)
        .where(GameSession.user_id == user.id)
        .order_by(GameSession.started_at.desc())
        .limit(limit).offset(offset)
    ).all()
    return [GameHistoryItem(session_id=s.id, song_id=s.song_id, song_title=title, song_slug=slug,
                            started_at=s.started_at, finished_at=s.finished_at, status=s.status,
                            score_points=points, accuracy_percent=accuracy, max_combo=combo)
            for s, title, slug, points, accuracy, combo in rows]

@router.get("/games/best-scores", response_model=PlayerRatingResponse)
def player_best_scores(db: Session = Depends(get_db),
                       user: User = Depends(get_current_user)) -> PlayerRatingResponse:
    """The signed-in player's best result for every published song.

    The client uses this to show a personal best next to each catalog entry.
    ``rating_points`` uses the same rule as the global leaderboard: one best
    score per song, summed across songs.
    """
    best_attempt = (
        select(
            GameSession.song_id.label("song_id"),
            Score.score_points.label("best_score"),
            Score.accuracy_percent.label("best_accuracy"),
            Score.max_combo.label("best_max_combo"),
        )
        .join(Score, Score.session_id == GameSession.id)
        .where(GameSession.user_id == user.id, GameSession.status == "finished")
        .distinct(GameSession.song_id)
        .order_by(
            GameSession.song_id,
            Score.score_points.desc(),
            Score.accuracy_percent.desc(),
            Score.created_at.asc(),
        )
        .subquery()
    )
    attempt_stats = (
        select(
            GameSession.song_id.label("song_id"),
            func.count(Score.id).label("attempts"),
            func.max(Score.created_at).label("last_played_at"),
        )
        .join(Score, Score.session_id == GameSession.id)
        .where(GameSession.user_id == user.id, GameSession.status == "finished")
        .group_by(GameSession.song_id)
        .subquery()
    )

    rows = db.execute(
        select(
            Song.id,
            Song.slug,
            Song.title,
            Song.artist,
            best_attempt.c.best_score,
            best_attempt.c.best_accuracy,
            best_attempt.c.best_max_combo,
            attempt_stats.c.attempts,
            attempt_stats.c.last_played_at,
        )
        .join(best_attempt, best_attempt.c.song_id == Song.id)
        .join(attempt_stats, attempt_stats.c.song_id == Song.id)
        .where(Song.is_published.is_(True))
        .order_by(best_attempt.c.best_score.desc(), Song.title.asc())
    ).all()

    items = [
        SongBestScore(
            song_id=row.id,
            song_slug=row.slug,
            song_title=row.title,
            artist=row.artist,
            best_score=int(row.best_score or 0),
            best_accuracy=float(row.best_accuracy)
            if row.best_accuracy is not None
            else None,
            best_max_combo=int(row.best_max_combo)
            if row.best_max_combo is not None
            else None,
            attempts=int(row.attempts or 0),
            last_played_at=row.last_played_at,
        )
        for row in rows
    ]
    return PlayerRatingResponse(
        rating_points=sum(item.best_score for item in items),
        games_played=sum(item.attempts for item in items),
        songs_played=len(items),
        items=items,
    )


@router.get("/leaderboard/global")
def global_leaderboard(limit: int = Query(default=10, ge=1, le=100),
                       offset: int = Query(default=0, ge=0),
                       db: Session = Depends(get_db)):
    """Global player rating: sum of each player's best score per song.

    Replays and weaker attempts never raise the rating. ``games_played`` counts
    the songs that contributed a best score, not the raw number of attempts.
    """
    best_scores = _best_score_per_song_subquery()
    rating_points = func.coalesce(func.sum(best_scores.c.best_score), 0).label(
        "rating_points"
    )
    songs_counted = func.count(best_scores.c.song_id).label("games_played")

    stmt = (
        select(
            User.id.label("user_id"),
            User.username,
            User.display_name,
            rating_points,
            songs_counted,
            func.max(best_scores.c.best_accuracy).label("best_accuracy"),
        )
        .join(best_scores, best_scores.c.user_id == User.id)
        .join(Song, Song.id == best_scores.c.song_id)
        .where(
            User.is_active.is_(True),
            Song.is_published.is_(True),
        )
        .group_by(User.id, User.username, User.display_name)
        .order_by(rating_points.desc(),
                  songs_counted.desc(),
                  User.username.asc())
        .limit(limit)
        .offset(offset)
    )
    rows = db.execute(stmt).all()
    return {
        "items": [
            {
                "rank": offset + index + 1,
                "username": row.username,
                "display_name": row.display_name,
                "rating_points": int(row.rating_points or 0),
                "games_played": int(row.games_played or 0),
                "best_accuracy": float(row.best_accuracy or 0),
            }
            for index, row in enumerate(rows)
        ],
        "limit": limit,
        "offset": offset,
    }


@router.get("/leaderboard")
def leaderboard(song_slug: str | None = None, limit: int = Query(default=20, ge=1, le=100),
                offset: int = Query(default=0, ge=0), db: Session = Depends(get_db)):
    stmt = (select(Score.score_points, Score.accuracy_percent, Score.max_combo, Score.created_at,
                   User.username, User.display_name, Song.title.label("song_title"), Song.slug.label("song_slug"))
            .join(GameSession, GameSession.id == Score.session_id)
            .join(User, User.id == GameSession.user_id)
            .join(Song, Song.id == GameSession.song_id)
            .where(GameSession.status == "finished", User.is_active.is_(True), Song.is_published.is_(True)))
    if song_slug:
        stmt = stmt.where(Song.slug == song_slug)
    rows = db.execute(stmt.order_by(Score.score_points.desc(), Score.accuracy_percent.desc(),
                                    Score.created_at.asc()).limit(limit).offset(offset)).all()
    return {"items": [{"rank": offset + i + 1, "username": r.username,
                       "display_name": r.display_name, "song_title": r.song_title,
                       "song_slug": r.song_slug, "score_points": r.score_points,
                       "accuracy_percent": r.accuracy_percent, "max_combo": r.max_combo,
                       "created_at": r.created_at} for i, r in enumerate(rows)],
            "limit": limit, "offset": offset}
