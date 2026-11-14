"""Pydantic schemas for the ``/api/v1/social`` "Friends" endpoints."""
from __future__ import annotations

from datetime import datetime
from typing import Any, Literal
from uuid import UUID

from pydantic import BaseModel, Field, model_validator

Presence = Literal["online", "in_lobby", "in_match", "offline", "hidden"]
Relationship = Literal["self", "friend", "incoming", "outgoing", "blocked", "none"]
RequestPolicy = Literal["everyone", "friends", "nobody"]
RequestStatus = Literal["pending", "accepted", "declined", "cancelled"]
ActivityKind = Literal[
    "record", "tower", "character", "level", "duel_win", "achievement"
]


class SocialStats(BaseModel):
    """The statistics block of a player card."""

    wins: int = 0
    losses: int = 0
    draws: int = 0
    duels: int = 0
    #: Percentage in ``[0, 100]``, same formula as the duel leaderboard.
    win_rate: float = 0.0
    #: Best single-match accuracy over finished human duels, in percent.
    best_accuracy: float = 0.0
    songs_mastered: int = 0
    achievements_unlocked: int = 0
    achievements_total: int = 0
    #: Sum of full clears over every tower.
    towers_completed: int = 0


class CompactPlayer(BaseModel):
    """The small card used by lists, search and requests."""

    user_id: UUID
    username: str
    display_name: str
    level: int = 1
    rating: int = 1000
    presence: Presence = "offline"
    relationship: Relationship | None = None


class PublicPlayerCard(BaseModel):
    """The aggregated public card of one player."""

    user_id: UUID
    username: str
    display_name: str
    level: int = 1
    xp_total: int = 0
    rating: int = 1000
    peak_rating: int = 1000
    #: Guitar Skills rating (additive field; ``None`` until first calculation).
    guitar_skills: float | None = None
    guitar_skills_rank: int | None = None
    provisional: bool | None = None
    character_name: str | None = None
    character_sheet_url: str | None = None
    presence: Presence = "offline"
    relationship: Relationship = "none"
    #: ``None`` when the player hides their statistics from this viewer.
    stats: SocialStats | None = None
    #: False when a block between the pair disables challenges.
    can_challenge: bool = True


class PlayerListResponse(BaseModel):
    items: list[CompactPlayer]
    total: int
    limit: int
    offset: int


class SearchResponse(PlayerListResponse):
    query: str = ""


class FriendsResponse(PlayerListResponse):
    pass


class FriendRequestItem(BaseModel):
    id: UUID
    from_user_id: UUID
    to_user_id: UUID
    status: RequestStatus
    created_at: datetime
    responded_at: datetime | None = None
    #: The other side of the request (sender for incoming, addressee for
    #: outgoing).
    user: CompactPlayer
    direction: Literal["incoming", "outgoing"]


class RequestsResponse(BaseModel):
    items: list[FriendRequestItem]
    total: int
    limit: int
    offset: int


class LeaderboardItem(BaseModel):
    rank: int
    user_id: UUID
    username: str
    display_name: str
    level: int = 1
    xp_total: int = 0
    rating: int = 1000
    #: Guitar Skills rating (additive field; ``None`` until first calculation).
    guitar_skills: float | None = None
    provisional: bool | None = None
    wins: int = 0
    losses: int = 0
    draws: int = 0
    duels: int = 0
    win_rate: float = 0.0
    presence: Presence = "offline"
    is_self: bool = False


class LeaderboardResponse(BaseModel):
    sort: Literal["rating", "level", "wins", "win_rate", "guitar_skills"]
    items: list[LeaderboardItem]
    total: int
    limit: int
    offset: int


class PrivacySettings(BaseModel):
    request_policy: RequestPolicy = "everyone"
    show_online: bool = True
    show_profile: bool = True
    show_stats: bool = True
    show_activity: bool = True
    allow_challenges: bool = True


class PrivacyUpdate(BaseModel):
    request_policy: RequestPolicy | None = None
    show_online: bool | None = None
    show_profile: bool | None = None
    show_stats: bool | None = None
    show_activity: bool | None = None
    allow_challenges: bool | None = None


class RivalUpdate(BaseModel):
    user_id: UUID


class RivalItem(BaseModel):
    user: CompactPlayer
    updated_at: datetime


class RivalResponse(BaseModel):
    rival: RivalItem | None = None


class ActivityItem(BaseModel):
    id: UUID
    kind: ActivityKind
    payload: dict[str, Any] = Field(default_factory=dict)
    created_at: datetime
    user: CompactPlayer


class ActivityResponse(BaseModel):
    items: list[ActivityItem]
    total: int
    limit: int
    offset: int


class NotificationItem(BaseModel):
    id: UUID
    kind: str
    payload: dict[str, Any] = Field(default_factory=dict)
    read_at: datetime | None = None
    created_at: datetime


class NotificationsResponse(BaseModel):
    items: list[NotificationItem]
    total: int
    #: Total unread notifications, independent of ``unread_only``.
    unread: int
    limit: int
    offset: int


class NotificationsReadRequest(BaseModel):
    #: Ids to mark read. Ignored when ``all`` is true.
    ids: list[UUID] | None = None
    #: Mark every unread notification read.
    all: bool = False

    @model_validator(mode="after")
    def _require_selection(self) -> "NotificationsReadRequest":
        if not self.all and not self.ids:
            raise ValueError("pass ids or all=true")
        return self


class NotificationsReadResponse(BaseModel):
    updated: int
