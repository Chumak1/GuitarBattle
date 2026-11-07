"""Request/response schemas for Guitar Duels (``/api/v1/duels``)."""
from __future__ import annotations

from datetime import datetime
from typing import Any, Literal
from uuid import UUID

from pydantic import BaseModel, Field, model_validator

DuelMode = Literal["bot", "online"]
DuelBotLevel = Literal["easy", "normal", "hard"]
DuelQualityToken = Literal["perfect", "great", "good", "miss"]


class DuelCreate(BaseModel):
    song_id: UUID
    mode: DuelMode = "bot"
    #: Required for ``mode=bot``, ignored for ``mode=online``.
    bot_level: DuelBotLevel | None = None

    @model_validator(mode="after")
    def _bot_level_matches_mode(self) -> "DuelCreate":
        if self.mode == "bot" and self.bot_level is None:
            raise ValueError("bot_level is required for a bot match")
        if self.mode == "online":
            self.bot_level = None
        return self


class DuelEventIn(BaseModel):
    note_index: int = Field(ge=0, le=1_000_000)
    quality: DuelQualityToken
    #: Optional client clock, milliseconds since the match start. It is stored
    #: for diagnostics; the server validates the receipt time itself.
    client_time_ms: int | None = Field(default=None, ge=0, le=86_400_000)


class DuelEventsRequest(BaseModel):
    events: list[DuelEventIn] = Field(min_length=1, max_length=100)


class DuelSongInfo(BaseModel):
    id: UUID
    slug: str
    title: str
    artist: str | None = None
    bpm: int
    difficulty: int
    duration_seconds: int
    time_signature_numerator: int = 4
    time_signature_denominator: int = 4


class DuelParticipantInfo(BaseModel):
    side: Literal["player", "opponent"]
    user_id: UUID | None = None
    username: str | None = None
    display_name: str | None = None
    is_bot: bool = False
    bot_name: str | None = None
    bot_avatar: str | None = None
    ready: bool = False
    connected: bool = False
    left: bool = False
    #: Current HP while playing; final HP after the match.
    hp: float = 100.0
    perfect: int = 0
    great: int = 0
    good: int = 0
    miss: int = 0
    notes: int = 0
    accuracy: float = 0.0
    damage_dealt: float = 0.0
    damage_taken: float = 0.0
    xp_awarded: int = 0
    rating_before: int | None = None
    rating_after: int | None = None
    rating_delta: int | None = None
    result: Literal["win", "loss", "draw"] | None = None


class DuelMatchState(BaseModel):
    """Authoritative snapshot of a match."""

    match_id: UUID
    mode: DuelMode
    status: Literal["waiting", "playing", "finished", "cancelled"]
    rules_version: int
    seed: int
    bot_level: DuelBotLevel | None = None
    song: DuelSongInfo
    note_count: int
    next_note_index: int
    your_side: Literal["player", "opponent"] | None = None
    #: Milliseconds until ``started_at`` (negative once the timeline started).
    starts_in_ms: int = 0
    #: Server clock relative to ``started_at`` (negative during countdown).
    server_time_ms: int = 0
    waiting_seconds: int = 0
    waiting_expired: bool = False
    winner_user_id: UUID | None = None
    winner_side: Literal["player", "opponent"] | None = None
    created_at: datetime
    started_at: datetime | None = None
    finished_at: datetime | None = None
    participants: list[DuelParticipantInfo] = Field(default_factory=list)


class DuelEventsRejected(BaseModel):
    index: int
    note_index: int
    code: str
    message: str


class DuelEventsResponse(BaseModel):
    match: DuelMatchState
    accepted: list[int] = Field(default_factory=list)
    duplicates: list[int] = Field(default_factory=list)
    rejected: list[DuelEventsRejected] = Field(default_factory=list)
    #: Exchanges resolved as a result of this batch (also broadcast over WS).
    exchanges: list[dict[str, Any]] = Field(default_factory=list)
    finished: bool = False


class DuelResultResponse(BaseModel):
    match: DuelMatchState
    #: ``True`` when this call found the match already finished and returned
    #: the stored result without awarding XP or rating twice.
    already_finished: bool = False
    #: ``True`` when an online ``finish`` was recorded but the opponent has not
    #: finished yet.
    waiting_for_opponent: bool = False


class DuelHistoryItem(BaseModel):
    match_id: UUID
    mode: DuelMode
    status: str
    song: DuelSongInfo
    opponent: DuelParticipantInfo | None = None
    your_side: Literal["player", "opponent"] | None = None
    hp: float
    opponent_hp: float = 0.0
    result: Literal["win", "loss", "draw"] | None = None
    xp_awarded: int = 0
    rating_delta: int | None = None
    created_at: datetime
    finished_at: datetime | None = None


class DuelHistoryResponse(BaseModel):
    items: list[DuelHistoryItem]
    total: int
    limit: int
    offset: int


class DuelLeaderboardItem(BaseModel):
    rank: int
    user_id: UUID
    username: str
    display_name: str | None = None
    level: int = 1
    rating: int = 1000
    duels: int = 0
    wins: int = 0
    losses: int = 0
    draws: int = 0
    win_rate: float = 0.0
    xp_earned: int = 0
    accuracy: float = 0.0
    damage_dealt: float = 0.0


class DuelLeaderboardResponse(BaseModel):
    period: Literal["week", "month", "all"]
    items: list[DuelLeaderboardItem]
    total: int
    limit: int
    offset: int


class DuelLobbyItem(BaseModel):
    """One joinable online room shown in the duel lobby."""

    match_id: UUID
    song_id: UUID
    song_slug: str
    song_title: str
    #: Display name of the host, falling back to the username.
    host_name: str
    #: Latest online Elo of the host, or the start rating before the first duel.
    host_rating: int = 1000
    created_at: datetime
    #: Whole seconds the host has been waiting; never negative.
    waiting_seconds: int = 0
    note_count: int = 0


class DuelLobbyResponse(BaseModel):
    items: list[DuelLobbyItem]
    total: int


class DuelProfileTotals(BaseModel):
    duels: int = 0
    wins: int = 0
    losses: int = 0
    draws: int = 0
    win_rate: float = 0.0
    xp_earned: int = 0
    accuracy: float = 0.0
    damage_dealt: float = 0.0
    best_win_streak: int = 0


class DuelProfileResponse(BaseModel):
    user_id: UUID
    username: str
    display_name: str | None = None
    level: int = 1
    rating: int = 1000
    peak_rating: int = 1000
    online: DuelProfileTotals
    bot: DuelProfileTotals
    recent_matches: list[DuelHistoryItem] = Field(default_factory=list)


class DuelMeResponse(DuelProfileResponse):
    active_match: DuelMatchState | None = None
