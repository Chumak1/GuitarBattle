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
    #: Lobby the match was started from; ``None`` for bot and quick-queue
    #: matches. Additive: older clients ignore it.
    lobby_id: UUID | None = None
    song: DuelSongInfo
    note_count: int
    next_note_index: int
    your_side: Literal["player", "opponent"] | None = None
    #: Character visuals of both sides, read from the players' current
    #: ``users.character_id`` at response time (nothing is snapshotted). The
    #: ``you_*`` pair mirrors ``your_side``; in a viewerless broadcast
    #: (``your_side == null``) all four fields are ``null``.
    you_character_name: str | None = None
    you_character_sheet_url: str | None = None
    opponent_character_name: str | None = None
    opponent_character_sheet_url: str | None = None
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

# ---------------------------------------------------------------------------
# Shared duel lobbies (rooms with chat, song picker and a host-started match)
# ---------------------------------------------------------------------------
DuelRoomStatus = Literal["waiting", "playing", "closed"]


class DuelRoomCreate(BaseModel):
    """``POST /duels/lobbies`` body."""

    name: str = Field(min_length=1, max_length=40)
    #: Optional room password. An empty/blank string means "no password".
    password: str | None = Field(default=None, max_length=128)

    @model_validator(mode="after")
    def _clean(self) -> "DuelRoomCreate":
        self.name = self.name.strip()
        if not self.name:
            raise ValueError("name must not be blank")
        if self.password is not None:
            self.password = self.password.strip() or None
        return self


class DuelRoomJoin(BaseModel):
    #: Required only when the room has a password.
    password: str | None = Field(default=None, max_length=128)


class DuelRoomSongSelect(BaseModel):
    #: UUID of a published catalogue song.
    song_id: UUID


class DuelRoomChatIn(BaseModel):
    message: str = Field(min_length=1, max_length=500)


class DuelRoomPlayer(BaseModel):
    user_id: UUID
    username: str
    display_name: str | None = None
    is_host: bool = False


class DuelRoomStartState(BaseModel):
    """Pending "start the duel?" request of a room.

    Both players confirm *before* the song is downloaded: the host creates the
    request, each member sets their own flag, and the match is created only when
    both flags are ``True``. ``DuelRoomState.start`` is ``None`` when there is
    no pending request.
    """

    requested_by: UUID | None = None
    host_confirmed: bool = False
    guest_confirmed: bool = False


class DuelRoomState(BaseModel):
    """Authoritative snapshot of a lobby, returned to both members."""

    lobby_id: UUID
    name: str
    has_password: bool = False
    status: DuelRoomStatus
    host_user_id: UUID
    guest_user_id: UUID | None = None
    song: DuelSongInfo | None = None
    #: Newest match started from this room (``None`` before the first start).
    match_id: UUID | None = None
    players: list[DuelRoomPlayer] = Field(default_factory=list)
    #: Pending start confirmation; ``None`` means nobody asked to start (or the
    #: request was declined / cleared). Additive: older clients ignore it.
    start: DuelRoomStartState | None = None
    created_at: datetime
    updated_at: datetime


class DuelRoomSummary(BaseModel):
    """One row of ``GET /duels/lobbies``."""

    lobby_id: UUID
    name: str
    has_password: bool = False
    status: DuelRoomStatus
    host_user_id: UUID
    song: DuelSongInfo | None = None
    match_id: UUID | None = None
    players: list[DuelRoomPlayer] = Field(default_factory=list)
    created_at: datetime


class DuelRoomListResponse(BaseModel):
    items: list[DuelRoomSummary]
    total: int
    include_full: bool = False


class DuelRoomStartResponse(BaseModel):
    #: ``None`` while the other player has not confirmed yet; the client keeps
    #: polling the room. Filled as soon as both confirmations arrive.
    match_id: UUID | None = None
    lobby: DuelRoomState


class DuelChatMessageOut(BaseModel):
    #: Monotonic id; use it as ``after_id`` on the next poll.
    id: int
    user_id: UUID
    username: str
    display_name: str | None = None
    text: str
    created_at: datetime


class DuelChatHistoryResponse(BaseModel):
    items: list[DuelChatMessageOut] = Field(default_factory=list)
    #: Highest id in ``items``, or the requested ``after_id`` when empty.
    latest_id: int = 0


class DuelRematchResponse(BaseModel):
    match_id: UUID
    previous_match_id: UUID
    lobby_id: UUID | None = None
    song: DuelSongInfo
    #: ``True`` when a fresh match already existed and was returned instead of
    #: creating a duplicate (double click / retry).
    already_created: bool = False
