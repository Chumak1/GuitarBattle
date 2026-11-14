"""Campaign read/write helpers shared by the public and admin API.

A campaign is a small aggregate: one row in ``campaigns`` plus an ordered list
of ``campaign_songs``. Both endpoints return the same flattened payload; the
public one filters the stages down to published songs and drops campaigns that
end up without any playable stage.
"""
from __future__ import annotations

from collections.abc import Sequence
from datetime import datetime, timezone
from uuid import UUID

from fastapi import HTTPException, status
from sqlalchemy import delete, func, select
from sqlalchemy.orm import Session, selectinload

from app.models.campaign import Campaign, CampaignSong
from app.models.character_unlocks import UserCampaignProgress
from app.models.song import Song
from app.schemas.campaigns import (
    AdminCampaignItem,
    CampaignItem,
    CampaignProgressItem,
    CampaignProgressResult,
    CampaignSongItem,
)
from app.schemas.gameplay_visuals import PublicCharacterResponse
from app.services.characters import grant_character, is_character_owned
from app.services.gameplay_media import character_sheet_url

#: Upper bound matching ``CampaignSongsUpdate.song_ids``; the admin UI never
#: needs more and a smaller list stays readable in one response.
MAX_CAMPAIGN_SONGS = 200

BOT_LEVEL_LABELS = {0: "easy", 1: "normal", 2: "hard"}


def effective_bot_level(campaign: Campaign, link: CampaignSong) -> int:
    """Stage level with the campaign-wide default as the fallback."""
    level = link.bot_level
    if level is None:
        level = campaign.bot_level
    return max(0, min(2, int(level)))



def campaign_query():
    """Select statement with the whole aggregate eagerly loaded."""
    return select(Campaign).options(
        selectinload(Campaign.songs).selectinload(CampaignSong.song)
    )


def get_campaign(db: Session, campaign_id: UUID) -> Campaign | None:
    return db.scalar(campaign_query().where(Campaign.id == campaign_id))


def list_campaigns(db: Session) -> Sequence[Campaign]:
    """All campaigns, stable alphabetical order (ties broken by id).

    The admin panel shows many campaigns and no pagination: the list stays
    small, and a stable order keeps the editor selection predictable.
    """
    return db.scalars(
        campaign_query().order_by(Campaign.name.asc(), Campaign.id.asc())
    ).all()


def serialize_campaign(
    campaign: Campaign,
    *,
    published_only: bool = False,
    admin: bool = False,
) -> CampaignItem:
    """Flatten a campaign; ``published_only`` hides unpublished stages.

    ``CampaignSongItem.bot_level`` is the *effective* stage level:
    ``campaign_songs.bot_level`` when set, otherwise the campaign default. That
    keeps old rows (``NULL``) and pre-0028 databases behaving exactly as
    before.

    The reward fields are additive. The public payload exposes the reward as the
    player-facing integer ``public_id``; the admin payload keeps the internal
    UUID (``AdminCampaignItem``). ``reward_condition`` is the administrator text
    or the derived ``"Complete <campaign name>"``.
    """
    songs: list[CampaignSongItem] = []
    for link in sorted(campaign.songs, key=lambda item: item.position):
        song = link.song
        if song is None:
            # The FK cascades, so this only protects against a half-deleted row.
            continue
        if published_only and not song.is_published:
            continue
        songs.append(
            CampaignSongItem(
                song_id=song.id,
                slug=song.slug,
                title=song.title,
                difficulty=song.difficulty,
                is_published=song.is_published,
                position=link.position,
                bot_level=effective_bot_level(campaign, link),
            )
        )

    reward = campaign.reward_character
    reward_name = reward.name if reward is not None else None
    condition: str | None = None
    if reward is not None:
        custom = (campaign.reward_condition or "").strip()
        if admin:
            # The editor round-trips the stored override only; the derived text
            # is built by the public payload and shown as the input placeholder.
            condition = custom or None
        else:
            condition = custom or f"Complete {campaign.name}"
    reward_id = None
    if reward is not None:
        reward_id = reward.id if admin else reward.public_id

    item_class = AdminCampaignItem if admin else CampaignItem
    return item_class(
        id=campaign.id,
        name=campaign.name,
        bot_level=campaign.bot_level,
        song_count=len(songs),
        songs=songs,
        reward_character_id=reward_id,
        reward_character_name=reward_name,
        reward_condition=condition,
        cover_image_url=campaign.cover_image_url or None,
        description=campaign.description or None,
        created_at=campaign.created_at,
        updated_at=campaign.updated_at,
    )


def public_campaigns(db: Session) -> list[CampaignItem]:
    """Published view: only published stages, empty campaigns are omitted.

    A campaign with zero published songs is not a playable tower, so it is not
    advertised on the public endpoint. It still exists for the administrator and
    reappears as soon as one of its songs is published again.
    """
    result: list[CampaignItem] = []
    for campaign in list_campaigns(db):
        item = serialize_campaign(campaign, published_only=True)
        if item.song_count:
            result.append(item)
    return result


def ensure_unique_name(
    db: Session, name: str, *, campaign_id: UUID | None = None
) -> None:
    """Reject a name that equals another campaign, case-insensitively."""
    normalized = name.strip().lower()
    query = select(Campaign.id).where(func.lower(Campaign.name) == normalized)
    if campaign_id is not None:
        query = query.where(Campaign.id != campaign_id)
    if db.scalar(query) is not None:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Campaign name already exists",
        )


def legacy_campaign_entries(
    campaign: Campaign, song_ids: Sequence[UUID]
) -> list[tuple[UUID, int | None]]:
    """Resolve the legacy ``song_ids`` shape to ``(song_id, bot_level)``.

    A song that is already a stage keeps its current explicit level (``NULL``
    means "campaign default", which stays ``NULL`` so a later change of the
    campaign default keeps applying). A newly added song is stored with the
    campaign default, i.e. also ``NULL``. This is why a legacy save never
    rewrites per-stage levels the administrator set earlier.
    """
    existing = {
        link.song_id: link.bot_level for link in campaign.songs
    }
    return [
        (song_id, existing.get(song_id)) for song_id in song_ids
    ]


def set_campaign_song_level(
    db: Session, campaign: Campaign, song_id: UUID, bot_level: int
) -> Campaign:
    """Change one stage's bot difficulty; the order and the list stay intact.

    Unknown ``song_id`` means the song is not a stage of this campaign → 404,
    so a typo cannot silently create a new stage.
    """
    link = db.scalar(
        select(CampaignSong).where(
            CampaignSong.campaign_id == campaign.id,
            CampaignSong.song_id == song_id,
        )
    )
    if link is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail="Song is not a stage of this campaign",
        )

    link.bot_level = bot_level
    campaign.updated_at = datetime.now(timezone.utc)
    db.commit()

    refreshed = get_campaign(db, campaign.id)
    assert refreshed is not None  # the row cannot disappear in this transaction
    return refreshed


def replace_campaign_songs(    db: Session,
    campaign: Campaign,
    entries: Sequence[tuple[UUID, int | None]],
) -> Campaign:
    """Replace the ordered stage list in one transaction.

    ``entries`` order becomes position 0, 1, 2… Each entry carries its
    per-stage bot level; ``None`` means "use ``campaigns.bot_level``".
    Duplicates are rejected with 422 and unknown songs with 404 before anything
    is written, so a rejected request never clears the previous list.
    """
    song_ids = [song_id for song_id, _ in entries]

    seen: set[UUID] = set()
    duplicates: list[str] = []
    for song_id in song_ids:
        if song_id in seen:
            duplicates.append(str(song_id))
        seen.add(song_id)
    if duplicates:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Duplicate song ids are not allowed: " + ", ".join(duplicates),
        )

    for _, level in entries:
        if level is not None and not 0 <= level <= 2:
            raise HTTPException(
                status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                detail="bot_level must be 0 (easy), 1 (normal) or 2 (hard)",
            )

    if song_ids:
        found = set(
            db.scalars(select(Song.id).where(Song.id.in_(list(song_ids)))).all()
        )
        missing = [str(song_id) for song_id in song_ids if song_id not in found]
        if missing:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail="Unknown song ids: " + ", ".join(missing),
            )

    # Delete first: the unique (campaign_id, position) constraint would
    # otherwise collide with the rows that are being replaced.
    db.execute(
        delete(CampaignSong).where(CampaignSong.campaign_id == campaign.id)
    )
    db.flush()
    db.add_all(
        CampaignSong(
            campaign_id=campaign.id,
            song_id=song_id,
            position=position,
            bot_level=level,
        )
        for position, (song_id, level) in enumerate(entries)
    )
    campaign.updated_at = datetime.now(timezone.utc)
    db.commit()
    # Re-select so ``songs`` and its ``song`` rows are eagerly loaded after the
    # bulk delete/re-insert; the identity-mapped campaign keeps the same id.
    refreshed = get_campaign(db, campaign.id)
    assert refreshed is not None  # the row cannot disappear in this transaction
    return refreshed


# ---------------------------------------------------------------------------
# Player progress and rewards
# ---------------------------------------------------------------------------
def public_stage_count(campaign: Campaign) -> int:
    """Number of stages the client can actually play (published songs only)."""
    return sum(
        1
        for link in campaign.songs
        if link.song is not None and link.song.is_published
    )


def displayed_stage(raw_best: int, stage_count: int) -> int:
    """Stage the player is currently on, in ``0..stage_count``.

    ``user_campaign_progress.best_stage`` is a cumulative counter: every won
    stage advances it by one, including the wins of a replayed finished tower.
    The stage of the *current* run is therefore ``best_stage % stage_count``,
    and a positive multiple of ``stage_count`` means "the run is finished".
    Every response that shows progress to a client must use this helper; the
    stored counter itself is never clamped, otherwise the modulo is destroyed
    (``min(4, 3) == 3`` would report a finished tower instead of stage 1).
    """
    if stage_count <= 0:
        return 0
    raw = int(raw_best)
    done = raw % int(stage_count)
    return int(stage_count) if (raw > 0 and done == 0) else done


def _reward_public(character) -> PublicCharacterResponse:
    return PublicCharacterResponse(
        id=character.public_id,
        name=character.name,
        sheet_url=character_sheet_url(character.sheet_key),
    )


def record_campaign_progress(
    db: Session, user, campaign: Campaign, reported_best: int
) -> CampaignProgressResult:
    """Upsert the monotonic best stage and grant the reward on completion.

    ``reported_best`` comes from the client and is therefore untrusted: it is
    clamped to the number of *published* stages before it is stored, and the
    reward is decided here from ``campaigns.reward_character_id``. A client
    cannot name a character, and replaying a completed tower never grants the
    same reward twice (``already_owned`` is reported instead).
    """
    stage_count = public_stage_count(campaign)
    clamped = max(0, min(int(reported_best), stage_count))

    row = db.scalar(
        select(UserCampaignProgress)
        .where(
            UserCampaignProgress.user_id == user.id,
            UserCampaignProgress.campaign_id == campaign.id,
        )
        .with_for_update()
    )
    was_completed = row is not None and row.completed_at is not None

    previous_best = int(row.best_stage) if row is not None else 0
    best = max(previous_best, clamped)
    if stage_count > 0:
        # A tower that lost published stages must not report more cleared
        # stages than it can be played for.
        best = min(best, stage_count)

    completed = stage_count > 0 and best >= stage_count
    now = datetime.now(timezone.utc)

    if row is None:
        row = UserCampaignProgress(
            user_id=user.id,
            campaign_id=campaign.id,
            best_stage=best,
        )
        db.add(row)
    else:
        row.best_stage = best

    row.updated_at = now
    if completed and row.completed_at is None:
        row.completed_at = now

    unlocked: PublicCharacterResponse | None = None
    already_owned = False
    reward = campaign.reward_character
    if completed and reward is not None:
        owned_before = is_character_owned(db, user, reward)
        if not was_completed and not owned_before:
            grant_character(db, user, reward, source_campaign_id=campaign.id)
            unlocked = _reward_public(reward)
        else:
            already_owned = True

    db.commit()

    return CampaignProgressResult(
        campaign_id=campaign.id,
        # The legacy endpoint keeps storing the monotonic, clamped counter; the
        # answer uses the same "current run" view as every other endpoint.
        best_stage=displayed_stage(best, stage_count),
        stage_count=stage_count,
        completed=completed,
        unlocked_character=unlocked,
        already_owned=already_owned,
    )


def reset_campaign_progress(
    db: Session, user, campaign: Campaign
) -> CampaignProgressResult:
    """Start a new Battle run: raw ``best_stage`` drops back to ``0``.

    The client's RESET button now starts a new run on the server. Only the
    position counter is cleared; ``completed_at`` and ``completion_count`` are
    history and anti-farm data (the repeat XP factor), so they survive. No XP
    or rating is touched and the reward is never granted again. Calling this
    twice is a no-op beyond bumping ``updated_at``; no row is created for a
    player who never played the tower.
    """
    stage_count = public_stage_count(campaign)
    row = db.scalar(
        select(UserCampaignProgress)
        .where(
            UserCampaignProgress.user_id == user.id,
            UserCampaignProgress.campaign_id == campaign.id,
        )
        .with_for_update()
    )

    completed = False
    if row is not None:
        row.best_stage = 0
        row.updated_at = datetime.now(timezone.utc)
        completed = row.completed_at is not None
        db.commit()

    return CampaignProgressResult(
        campaign_id=campaign.id,
        best_stage=0,
        stage_count=stage_count,
        completed=completed,
        unlocked_character=None,
    )


def campaign_progress_items(db: Session, user) -> list[CampaignProgressItem]:
    """Server-side progress of every campaign, for the signed-in player."""
    rows = {
        row.campaign_id: row
        for row in db.scalars(
            select(UserCampaignProgress).where(
                UserCampaignProgress.user_id == user.id
            )
        ).all()
    }
    items: list[CampaignProgressItem] = []
    for campaign in list_campaigns(db):
        stage_count = public_stage_count(campaign)
        row = rows.get(campaign.id)
        # Read the raw counter WITHOUT clamping: the stage of the current run
        # is ``raw % stage_count``, so clamping first would destroy the modulo.
        best_raw = int(row.best_stage) if row is not None else 0
        completed = bool(
            (row is not None and row.completed_at is not None)
            or (stage_count > 0 and best_raw >= stage_count)
        )
        items.append(
            CampaignProgressItem(
                campaign_id=campaign.id,
                best_stage=displayed_stage(best_raw, stage_count),
                stage_count=stage_count,
                completed=completed,
            )
        )
    return items
