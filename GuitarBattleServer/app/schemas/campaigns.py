"""Pydantic schemas of the admin, public and progress campaign API."""
from __future__ import annotations

from datetime import datetime
from typing import Any
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from app.schemas.gameplay_visuals import PublicCharacterResponse


class CampaignSongItem(BaseModel):
    """One stage of a campaign, flattened with the song fields the client needs."""

    song_id: UUID
    slug: str
    title: str
    difficulty: int
    is_published: bool
    #: 0-based position inside the campaign; the response is sorted by it.
    position: int
    #: **Effective** bot difficulty of this stage: ``campaign_songs.bot_level``
    #: when set, otherwise the campaign default. Always ``0..2``; older servers
    #: do not send it and clients fall back to ``CampaignItem.bot_level``.
    bot_level: int = Field(ge=0, le=2, description="0 = easy, 1 = normal, 2 = hard")


class CampaignItem(BaseModel):
    """Campaign payload returned by both ``/campaigns`` and ``/admin/campaigns``.

    ``song_count`` is the number of entries in ``songs``: for the public
    endpoint that means *published* stages only, for the admin endpoint every
    configured stage.

    The reward fields are additive:

    * ``reward_character_id`` is the **public integer** id on the public
      endpoint and the internal UUID on the admin endpoint
      (:class:`AdminCampaignItem` overrides the type);
    * ``reward_character_name`` is always the display name;
    * ``reward_condition`` is the administrator text or the derived
      ``"Complete <name>"``. It is ``None`` when the tower has no reward.
    """

    id: UUID
    name: str
    bot_level: int
    song_count: int
    songs: list[CampaignSongItem]
    reward_character_id: int | None = None
    reward_character_name: str | None = None
    reward_condition: str | None = None
    #: Origin-relative URL of the tower cover (``/media/campaigns/<file>``) or
    #: ``None``. The client downloads it from ``serverOrigin + cover_image_url``.
    cover_image_url: str | None = None
    #: Administrator description or ``None``.
    description: str | None = None
    created_at: datetime
    updated_at: datetime


class AdminCampaignItem(CampaignItem):
    """Admin payload: the reward character keeps its internal UUID."""

    reward_character_id: UUID | None = None


class CampaignListResponse(BaseModel):
    items: list[CampaignItem]


class AdminCampaignListResponse(BaseModel):
    items: list[AdminCampaignItem]


def _trim_name(value: str) -> str:
    cleaned = value.strip()
    if not cleaned:
        raise ValueError("Campaign name must not be blank")
    return cleaned


def _clean_condition(value: str | None) -> str | None:
    """Trim the unlock text; an all-space value clears it (``None``)."""
    if value is None:
        return None
    cleaned = value.strip()
    return cleaned or None


def _clean_description(value: str | None) -> str | None:
    """Trim the description; an all-space value clears it (``None``)."""
    return _clean_condition(value)


class CampaignCreate(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str = Field(min_length=1, max_length=120)
    bot_level: int = Field(ge=0, le=2, description="0 = easy, 1 = normal, 2 = hard")
    #: Internal UUID of the character the tower rewards on completion.
    reward_character_id: UUID | None = None
    reward_condition: str | None = Field(default=None, max_length=200)
    #: Optional tower description. The cover is set only by the upload route.
    description: str | None = Field(default=None, max_length=400)

    @field_validator("name")
    @classmethod
    def trim_name(cls, value: str) -> str:
        return _trim_name(value)

    @field_validator("reward_condition")
    @classmethod
    def clean_condition(cls, value: str | None) -> str | None:
        return _clean_condition(value)

    @field_validator("description")
    @classmethod
    def clean_description(cls, value: str | None) -> str | None:
        return _clean_description(value)


class CampaignUpdate(BaseModel):
    """Partial update; omitted fields keep their stored value.

    ``reward_character_id: null`` / ``reward_condition: null`` (or an empty
    string) clear the reward. ``description: null`` (or an empty string) clears
    the description. ``name`` and ``bot_level`` stay NOT NULL: an explicit
    ``null`` is rejected with 422 by the endpoint.
    """

    model_config = ConfigDict(extra="forbid")

    name: str | None = Field(default=None, min_length=1, max_length=120)
    bot_level: int | None = Field(default=None, ge=0, le=2)
    reward_character_id: UUID | None = None
    reward_condition: str | None = Field(default=None, max_length=200)
    description: str | None = Field(default=None, max_length=400)

    @field_validator("name")
    @classmethod
    def trim_name(cls, value: str | None) -> str | None:
        return _trim_name(value) if value is not None else None

    @field_validator("reward_condition")
    @classmethod
    def clean_condition(cls, value: str | None) -> str | None:
        return _clean_condition(value)

    @field_validator("description")
    @classmethod
    def clean_description(cls, value: str | None) -> str | None:
        return _clean_description(value)


class CampaignSongEntry(BaseModel):
    """One stage in the per-song replacement form.

    ``bot_level`` may be omitted (``null``): the stage then uses
    ``campaigns.bot_level``, exactly like a row stored with ``NULL``.
    """

    model_config = ConfigDict(extra="forbid")

    song_id: UUID
    bot_level: int | None = Field(default=None, ge=0, le=2)


class CampaignSongLevelUpdate(BaseModel):
    """Change the bot difficulty of one existing stage."""

    model_config = ConfigDict(extra="forbid")

    bot_level: int = Field(ge=0, le=2, description="0 = easy, 1 = normal, 2 = hard")


class CampaignSongsUpdate(BaseModel):
    """Replacement of the ordered stage list.

    Two shapes are accepted, never both at once:

    * ``{"songs": [{"song_id": ..., "bot_level": 0}, ...]}`` — the current
      form. Array order is the stage order and every stage carries its own
      difficulty; a missing ``bot_level`` falls back to the campaign default.
    * ``{"song_ids": [...]}`` — the legacy form kept for older admin pages.
      The existing per-stage levels of songs that stay in the list are
      preserved (matched by ``song_id``); newly added songs use
      ``campaigns.bot_level``.

    An empty list clears the campaign. ``song_id`` duplicates are rejected
    with 422 and unknown ids with 404 before anything is written.
    """

    model_config = ConfigDict(extra="forbid")

    song_ids: list[UUID] | None = Field(default=None, max_length=200)
    songs: list[CampaignSongEntry] | None = Field(default=None, max_length=200)

    @model_validator(mode="after")
    def exactly_one_shape(self) -> "CampaignSongsUpdate":
        if (self.song_ids is None) == (self.songs is None):
            raise ValueError(
                "Provide exactly one of 'songs' or 'song_ids'"
            )
        return self


# ---------------------------------------------------------------------------
# Player progress
# ---------------------------------------------------------------------------
class CampaignProgressUpdate(BaseModel):
    """The highest cleared stage reported by the client.

    The client never sends a character id or a completion flag: the server
    clamps ``best_stage`` to the published stage count and grants the reward
    itself.
    """

    model_config = ConfigDict(extra="forbid")

    best_stage: int = Field(ge=0, description="Number of cleared stages")


class CampaignProgressResult(BaseModel):
    """Server view of one tower after a progress report."""

    campaign_id: UUID
    #: Stage of the **current run**: ``best_stage % stage_count`` in
    #: ``0..stage_count``; ``stage_count`` means the run is finished.
    best_stage: int
    #: Number of published stages the client can play.
    stage_count: int
    completed: bool
    #: Filled only on the transition to completion when the reward was newly
    #: granted. ``None`` when there is no reward or it was already owned.
    unlocked_character: PublicCharacterResponse | None = None
    #: True when the reward character was already in the collection.
    already_owned: bool = False


class CampaignProgressItem(BaseModel):
    campaign_id: UUID
    #: Stage of the current run (``best_stage % stage_count``), 0..stage_count.
    best_stage: int
    stage_count: int
    completed: bool


class CampaignProgressListResponse(BaseModel):
    items: list[CampaignProgressItem]


# ---------------------------------------------------------------------------
# Battle ("tower") stage results — authoritative XP
# ---------------------------------------------------------------------------
class CampaignStageResultSubmit(BaseModel):
    """One finished Battle stage, reported by the client.

    The client sends facts only: its actual score, accuracy, note qualities,
    completion and the played/total note counters. XP, the repeat factor and
    the personal-best bonus are computed server-side; there is no field for
    them, and ``run_id`` (unique per player) makes a retried report idempotent.
    """

    model_config = ConfigDict(extra="forbid")

    run_id: UUID
    won: bool
    score_points: int = Field(ge=0, le=10_000_000)
    accuracy_percent: float = Field(ge=0, le=100)
    perfect_count: int = Field(ge=0)
    great_count: int = Field(ge=0)
    good_count: int = Field(ge=0)
    miss_count: int = Field(ge=0)
    completion_percent: float = Field(ge=0, le=100)
    notes_total: int | None = Field(default=None, ge=0)
    notes_played: int | None = Field(default=None, ge=0)

    @model_validator(mode="after")
    def sensible_counters(self) -> "CampaignStageResultSubmit":
        total = (
            self.perfect_count
            + self.great_count
            + self.good_count
            + self.miss_count
        )
        if total > 1_000_000:
            raise ValueError("Invalid note counters")
        return self


class CampaignStageResultResponse(BaseModel):
    """Server view of one Battle stage: progress, XP and the optional unlock."""

    run_id: UUID
    #: True when this run had already been rewarded and nothing changed now.
    already_processed: bool = False
    won: bool
    #: Number of playable (published) stages of the tower.
    stage_count: int
    #: Stage of the current run (``best_stage % stage_count``), 0..stage_count.
    best_stage: int
    completed: bool
    #: Actual score exactly as submitted (Battle XP never rewrites it).
    score_points: int
    #: Extrapolated score; ``NULL`` when the attempt ended too early.
    normalized_score: int | None = None
    completion_percent: float
    xp_earned: int
    xp_breakdown: dict[str, Any] | None = None
    xp_total: int
    level: int
    level_up: bool
    pb_improved: bool
    repeat_factor: float
    #: Filled only on the transition to completion when the reward was newly
    #: granted; ``None`` when the tower has no reward or it was already owned.
    unlocked_character: PublicCharacterResponse | None = None
