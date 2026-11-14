"""Campaigns ("Tower riff") — public listing and administrator CRUD.

A campaign is a name, a default bot difficulty and an ordered list of songs;
each stage may override the default with its own difficulty. The number of
stages equals the number of songs added. There is no limit on the number of
campaigns.

Public ``GET /campaigns`` is anonymous and exposes only published songs; the
admin endpoints under ``/admin/campaigns`` use the same JWT/``is_admin``
dependency as the rest of the admin API. See ``docs/CAMPAIGNS_API.md``.
"""
from __future__ import annotations

import uuid
from pathlib import Path
from uuid import UUID

from fastapi import (
    APIRouter,
    Depends,
    File,
    HTTPException,
    Response,
    UploadFile,
    status,
)
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.api.v1.admin_site import (
    IMAGE_SIGNATURES,
    _remove_media_file,
    _safe_upload_path,
    _write_upload,
)
from app.api.v1.admin_songs import require_admin
from app.api.v1.auth import get_current_user
from app.config import settings
from app.database import get_db
from app.models.campaign import Campaign
from app.models.gameplay_visuals import UiCharacter
from app.models.user import User
from app.schemas.campaigns import (
    AdminCampaignItem,
    AdminCampaignListResponse,
    CampaignCreate,
    CampaignListResponse,
    CampaignProgressListResponse,
    CampaignProgressResult,
    CampaignProgressUpdate,
    CampaignSongLevelUpdate,
    CampaignSongsUpdate,
    CampaignStageResultResponse,
    CampaignStageResultSubmit,
    CampaignUpdate,
)
from app.services.campaign_runs import StageResultError, submit_stage_result
from app.services.campaigns import (
    campaign_progress_items,
    ensure_unique_name,
    get_campaign,
    legacy_campaign_entries,
    list_campaigns,
    public_campaigns,
    record_campaign_progress,
    replace_campaign_songs,
    reset_campaign_progress,
    serialize_campaign,
    set_campaign_song_level,
)

router = APIRouter(prefix="/campaigns", tags=["campaigns"])
admin_router = APIRouter(prefix="/admin/campaigns", tags=["admin: campaigns"])

#: Origin-relative prefix of an uploaded cover; the client downloads
#: ``serverOrigin + cover_image_url``. Mirrored by the ``/media/campaigns``
#: static mount in ``app/main.py``.
CAMPAIGN_MEDIA_URL_PREFIX = "/media/campaigns/"
#: Cover formats accepted by the upload route. The extension selects the
#: signature check; the bytes always win over the client ``content_type``.
COVER_SUFFIXES = frozenset({".png", ".jpg", ".jpeg", ".webp"})


@router.get("", response_model=CampaignListResponse, summary="List playable campaigns")
def get_public_campaigns(db: Session = Depends(get_db)) -> CampaignListResponse:
    """Anonymous list for the landing page and the desktop client.

    Only published songs are returned, and a campaign whose whole list is
    unpublished is omitted until at least one song is published again. The
    reward fields are additive: ``reward_character_id`` is the public integer
    id, ``reward_character_name`` / ``reward_condition`` describe the unlock.
    """
    return CampaignListResponse(items=public_campaigns(db))


# ---------------------------------------------------------------------------
# Player progress (server-validated rewards)
# ---------------------------------------------------------------------------
@router.get(
    "/progress",
    response_model=CampaignProgressListResponse,
    summary="Server-side tower progress of the signed-in player",
)
def get_my_campaign_progress(
    db: Session = Depends(get_db),
    current_user: User = Depends(get_current_user),
) -> CampaignProgressListResponse:
    return CampaignProgressListResponse(
        items=campaign_progress_items(db, current_user)
    )


@router.post(
    "/{campaign_id}/progress",
    response_model=CampaignProgressResult,
    summary="Report the cleared stage count of a tower",
)
def report_campaign_progress(
    campaign_id: UUID,
    payload: CampaignProgressUpdate,
    db: Session = Depends(get_db),
    current_user: User = Depends(get_current_user),
) -> CampaignProgressResult:
    """Monotonic upsert of the player's progress in one campaign.

    The submitted ``best_stage`` is clamped to the number of **published**
    stages and never decreases. When the final stage is cleared the server
    grants ``campaigns.reward_character_id`` idempotently and reports it in
    ``unlocked_character`` (only on the transition) or ``already_owned``. A
    client cannot name a character: there is no field for it.
    """
    campaign = _require_campaign(db, campaign_id)
    return record_campaign_progress(db, current_user, campaign, payload.best_stage)


@router.post(
    "/{campaign_id}/reset",
    response_model=CampaignProgressResult,
    summary="Reset the current Battle run (start again at stage 0)",
)
def reset_campaign_run(
    campaign_id: UUID,
    db: Session = Depends(get_db),
    current_user: User = Depends(get_current_user),
) -> CampaignProgressResult:
    """Start a new Battle run on the server.

    The client's RESET button now starts a new run on the server: the raw
    ``best_stage`` counter is set back to ``0`` so the next reported stage is
    ``0`` again. ``completed_at`` and ``completion_count`` are history and
    anti-farm data and are kept, so nothing is re-granted and the repeat XP
    factor does not improve. No XP or rating changes and no reward is granted.

    Idempotent; a player who never played the tower gets no row. Unknown
    campaign → 404.
    """
    campaign = _require_campaign(db, campaign_id)
    return reset_campaign_progress(db, current_user, campaign)


@router.post(
    "/{campaign_id}/stages/{position}/result",
    response_model=CampaignStageResultResponse,
    summary="Report one Battle stage result (server-computed XP)",
)
def report_stage_result(
    campaign_id: UUID,
    position: int,
    payload: CampaignStageResultSubmit,
    db: Session = Depends(get_db),
    current_user: User = Depends(get_current_user),
) -> CampaignStageResultResponse:
    """Authoritative reward path of a Battle ("tower") run.

    The client sends facts only: score, accuracy, note qualities, completion and
    the played/total counters. XP comes from the existing solo formula times the
    tower repeat factor of the number of full completions before this run; the
    personal-best bonus is granted once for a strictly better accuracy.

    ``run_id`` is generated per attempt and unique per player, so retrying a
    failed report returns the stored row with ``already_processed=true`` and no
    new XP or progress. A stage can only be reported when it is the current one
    (``position == best_stage % stage_count``), which also makes a finished tower
    restart at stage 0 for its next run. A loss resets the run to stage 0 while
    ``completed_at`` and ``completion_count`` are kept.
    """
    campaign = _require_campaign(db, campaign_id)
    try:
        return submit_stage_result(db, current_user, campaign, position, payload)
    except StageResultError as exc:
        raise HTTPException(status_code=exc.status_code, detail=exc.code) from None


# ---------------------------------------------------------------------------
# Administrator API
# ---------------------------------------------------------------------------
@admin_router.get(
    "", response_model=AdminCampaignListResponse, summary="List all campaigns"
)
def get_admin_campaigns(
    db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> AdminCampaignListResponse:
    return AdminCampaignListResponse(
        items=[
            serialize_campaign(campaign, admin=True)
            for campaign in list_campaigns(db)
        ]
    )


def _validate_reward_character(db: Session, reward_character_id: UUID | None) -> None:
    """The reward must reference an existing character (admin-internal UUID)."""
    if reward_character_id is None:
        return
    if db.get(UiCharacter, reward_character_id) is None:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Unknown reward_character_id",
        )


@admin_router.post(
    "",
    response_model=AdminCampaignItem,
    status_code=status.HTTP_201_CREATED,
    summary="Create a campaign",
)
def create_campaign(
    payload: CampaignCreate,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminCampaignItem:
    ensure_unique_name(db, payload.name)
    _validate_reward_character(db, payload.reward_character_id)
    campaign = Campaign(
        name=payload.name,
        bot_level=payload.bot_level,
        reward_character_id=payload.reward_character_id,
        reward_condition=payload.reward_condition,
        description=payload.description,
    )
    db.add(campaign)
    try:
        db.commit()
    except IntegrityError:
        db.rollback()
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Campaign name already exists",
        ) from None
    db.refresh(campaign)
    return serialize_campaign(campaign, admin=True)


def _require_campaign(db: Session, campaign_id: UUID) -> Campaign:
    campaign = get_campaign(db, campaign_id)
    if campaign is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Campaign not found"
        )
    return campaign


def _campaign_cover_path(url: str | None) -> Path | None:
    """Map a ``/media/campaigns/<name>`` URL to a file inside the upload dir.

    Anything outside the prefix, a nested path or an escaping name is mapping
    to ``None``: the stored value is never trusted as a filesystem path.
    """
    if not url or not url.startswith(CAMPAIGN_MEDIA_URL_PREFIX):
        return None
    name = url[len(CAMPAIGN_MEDIA_URL_PREFIX):]
    if not name or "/" in name or "\\" in name or name in {".", ".."}:
        return None
    storage_root = settings.campaign_media_dir.resolve()
    target = (storage_root / name).resolve()
    if not target.is_relative_to(storage_root):
        return None
    return target


@admin_router.get(
    "/{campaign_id}", response_model=AdminCampaignItem, summary="Read one campaign"
)
def get_admin_campaign(
    campaign_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminCampaignItem:
    return serialize_campaign(_require_campaign(db, campaign_id), admin=True)


@admin_router.patch(
    "/{campaign_id}", response_model=AdminCampaignItem, summary="Update campaign metadata"
)
def update_campaign(
    campaign_id: UUID,
    payload: CampaignUpdate,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminCampaignItem:
    campaign = _require_campaign(db, campaign_id)
    changes = payload.model_dump(exclude_unset=True)
    if not changes:
        return serialize_campaign(campaign, admin=True)

    # ``name`` and ``bot_level`` are NOT NULL: an explicit null is a 422, not a
    # silent no-op (mirrors the song metadata endpoint). The reward fields are
    # optional and ``null`` clears them.
    if any(
        changes.get(key) is None
        for key in ("name", "bot_level")
        if key in changes
    ):
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Campaign name and bot_level cannot be null",
        )

    if "reward_character_id" in changes:
        _validate_reward_character(db, changes["reward_character_id"])

    if "name" in changes:
        ensure_unique_name(db, changes["name"], campaign_id=campaign.id)
    for key, value in changes.items():
        setattr(campaign, key, value)
    try:
        db.commit()
    except IntegrityError:
        db.rollback()
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Campaign name already exists",
        ) from None

    refreshed = _require_campaign(db, campaign_id)
    return serialize_campaign(refreshed, admin=True)


@admin_router.post(
    "/{campaign_id}/cover",
    response_model=AdminCampaignItem,
    status_code=status.HTTP_201_CREATED,
    summary="Upload or replace the campaign cover",
)
async def upload_campaign_cover(
    campaign_id: UUID,
    file: UploadFile = File(..., description="PNG, JPEG or WebP cover image"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminCampaignItem:
    """Store a cover image and expose it at ``/media/campaigns/<file>``.

    The type is decided by the file signature, never by the client
    ``content_type``: only PNG, JPEG and WebP are accepted (415 otherwise) and
    the configured size limit is enforced while streaming (413). Every upload
    gets a fresh uuid name, so a replaced cover has a *new* URL and clients that
    cache by URL pick it up; the previous file is removed after the new URL is
    committed.
    """
    campaign = _require_campaign(db, campaign_id)

    suffix = Path(file.filename or "").suffix.lower()
    media = IMAGE_SIGNATURES.get(suffix) if suffix in COVER_SUFFIXES else None
    if media is None:
        raise HTTPException(
            status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE,
            detail="Campaign cover must be PNG, JPEG or WebP",
        )
    _, _, matches_signature = media

    limit = settings.max_campaign_cover_bytes
    declared = getattr(file, "size", None)
    if declared is not None and declared > limit:
        raise HTTPException(
            status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
            detail="Uploaded file exceeds the configured size limit",
        )

    try:
        head = await file.read(64)
        if not matches_signature(head):
            raise HTTPException(
                status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE,
                detail="File content does not match its extension",
            )

        stored_name = f"cover-{uuid.uuid4()}{suffix}"
        target = _safe_upload_path(settings.campaign_media_dir, stored_name)
        await _write_upload(file, target, limit, prefix=head)

        previous_path = _campaign_cover_path(campaign.cover_image_url)
        campaign.cover_image_url = f"{CAMPAIGN_MEDIA_URL_PREFIX}{stored_name}"
        try:
            db.commit()
        except Exception:
            db.rollback()
            _remove_media_file(target)
            raise
        # Remove the replaced local file only after the new URL is committed.
        _remove_media_file(previous_path)
        return serialize_campaign(_require_campaign(db, campaign_id), admin=True)
    finally:
        await file.close()


@admin_router.delete(
    "/{campaign_id}/cover",
    response_model=AdminCampaignItem,
    summary="Remove the campaign cover",
)
def delete_campaign_cover(
    campaign_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminCampaignItem:
    """Delete the stored image, clear the column and return the payload.

    Idempotent: a campaign without a cover is returned unchanged.
    """
    campaign = _require_campaign(db, campaign_id)
    previous_path = _campaign_cover_path(campaign.cover_image_url)
    campaign.cover_image_url = None
    db.commit()
    _remove_media_file(previous_path)
    return serialize_campaign(_require_campaign(db, campaign_id), admin=True)


@admin_router.delete(
    "/{campaign_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete a campaign",
)
def delete_campaign(
    campaign_id: UUID,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> Response:
    campaign = _require_campaign(db, campaign_id)
    cover_path = _campaign_cover_path(campaign.cover_image_url)
    db.delete(campaign)
    db.commit()
    _remove_media_file(cover_path)
    return Response(status_code=status.HTTP_204_NO_CONTENT)


@admin_router.put(
    "/{campaign_id}/songs",
    response_model=AdminCampaignItem,
    summary="Replace the ordered song list of a campaign",
)
def put_campaign_songs(
    campaign_id: UUID,
    payload: CampaignSongsUpdate,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminCampaignItem:
    """Replace the stage list atomically.

    Two accepted shapes (exactly one, otherwise 422):

    * ``{"songs": [{"song_id": ..., "bot_level": 0..2}, ...]}`` — per-stage
      difficulty; a missing level falls back to the campaign default.
    * ``{"song_ids": [...]}`` — legacy form; existing per-stage levels are
      preserved and new songs get the campaign default.

    Array order = play order. Duplicate ids → 422, unknown song → 404, and in
    both cases the previous list is left untouched. An empty list clears the
    campaign.
    """
    campaign = _require_campaign(db, campaign_id)
    if payload.songs is not None:
        entries = [(item.song_id, item.bot_level) for item in payload.songs]
    else:
        entries = legacy_campaign_entries(campaign, payload.song_ids or [])
    updated = replace_campaign_songs(db, campaign, entries)
    return serialize_campaign(updated, admin=True)


@admin_router.patch(
    "/{campaign_id}/songs/{song_id}",
    response_model=AdminCampaignItem,
    summary="Change the bot difficulty of one stage",
)
def patch_campaign_song_level(
    campaign_id: UUID,
    song_id: UUID,
    payload: CampaignSongLevelUpdate,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> AdminCampaignItem:
    """Set one stage's ``bot_level`` without touching the order or other stages.

    A song that is not a stage of this campaign → 404. Level outside ``0..2`` →
    422. ``campaigns.updated_at`` is bumped.
    """
    campaign = _require_campaign(db, campaign_id)
    updated = set_campaign_song_level(db, campaign, song_id, payload.bot_level)
    return serialize_campaign(updated)

