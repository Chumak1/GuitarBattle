"""Schemas of the administrator's per-account card.

The card combines three independent groups, each with its own rules:

* basics — account fields plus the character the player has picked;
* ``online`` / ``battle`` — aggregates from :mod:`app.services.admin_user_stats`;
* ``characters`` — the collection, the full catalogue for the "add" picker and
  the per-character ``can_remove`` flag (starters are always owned and can
  never be revoked).
"""
from datetime import datetime
from uuid import UUID

from pydantic import BaseModel, Field


class AdminCharacterItem(BaseModel):
    """One character of the catalogue, with the account's ownership state."""

    #: Internal ``ui_characters.id`` — what the grant/revoke endpoints accept.
    id: UUID
    #: Player-facing integer id (``GET /characters``, ``PATCH /auth/me``).
    public_id: int
    name: str
    sheet_url: str
    is_starter: bool
    #: ``NULL`` for a starter: it is owned without a ``user_characters`` row.
    unlocked_at: datetime | None = None
    source_campaign_id: UUID | None = None
    #: Starters are always available by design, so they cannot be revoked.
    can_remove: bool = True


class AdminSelectedCharacter(BaseModel):
    """The character the player currently has selected, if any."""

    id: UUID
    public_id: int
    name: str
    sheet_url: str


class AdminUserBasics(BaseModel):
    user_id: UUID
    username: str
    display_name: str
    email: str
    level: int
    xp_total: int
    created_at: datetime
    is_active: bool
    selected_character: AdminSelectedCharacter | None = None


class AdminOnlineStats(BaseModel):
    """Finished online duels only; bot matches are not part of this group."""

    matches: int
    wins: int
    losses: int
    draws: int
    win_rate: float
    #: Newest non-NULL ``rating_after``; ``NULL`` when never rated.
    rating: int | None = None
    best_win_streak: int
    accuracy: float
    damage_dealt: float
    xp_earned: int
    last_match_at: datetime | None = None


class AdminBattleTower(BaseModel):
    """Progress of one tower that this account has started."""

    campaign_id: UUID
    name: str
    #: Cleared stages, clamped to ``stage_count``.
    best_stage: int
    stage_count: int
    completions: int
    completed_at: datetime | None = None


class AdminBattleStats(BaseModel):
    towers_completed: int
    stages_cleared: int
    runs: int
    xp_earned: int
    last_run_at: datetime | None = None
    towers: list[AdminBattleTower] = Field(default_factory=list)


class AdminUserCharacters(BaseModel):
    owned: list[AdminCharacterItem] = Field(default_factory=list)
    #: Public id of the selected character, or ``NULL``.
    selected_character_id: int | None = None
    #: Whole catalogue with the same ownership fields; the UI hides what the
    #: account already owns before offering the "add" picker.
    available: list[AdminCharacterItem] = Field(default_factory=list)


class AdminUserOverview(BaseModel):
    basics: AdminUserBasics
    online: AdminOnlineStats
    battle: AdminBattleStats
    characters: AdminUserCharacters


class AdminUserCharacterChange(BaseModel):
    """Result of a grant (``created`` tells 201 from the idempotent 200)."""

    character: AdminCharacterItem
    created: bool
    selected_character_id: int | None = None


class AdminUserCharacterRemoval(BaseModel):
    removed: bool
    #: True when the revoked character was the player's selected one, so the
    #: selection was cleared.
    cleared_selected: bool
    selected_character_id: int | None = None
