"""Battle ("Tower riffs") stage results: normalization, progress and XP.

This module is the single reward path of a Battle run. It deliberately reuses
the existing rules instead of re-implementing them:

* ``progression.effective_completion`` / ``progression.normalized_score`` for
  the played fraction and the extrapolated score;
* ``progression.xp_for_attempt`` (difficulty, tempo, content, ``repeat_multiplier``)
  — the formula and its coefficients are untouched;
* ``progression.award_attempt_achievements`` for the achievement block, so a
  real tower win can unlock achievements exactly like a solo attempt;
* ``campaigns.public_stage_count`` and the character service for the reward.

Anti-farm rules implemented here:

* ``run_id`` is unique per player: a retried report returns the stored row with
  ``already_processed=true`` and never adds XP or progress again.
* XP is recomputed from the payload; the client cannot send an amount.
* only a **won** stage with ``completion >= MIN_COMPLETION_FOR_XP`` earns XP;
* the repeat factor decays with the number of **full** tower completions before
  this run (``reward_config.tower_repeat_factor``), so every stage of the first
  run is full XP, the second run is halved, and so on;
* the PB bonus is granted at most once per run and only when the accuracy
  strictly improves the stored best of that campaign stage.

A lost stage ends the run: ``best_stage`` drops back to ``0`` so the next
attempt starts at the first opponent. ``completed_at`` and ``completion_count``
are history/anti-farm data and are never reset.
"""
from __future__ import annotations

import math
from datetime import datetime, timezone
from typing import Any

from sqlalchemy import func, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.models.campaign import Campaign, CampaignStageRun
from app.models.character_unlocks import UserCampaignProgress
from app.models.progression import DailyResult
from app.models.song import Song
from app.models.user import User
from app.schemas.campaigns import (
    CampaignStageResultResponse,
    CampaignStageResultSubmit,
)
from app.schemas.gameplay_visuals import PublicCharacterResponse
from app.services import progression, reward_config
from app.services.campaigns import displayed_stage, public_stage_count
from app.services.characters import grant_character
from app.services.gameplay_media import character_sheet_url

#: Rejected stage result (mapped to a 409 by the endpoint).
STAGE_NOT_CURRENT = "stage_not_current"
NO_STAGES = "no_stages"


class StageResultError(Exception):
    """A Battle run that cannot be accepted; ``code`` is the API detail."""

    def __init__(self, status_code: int, code: str) -> None:
        super().__init__(code)
        self.status_code = status_code
        self.code = code


def published_stages(campaign: Campaign) -> list[tuple[int, Song]]:
    """Playable stages as ``(index, song)`` in the public order.

    The index is the 0-based position among *published* stages — exactly what
    the client plays and what ``campaignStage`` counts. That is why a tower
    losing a published stage can never shift the accepted position while a
    run is in progress beyond a 409.
    """
    stages: list[tuple[int, Song]] = []
    for link in sorted(campaign.songs, key=lambda item: item.position):
        if link.song is not None and link.song.is_published:
            stages.append((len(stages), link.song))
    return stages


def _progress_row(db: Session, user: User, campaign: Campaign) -> UserCampaignProgress | None:
    return db.scalar(
        select(UserCampaignProgress)
        .where(
            UserCampaignProgress.user_id == user.id,
            UserCampaignProgress.campaign_id == campaign.id,
        )
        .with_for_update()
    )


def _completed_before(row: UserCampaignProgress | None, stage_count: int) -> bool:
    if row is None or stage_count <= 0:
        return False
    return bool(row.completed_at is not None or int(row.best_stage) >= stage_count)


def _response_from_run(
    db: Session,
    user: User,
    campaign: Campaign,
    run: CampaignStageRun,
    stage_count: int,
    *,
    already_processed: bool,
    xp_total: int,
    level: int,
    level_up: bool,
    unlocked: PublicCharacterResponse | None,
) -> CampaignStageResultResponse:
    progress = db.scalar(
        select(UserCampaignProgress).where(
            UserCampaignProgress.user_id == user.id,
            UserCampaignProgress.campaign_id == campaign.id,
        )
    )
    best_raw = int(progress.best_stage) if progress is not None else run.position
    completed = _completed_before(progress, stage_count)
    return CampaignStageResultResponse(
        run_id=run.run_id,
        already_processed=already_processed,
        won=bool(run.won),
        stage_count=stage_count,
        # The stage of the current run: ``best_stage % stage_count`` (a
        # finished tower reports ``stage_count``). Never clamp the raw counter
        # with ``min``: that would turn 4 cleared stages of a 3-stage tower into
        # "finished" instead of stage 1.
        best_stage=displayed_stage(best_raw, stage_count),
        completed=completed,
        score_points=int(run.score_points),
        normalized_score=run.normalized_score,
        completion_percent=float(run.completion_percent),
        # The amount this run was worth (already awarded once). A replay does
        # not add it again: ``xp_total`` is unchanged and ``already_processed``
        # is true. Returning the stored amount keeps a retried report from
        # showing a misleading zero on the result card.
        xp_earned=int(run.xp_awarded),
        xp_breakdown=None,
        xp_total=xp_total,
        level=level,
        level_up=level_up,
        pb_improved=bool(run.pb_bonus),
        repeat_factor=float(run.repeat_factor),
        unlocked_character=unlocked,
    )


def _stored_response(
    db: Session,
    user: User,
    campaign: Campaign,
    run: CampaignStageRun,
    stage_count: int,
) -> CampaignStageResultResponse:
    """Idempotent replay: the stored row, no XP and no progress change."""
    xp_total = int(user.xp_total or 0)
    return _response_from_run(
        db,
        user,
        campaign,
        run,
        stage_count,
        already_processed=True,
        xp_total=xp_total,
        level=progression.level_for_xp(xp_total),
        level_up=False,
        unlocked=None,
    )


def submit_stage_result(
    db: Session,
    user: User,
    campaign: Campaign,
    position: int,
    payload: CampaignStageResultSubmit,
) -> CampaignStageResultResponse:
    """Validate, store and reward one Battle stage; caller gets the answer."""
    stages = published_stages(campaign)
    stage_count = public_stage_count(campaign)

    if stage_count <= 0 or len(stages) != stage_count:
        raise StageResultError(409, NO_STAGES)

    existing = db.scalar(
        select(CampaignStageRun).where(
            CampaignStageRun.user_id == user.id,
            CampaignStageRun.run_id == payload.run_id,
        )
    )
    if existing is not None:
        return _stored_response(db, user, campaign, existing, stage_count)

    if position < 0 or position >= stage_count:
        raise StageResultError(409, STAGE_NOT_CURRENT)

    # Serialise per player: XP, level and the PB comparison are read-modify-write.
    locked_user = db.scalar(
        select(User)
        .where(User.id == user.id)
        .with_for_update()
        .execution_options(populate_existing=True)
    )
    if locked_user is None:  # pragma: no cover - the JWT user must exist
        raise StageResultError(403, "unknown_user")
    user = locked_user

    progress = _progress_row(db, user, campaign)
    best_raw = int(progress.best_stage) if progress is not None else 0
    completion_count_before = int(progress.completion_count or 0) if progress else 0

    # The stage currently being played. A finished tower continues counting
    # (``best_stage % stage_count``), so replaying it starts at stage 0 again
    # without losing the completion record.
    current_position = best_raw % stage_count
    if position != current_position:
        raise StageResultError(409, STAGE_NOT_CURRENT)

    song = stages[position][1]

    completion = progression.effective_completion(
        payload.completion_percent, payload.notes_total, payload.notes_played
    )
    normalized = progression.normalized_score(
        score_points=payload.score_points,
        completion_percent=payload.completion_percent,
        notes_total=payload.notes_total,
        notes_played=payload.notes_played,
    )

    repeat = reward_config.tower_repeat_factor(completion_count_before)

    # Base XP: the existing formula only. A lost stage never earns tower XP,
    # and the completion threshold is checked by ``xp_for_attempt`` itself.
    base_xp = (
        progression.xp_for_attempt(
            accuracy_percent=float(payload.accuracy_percent),
            difficulty=int(song.difficulty),
            bpm=int(song.bpm or 0),
            duration_seconds=int(song.duration_seconds or 0),
            completion_percent=completion,
            repeat_multiplier=repeat,
            notes_total=payload.notes_total,
            notes_played=payload.notes_played,
        )
        if payload.won
        else 0
    )

    previous_best = db.scalar(
        select(func.max(CampaignStageRun.accuracy_percent)).where(
            CampaignStageRun.user_id == user.id,
            CampaignStageRun.campaign_id == campaign.id,
            CampaignStageRun.position == position,
        )
    )
    pb_improved = (
        previous_best is not None
        and float(payload.accuracy_percent) > float(previous_best)
    )
    pb_bonus = (
        int(math.floor(base_xp * reward_config.PB_BONUS_FACTOR + 0.5))
        if pb_improved and base_xp > 0
        else 0
    )
    xp_earned = base_xp + pb_bonus

    level_before = progression.level_for_xp(int(user.xp_total or 0))

    run = CampaignStageRun(
        user_id=user.id,
        campaign_id=campaign.id,
        position=position,
        run_id=payload.run_id,
        score_points=int(payload.score_points),
        normalized_score=normalized,
        accuracy_percent=float(payload.accuracy_percent),
        perfect_count=int(payload.perfect_count),
        great_count=int(payload.great_count),
        good_count=int(payload.good_count),
        miss_count=int(payload.miss_count),
        completion_percent=completion,
        notes_total=payload.notes_total,
        notes_played=payload.notes_played,
        won=bool(payload.won),
        xp_awarded=xp_earned,
        repeat_factor=repeat,
        pb_bonus=pb_bonus,
    )
    db.add(run)
    db.flush()

    now = datetime.now(timezone.utc)
    completed = False
    unlocked: PublicCharacterResponse | None = None

    if payload.won:
        if progress is None:
            progress = UserCampaignProgress(
                user_id=user.id,
                campaign_id=campaign.id,
                best_stage=0,
                completion_count=0,
            )
            db.add(progress)

        new_best = best_raw + 1
        completed = stage_count > 0 and new_best % stage_count == 0
        # Store the raw counter; the read paths report ``best_stage % stage_count``
        # (see ``campaigns.displayed_stage``) so a replay stays on its own run.
        progress.best_stage = new_best
        progress.updated_at = now

        if completed:
            if progress.completed_at is None:
                progress.completed_at = now
            progress.completion_count = int(progress.completion_count or 0) + 1

            reward = campaign.reward_character
            if reward is not None and grant_character(
                db, user, reward, source_campaign_id=campaign.id
            ):
                unlocked = PublicCharacterResponse(
                    id=reward.public_id,
                    name=reward.name,
                    sheet_url=character_sheet_url(reward.sheet_key),
                )
    elif progress is not None:
        # A lost stage ends the run: the next attempt starts at the first
        # opponent. ``completed_at`` and ``completion_count`` are history and
        # anti-farm data, so they survive the reset.
        progress.best_stage = 0
        progress.updated_at = now

    awarded: list[dict[str, Any]] = []
    bonus = 0
    if payload.won:
        # Same rules as a solo attempt, facts built from the run. A lost stage
        # is not a completed attempt and never grants achievement XP either.
        daily_result_count = int(
            db.scalar(
                select(func.count(DailyResult.challenge_date)).where(
                    DailyResult.user_id == user.id
                )
            )
            or 0
        )
        battle_qualifying = int(
            db.scalar(
                select(func.count(CampaignStageRun.id)).where(
                    CampaignStageRun.user_id == user.id,
                    CampaignStageRun.completion_percent
                    >= progression.MIN_COMPLETION_FOR_XP,
                )
            )
            or 0
        )
        awarded, bonus = progression.award_attempt_achievements(
            db,
            user,
            completion_percent=completion,
            accuracy_percent=float(payload.accuracy_percent),
            miss_count=int(payload.miss_count),
            avg_timing_error_ms=None,
            difficulty=int(song.difficulty),
            is_daily=False,
            personal_best_updated=pb_improved,
            previous_best_accuracy=(
                float(previous_best) if previous_best is not None else None
            ),
            daily_result_count=daily_result_count,
            now=now,
            additional_qualifying_attempts=battle_qualifying,
        )

    total = int(user.xp_total or 0) + xp_earned + bonus
    user.xp_total = total
    user.level = progression.level_for_xp(total)
    db.flush()

    breakdown: dict[str, Any] = progression.xp_breakdown(
        accuracy_percent=float(payload.accuracy_percent),
        difficulty=int(song.difficulty),
        bpm=int(song.bpm or 0),
        duration_seconds=int(song.duration_seconds or 0),
        completion_percent=completion,
        repeat_multiplier=repeat,
        attempts_today=completion_count_before,
    )
    breakdown.update(
        {
            "reward_kind": "battle",
            "stage": position,
            "won": bool(payload.won),
            "tower_completions_before": completion_count_before,
            "tower_repeat_factor": round(repeat, 4),
            "base_xp": base_xp,
            "pb_bonus": pb_bonus,
            "pb_improved": pb_improved,
            "normalized_score": normalized,
            "achievement_xp": bonus,
            "achievements": [item["code"] for item in awarded],
        }
    )

    try:
        db.commit()
    except IntegrityError:
        # A concurrent retry of the same run_id won the race: return its row.
        db.rollback()
        existing = db.scalar(
            select(CampaignStageRun).where(
                CampaignStageRun.user_id == user.id,
                CampaignStageRun.run_id == payload.run_id,
            )
        )
        if existing is None:  # pragma: no cover - the only unique key is run_id
            raise
        db.refresh(user)
        return _stored_response(db, user, campaign, existing, stage_count)

    return CampaignStageResultResponse(
        run_id=run.run_id,
        already_processed=False,
        won=bool(payload.won),
        stage_count=stage_count,
        best_stage=displayed_stage(
            int(progress.best_stage) if progress else 0, stage_count
        ),
        completed=completed or _completed_before(progress, stage_count),
        score_points=int(payload.score_points),
        normalized_score=normalized,
        completion_percent=completion,
        xp_earned=xp_earned,
        xp_breakdown=breakdown,
        xp_total=total,
        level=user.level,
        level_up=user.level > level_before,
        pb_improved=pb_improved,
        repeat_factor=repeat,
        unlocked_character=unlocked,
    )
