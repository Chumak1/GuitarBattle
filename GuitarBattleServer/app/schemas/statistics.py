"""Request/response models for activity telemetry and admin statistics."""
from datetime import datetime
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator


class HeartbeatRequest(BaseModel):
    client_session_id: str = Field(min_length=1, max_length=64)
    # Telemetry must never be rejected for a cosmetic reason: version, platform
    # and mode are trimmed and truncated to the column width instead of failing
    # the whole heartbeat (a rejected heartbeat would lose app time).
    app_version: str | None = None
    platform: str | None = None
    final: bool = False

    @field_validator("client_session_id")
    @classmethod
    def trim_client_session_id(cls, value: str) -> str:
        cleaned = value.strip()
        if not cleaned:
            raise ValueError("client_session_id must not be blank")
        return cleaned

    @field_validator("app_version", "platform")
    @classmethod
    def trim_optional_text(cls, value: str | None) -> str | None:
        if value is None:
            return None
        cleaned = value.strip()
        return cleaned[:32] or None


class HeartbeatResponse(BaseModel):
    session_id: UUID
    started_at: datetime
    last_seen_at: datetime


class GameLaunchRequest(BaseModel):
    client_session_id: str | None = Field(default=None, max_length=64)
    game_key: str = Field(min_length=1)
    song_id: UUID | None = None
    mode: str | None = None

    @field_validator("game_key")
    @classmethod
    def trim_game_key(cls, value: str) -> str:
        cleaned = value.strip()
        if not cleaned:
            raise ValueError("game_key must not be blank")
        # Keys such as ``local:<slug>`` can exceed the column width; keep the
        # launch instead of dropping it.
        return cleaned[:120]

    @field_validator("client_session_id")
    @classmethod
    def trim_client_session_id(cls, value: str | None) -> str | None:
        if value is None:
            return None
        cleaned = value.strip()
        return cleaned[:64] or None

    @field_validator("mode")
    @classmethod
    def trim_mode(cls, value: str | None) -> str | None:
        if value is None:
            return None
        cleaned = value.strip()
        return cleaned[:32] or None


class GameLaunchResponse(BaseModel):
    id: UUID


class StatisticsOverview(BaseModel):
    generated_at: datetime
    users_total: int
    users_active_24h: int
    users_active_7d: int
    sessions_total: int
    app_seconds_total: int
    avg_session_seconds: int
    launches_total: int
    launches_24h: int


class StatisticsUserItem(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    user_id: UUID
    username: str
    display_name: str
    email: str
    is_active: bool
    created_at: datetime
    app_seconds_total: int
    sessions_count: int
    last_seen_at: datetime | None
    launches_count: int
    last_launch_at: datetime | None
    games_distinct: int
    # Game results are stored in ``scores`` and linked through ``game_sessions``.
    scores_count: int
    best_score: int | None


class StatisticsUserList(BaseModel):
    items: list[StatisticsUserItem]
    total: int
    limit: int
    offset: int


class StatisticsGameItem(BaseModel):
    game_key: str
    title: str
    launches: int
    unique_users: int
    last_launch_at: datetime | None


class StatisticsGameList(BaseModel):
    items: list[StatisticsGameItem]
    total: int
