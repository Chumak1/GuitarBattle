from uuid import UUID
from fastapi import APIRouter, Depends, HTTPException, Query, status
from sqlalchemy import func, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session
from app.api.v1.auth import get_current_user
from app.database import get_db
from app.models.competition import Competition, CompetitionParticipant
from app.models.song import Song
from app.models.user import User
from app.schemas.competitions import CompetitionCreate, CompetitionListResponse, CompetitionResponse

router = APIRouter(prefix="/competitions", tags=["competitions"])

@router.get("", response_model=CompetitionListResponse)
def list_competitions(limit: int = Query(default=20, ge=1, le=100), offset: int = Query(default=0, ge=0),
                      db: Session = Depends(get_db)):
    filters = [Competition.status == "open"]
    total = db.scalar(select(func.count()).select_from(Competition).where(*filters)) or 0
    items = db.scalars(select(Competition).where(*filters).order_by(Competition.created_at.desc())
                       .limit(limit).offset(offset)).all()
    return CompetitionListResponse(items=items, total=total, limit=limit, offset=offset)

@router.post("", response_model=CompetitionResponse, status_code=status.HTTP_201_CREATED)
def create_competition(payload: CompetitionCreate, db: Session = Depends(get_db),
                       user: User = Depends(get_current_user)):
    song = db.scalar(select(Song).where(Song.id == payload.song_id, Song.is_published.is_(True)))
    if song is None:
        raise HTTPException(status_code=404, detail="Published song not found")
    competition = Competition(name=payload.name.strip(), description=payload.description,
                              song_id=song.id, created_by=user.id, max_players=payload.max_players,
                              status="open")
    db.add(competition)
    db.commit()
    db.refresh(competition)
    return competition

@router.post("/{competition_id}/join", status_code=status.HTTP_201_CREATED)
def join_competition(competition_id: UUID, db: Session = Depends(get_db),
                     user: User = Depends(get_current_user)):
    competition = db.scalar(select(Competition).where(Competition.id == competition_id))
    if competition is None or competition.status != "open":
        raise HTTPException(status_code=404, detail="Open competition not found")
    existing = db.scalar(select(CompetitionParticipant.id).where(
        CompetitionParticipant.competition_id == competition_id,
        CompetitionParticipant.user_id == user.id))
    if existing:
        return {"competition_id": str(competition_id), "joined": True, "already_joined": True}
    count = db.scalar(select(func.count()).select_from(CompetitionParticipant).where(
        CompetitionParticipant.competition_id == competition_id)) or 0
    if count >= competition.max_players:
        raise HTTPException(status_code=409, detail="Competition is full")
    db.add(CompetitionParticipant(competition_id=competition_id, user_id=user.id))
    try:
        db.commit()
    except IntegrityError:
        db.rollback()
        return {"competition_id": str(competition_id), "joined": True, "already_joined": True}
    return {"competition_id": str(competition_id), "joined": True, "already_joined": False}

@router.get("/{competition_id}/participants")
def competition_participants(competition_id: UUID, db: Session = Depends(get_db)):
    competition = db.get(Competition, competition_id)
    if competition is None:
        raise HTTPException(status_code=404, detail="Competition not found")
    rows = db.execute(select(User.username, User.display_name, CompetitionParticipant.joined_at)
                      .join(CompetitionParticipant, CompetitionParticipant.user_id == User.id)
                      .where(CompetitionParticipant.competition_id == competition_id)
                      .order_by(CompetitionParticipant.joined_at.asc())).all()
    return {"competition_id": str(competition_id),
            "items": [{"username": r.username, "display_name": r.display_name, "joined_at": r.joined_at}
                      for r in rows]}
