from datetime import datetime
from uuid import UUID
from pydantic import BaseModel, ConfigDict, Field

class CompetitionCreate(BaseModel):
    name: str = Field(min_length=3, max_length=120)
    description: str | None = Field(default=None, max_length=1000)
    song_id: UUID
    max_players: int = Field(default=8, ge=2, le=64)

class CompetitionResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)
    id: UUID
    name: str
    description: str | None
    song_id: UUID
    created_by: UUID
    status: str
    max_players: int
    created_at: datetime

class CompetitionListResponse(BaseModel):
    items: list[CompetitionResponse]
    total: int
    limit: int
    offset: int
