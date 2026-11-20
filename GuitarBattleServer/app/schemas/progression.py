"""Response schemas for the progression, achievements and daily challenge API."""
from __future__ import annotations

from datetime import date, datetime
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field


class MasteryPayload(BaseModel):
    """Mastery of one song: accuracy leaning plus a difficulty bonus."""

    tier: str
    value: float
    next_threshold: float | None = None


class AchievementAwardPayload(BaseModel):
    code: str
    title: str
    xp_bonus: int


class PublicAchievementItem(BaseModel):
    code: str
    title: str
    description: str
    category: str
    xp_bonus: int
    #: Icon file name in the bundled catalog (kept for older clients).
    icon: str | None = None
    #: Ready-to-use URL of the icon, e.g. ``/media/achievements/star.svg``.
    icon_url: str | None = None
    awarded_at: datetime


class PublicProfileResponse(BaseModel):
    """Public card shown when an admin/user opens a leaderboard row."""

    user_id: UUID
    username: str
    display_name: str
    #: Additive: origin-relative avatar URL, ``null`` when unset.
    avatar_image_url: str | None = None
    level: int
    xp_total: int
    songs_mastered: int
    achievements_unlocked: int
    achievements: list[PublicAchievementItem] = []


class ProgressionPayload(BaseModel):
    """The ``progression`` block returned by ``POST /games/sessions/{id}/finish``."""

    xp_earned: int
    xp_total: int
    level: int
    level_up: bool
    xp_to_next: int
    xp_remaining: int
    personal_best_updated: bool
    mastery: MasteryPayload
    achievements_awarded: list[AchievementAwardPayload] = []


class PersonalBestItem(BaseModel):
    song_id: UUID
    song_slug: str
    song_title: str
    artist: str | None = None
    best_score: int
    best_accuracy: float
    best_max_combo: int
    best_avg_timing_error_ms: float | None = None
    attempts: int
    mastery: MasteryPayload
    updated_at: datetime


class PersonalBestPage(BaseModel):
    items: list[PersonalBestItem]
    total: int
    limit: int
    offset: int


class MasteryBySongItem(BaseModel):
    song_id: UUID
    song_slug: str
    song_title: str
    genre: str
    difficulty: int
    tier: str
    value: float
    next_threshold: float | None = None
    best_accuracy: float


class GenreMasteryItem(BaseModel):
    genre: str
    value: float
    songs: int


class PlayerStatsPayload(BaseModel):
    accuracy: float
    timing: float | None = None
    speed: float
    consistency: float
    versatility: float
    samples: int


class ProfileProgressionResponse(BaseModel):
    level: int
    xp_total: int
    xp_into_level: int
    xp_for_next: int
    xp_remaining: int
    songs_mastered: int
    achievements_unlocked: int
    achievements_total: int
    personal_bests: list[PersonalBestItem]
    mastery_by_song: list[MasteryBySongItem]
    genre_mastery: list[GenreMasteryItem]
    stats: PlayerStatsPayload


class AchievementItem(BaseModel):
    code: str
    title: str
    description: str
    category: str
    xp_bonus: int
    goal: int | None = None
    progress: int
    unlocked: bool
    awarded_at: datetime | None = None
    #: Icon file name in the bundled catalog (kept for older clients).
    icon: str | None = None
    #: Ready-to-use URL of the icon, e.g. ``/media/achievements/star.svg``.
    icon_url: str | None = None


class AchievementCatalogResponse(BaseModel):
    items: list[AchievementItem]
    total: int
    unlocked: int


class DailySongInfo(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: UUID
    slug: str
    title: str
    artist: str | None = None
    genre: str
    bpm: int
    difficulty: int
    duration_seconds: int
    version: int


class DailyResultItem(BaseModel):
    date: date
    score_points: int
    accuracy_percent: float
    avg_timing_error_ms: float | None = None
    rank: int | None = None


class DailyLeaderboardEntry(BaseModel):
    rank: int
    user_id: UUID
    username: str
    display_name: str
    song_slug: str
    song_title: str
    score_points: int
    accuracy_percent: float
    avg_timing_error_ms: float | None = None
    created_at: datetime


class DailyTodayResponse(BaseModel):
    date: date
    #: 0 = Monday ... 6 = Sunday (UTC), the day the challenge belongs to.
    weekday: int = 0
    #: ``None`` when no published song is scheduled for this weekday.
    song: DailySongInfo | None = None
    rules_version: int = 1
    official_attempt_used: bool = False
    official_result: DailyResultItem | None = None
    leaderboard_top: list[DailyLeaderboardEntry] = []


class DailyLeaderboardResponse(BaseModel):
    date: date
    items: list[DailyLeaderboardEntry]
    total: int = Field(default=0)


class DailyHistoryItem(BaseModel):
    """One official daily attempt of the signed-in player."""

    date: date
    song_id: UUID
    song_slug: str
    song_title: str
    artist: str | None = None
    score_points: int
    accuracy_percent: float
    avg_timing_error_ms: float | None = None
    #: Place among all official results of that day (1 = best).
    rank: int
    #: How many players submitted an official result that day.
    participants: int
    created_at: datetime


class DailyHistoryResponse(BaseModel):
    items: list[DailyHistoryItem]
    total: int
    limit: int
    offset: int

