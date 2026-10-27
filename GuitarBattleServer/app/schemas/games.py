from __future__ import annotations

from datetime import datetime
from uuid import UUID
from pydantic import BaseModel, ConfigDict, Field, model_validator

from app.schemas.progression import (
    AchievementAwardPayload,
    MasteryPayload,
)

class GameSessionCreate(BaseModel):
    song_id: UUID
    speed_percent: int = Field(default=100, ge=50, le=120)
    #: ``True`` asks for the official daily-challenge attempt instead of a
    #: free-play session. The song must match the challenge of the current UTC
    #: day and the player must not have used the official attempt yet.
    daily: bool = False

class GameSessionResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)
    id: UUID
    song_id: UUID
    song_version: int
    speed_percent: int
    mode: str = "normal"
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
    #: How much of the chart the player actually finished. Older clients omit it
    #: and keep the historical behaviour of a full attempt.
    completion_percent: float = Field(default=100.0, ge=0, le=100)
    avg_timing_error_ms: float | None = Field(default=None, ge=0, le=2000)
    notes_total: int | None = Field(default=None, ge=0, le=1_000_000)
    notes_played: int | None = Field(default=None, ge=0, le=1_000_000)

    @model_validator(mode="after")
    def _clamp_completion_by_notes(self) -> ScoreSubmit:
        """Never trust completion above the fraction of notes actually played."""
        if (
            self.notes_total is not None
            and self.notes_played is not None
            and self.notes_played > self.notes_total
        ):
            raise ValueError("notes_played cannot exceed notes_total")
        if self.notes_total:
            played = min(self.notes_played or 0, self.notes_total)
            self.completion_percent = min(
                self.completion_percent, 100.0 * played / self.notes_total
            )
        return self

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
    completion_percent: float = 100.0
    avg_timing_error_ms: float | None = None
    notes_total: int | None = None
    notes_played: int | None = None
    created_at: datetime

class ScoreFinishResponse(ScoreResponse):
    """Score fields plus the progression block.

    The flat layout keeps the response backwards compatible: a client that only
    reads ``score_points``/``accuracy_percent``/... keeps working unchanged.
    """

    xp_earned: int = 0
    xp_total: int = 0
    level: int = 1
    level_up: bool = False
    xp_to_next: int = 0
    xp_remaining: int = 0
    personal_best_updated: bool = False
    mastery: MasteryPayload | None = None
    achievements_awarded: list[AchievementAwardPayload] = []

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


class SongBestScore(BaseModel):
    """The signed-in player's best result for one published song."""

    song_id: UUID
    song_slug: str
    song_title: str
    artist: str | None = None
    best_score: int = 0
    best_accuracy: float | None = None
    best_max_combo: int | None = None
    attempts: int = 0
    last_played_at: datetime | None = None


class PlayerRatingResponse(BaseModel):
    """Personal rating built from the best score on every published song."""

    rating_points: int
    games_played: int
    songs_played: int
    items: list[SongBestScore]
