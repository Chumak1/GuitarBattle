from datetime import datetime
from uuid import UUID
from pydantic import BaseModel, ConfigDict, Field

class GameSessionCreate(BaseModel):
    song_id: UUID
    speed_percent: int = Field(default=100, ge=50, le=120)

class GameSessionResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)
    id: UUID
    song_id: UUID
    song_version: int
    speed_percent: int
    started_at: datetime
    finished_at: datetime | None
    status: str

class ScoreSubmit(BaseModel):
    score_points: int = Field(ge=0, le=100_000_000)
    accuracy_percent: float = Field(ge=0, le=100)
    max_combo: int = Field(ge=0, le=1_000_000)
    perfect_count: int = Field(ge=0, le=1_000_000)
    good_count: int = Field(ge=0, le=1_000_000)
    miss_count: int = Field(ge=0, le=1_000_000)

class ScoreResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)
    id: UUID
    session_id: UUID
    score_points: int
    accuracy_percent: float
    max_combo: int
    perfect_count: int
    good_count: int
    miss_count: int
    created_at: datetime

class GameHistoryItem(BaseModel):
    session_id: UUID
    song_id: UUID
    song_title: str
    song_slug: str
    started_at: datetime
    finished_at: datetime | None
    status: str
    score_points: int | None
    accuracy_percent: float | None
    max_combo: int | None
