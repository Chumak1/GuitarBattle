"""Pydantic schemas for the Guitar Skills rating.

The public leaderboard keeps the legacy fields (``rating_points``,
``games_played``, ``best_accuracy``, ``achievements_unlocked``) so an older
client can still render the old columns, and adds the new rating fields. The
admin endpoints expose the single tuning row, its append-only history, a
recalculation trigger and a status block.
"""
from __future__ import annotations

from datetime import datetime
from typing import Any
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, model_validator


class GuitarSkillsComponent(BaseModel):
    """One weighted component of the rating (all values ``0..100``)."""

    value: float
    events: int = 0
    confidence: float = 0.0


class GuitarSkillsComponents(BaseModel):
    accuracy: GuitarSkillsComponent
    challenge: GuitarSkillsComponent
    score: GuitarSkillsComponent
    duel: GuitarSkillsComponent
    progression: GuitarSkillsComponent


class GuitarSkillsLeaderboardItem(BaseModel):
    """One row of the public global leaderboard."""

    rank: int
    user_id: UUID
    username: str
    display_name: str
    #: Additive: origin-relative avatar URL, ``null`` when unset.
    avatar_image_url: str | None = None
    level: int = 1
    xp_total: int = 0

    #: The new competitive rating.
    guitar_skills: float
    accuracy_score: float
    challenge_score: float
    score_performance: float
    duel_performance: float
    progression_score: float
    confidence: float = 0.0
    provisional: bool = True
    accuracy_percent: float = 0.0

    duel_wins: int = 0
    duel_losses: int = 0
    duel_draws: int = 0
    duel_win_rate: float = 0.0
    duel_matches: int = 0

    position_change: int | None = None
    events_counted: int = 0

    #: Legacy "sum of best scores per song" values, kept for older clients.
    rating_points: int = 0
    games_played: int = 0
    best_accuracy: float = 0.0
    achievements_unlocked: int = 0


class GuitarSkillsLeaderboardResponse(BaseModel):
    items: list[GuitarSkillsLeaderboardItem]
    limit: int
    offset: int
    total: int
    calculated_at: datetime | None = None


class GuitarSkillsPlayerDetail(GuitarSkillsLeaderboardItem):
    """Public detail of one player, including the component breakdown."""

    last_calculated_at: datetime | None = None
    calculation_version: int = 1
    components: GuitarSkillsComponents


class GuitarSkillsConfigResponse(BaseModel):
    """The single tuning row (same keys are stored in the history snapshot)."""

    id: int = 1
    accuracy_weight: float
    challenge_weight: float
    score_weight: float
    duel_weight: float
    progression_weight: float
    window_size: int
    recency_decay: float
    difficulty_easy_max: int
    difficulty_normal_max: int
    difficulty_easy_factor: float
    difficulty_normal_factor: float
    difficulty_hard_factor: float
    note_factor_min: float
    note_factor_max: float
    median_note_count: int | None = None
    level_normalization_max: int
    xp_reference: int
    duel_strength_guitar_weight: float
    duel_strength_level_weight: float
    expected_score_scale: float
    reliability_constant: float
    per_song_attempt_cap: float
    confidence_expected_events: int
    confidence_expected_duels: int
    provisional_threshold: float
    min_ranked_events: int
    calculation_version: int
    updated_at: datetime | None = None
    updated_by: UUID | None = None


class GuitarSkillsConfigUpdate(BaseModel):
    """Partial update; every omitted field keeps its current value."""

    model_config = ConfigDict(extra="forbid")

    recalculate: bool = True

    accuracy_weight: float | None = Field(default=None, ge=0.0, le=1.0)
    challenge_weight: float | None = Field(default=None, ge=0.0, le=1.0)
    score_weight: float | None = Field(default=None, ge=0.0, le=1.0)
    duel_weight: float | None = Field(default=None, ge=0.0, le=1.0)
    progression_weight: float | None = Field(default=None, ge=0.0, le=1.0)
    window_size: int | None = Field(default=None, ge=1, le=200)
    recency_decay: float | None = Field(default=None, gt=0.0, le=100.0)
    difficulty_easy_max: int | None = Field(default=None, ge=1, le=10)
    difficulty_normal_max: int | None = Field(default=None, ge=1, le=10)
    difficulty_easy_factor: float | None = Field(default=None, ge=0.0, le=2.0)
    difficulty_normal_factor: float | None = Field(default=None, ge=0.0, le=2.0)
    difficulty_hard_factor: float | None = Field(default=None, ge=0.0, le=2.0)
    note_factor_min: float | None = Field(default=None, ge=0.0, le=2.0)
    note_factor_max: float | None = Field(default=None, ge=0.0, le=2.0)
    median_note_count: int | None = Field(default=None, ge=1)
    level_normalization_max: int | None = Field(default=None, ge=1, le=1000)
    xp_reference: int | None = Field(default=None, ge=1)
    duel_strength_guitar_weight: float | None = Field(default=None, ge=0.0, le=1.0)
    duel_strength_level_weight: float | None = Field(default=None, ge=0.0, le=1.0)
    expected_score_scale: float | None = Field(default=None, gt=0.0, le=1000.0)
    reliability_constant: float | None = Field(default=None, ge=0.0, le=1000.0)
    per_song_attempt_cap: float | None = Field(default=None, ge=0.0, le=100.0)
    confidence_expected_events: int | None = Field(default=None, ge=1, le=1000)
    confidence_expected_duels: int | None = Field(default=None, ge=1, le=1000)
    provisional_threshold: float | None = Field(default=None, ge=0.0, le=1.0)
    min_ranked_events: int | None = Field(default=None, ge=0, le=1000)

    @model_validator(mode="after")
    def _check_tiers(self) -> "GuitarSkillsConfigUpdate":
        easy = self.difficulty_easy_max
        normal = self.difficulty_normal_max
        if easy is not None and normal is not None and normal < easy:
            raise ValueError("difficulty_normal_max must be >= difficulty_easy_max")
        if (
            self.note_factor_min is not None
            and self.note_factor_max is not None
            and self.note_factor_max < self.note_factor_min
        ):
            raise ValueError("note_factor_max must be >= note_factor_min")
        return self


class GuitarSkillsConfigHistoryItem(BaseModel):
    id: int
    config: dict[str, Any]
    admin_user_id: UUID | None = None
    created_at: datetime


class GuitarSkillsConfigHistoryResponse(BaseModel):
    items: list[GuitarSkillsConfigHistoryItem]


class GuitarSkillsChangeItem(BaseModel):
    user_id: UUID
    username: str
    old_skills: float | None = None
    new_skills: float
    old_rank: int | None = None
    new_rank: int
    delta: float


class GuitarSkillsRecalculateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    dry_run: bool = False
    reason: str = Field(default="manual", min_length=1, max_length=64)


class GuitarSkillsRecalculateResponse(BaseModel):
    users: int
    changed: int
    items: list[GuitarSkillsChangeItem]
    started: bool = False


class GuitarSkillsStatusResponse(BaseModel):
    running: bool = False
    last_calculated_at: datetime | None = None
    calculation_version: int = 1
    users: int = 0
    provisional: int = 0
    incomplete_history: int = 0
