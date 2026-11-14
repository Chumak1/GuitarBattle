"""Online-duel and Battle ("Tower riff") aggregates for the admin panel.

Two callers share these numbers:

* ``GET /admin/users/{user_id}/overview`` — one full card per account;
* ``GET /admin/statistics/users`` — six compact columns for the users table,
  fetched for the current page only with one grouped query per group (never
  one query per row).

The rules match the player-facing endpoints, so the panel never contradicts
what a player sees:

* online: only ``mode = 'online'`` matches with ``status = 'finished'`` and
  human participants (``duel_participants.is_bot = false``). The rating is the
  newest non-NULL ``rating_after`` (``duel_service._latest_rating_subquery``),
  the accuracy is the quality-points total over all played notes — the same
  weights :func:`app.services.duel_rules.accuracy_factor` uses;
* battle: ``user_campaign_progress.best_stage`` clamped to the number of
  *published* stages (``campaigns.public_stage_count``), the same clamp
  :func:`app.services.campaigns.record_campaign_progress` applies. The XP is
  the sum of the stored ``campaign_stage_runs.xp_awarded``.
"""
from __future__ import annotations

from uuid import UUID

from sqlalchemy import case, func, select
from sqlalchemy.orm import Session

from app.models.campaign import Campaign, CampaignSong, CampaignStageRun
from app.models.character_unlocks import UserCampaignProgress
from app.models.duel import DuelMatch, DuelParticipant
from app.models.song import Song
from app.services.campaigns import campaign_query, public_stage_count
from app.services.duel_service import _latest_rating_subquery

#: Quality points per resolved note. Kept in sync with
#: :func:`app.services.duel_rules.accuracy_factor` and the player leaderboard.
QUALITY_POINTS = (
    DuelParticipant.perfect_count * 100
    + DuelParticipant.great_count * 85
    + DuelParticipant.good_count * 70
)

#: A match counts for a human only when it was a real online duel that finished.
ONLINE_MATCH_FILTERS = (
    DuelMatch.mode == "online",
    DuelMatch.status == "finished",
    DuelParticipant.is_bot.is_(False),
)

#: Published stages of a campaign: a stage whose song is archived or unpublished
#: is not playable, so it never counts towards cleared stages.
_PUBLISHED_STAGE_COUNTS = (
    select(
        CampaignSong.campaign_id.label("campaign_id"),
        func.count(CampaignSong.id).label("stage_count"),
    )
    .join(Song, Song.id == CampaignSong.song_id)
    .where(Song.is_published.is_(True))
    .group_by(CampaignSong.campaign_id)
    .subquery()
)


def _match_order():
    """Chronological order used for the win-streak scan (same as the profile)."""
    return func.coalesce(DuelMatch.finished_at, DuelMatch.created_at)


def online_stats(db: Session, *, user_id: UUID) -> dict:
    """Totals of one account's finished online duels (bot matches excluded)."""
    row = db.execute(
        select(
            func.count(DuelParticipant.id).label("matches"),
            func.sum(case((DuelParticipant.result == "win", 1), else_=0)).label(
                "wins"
            ),
            func.sum(case((DuelParticipant.result == "loss", 1), else_=0)).label(
                "losses"
            ),
            func.sum(case((DuelParticipant.result == "draw", 1), else_=0)).label(
                "draws"
            ),
            func.sum(DuelParticipant.xp_awarded).label("xp"),
            func.sum(QUALITY_POINTS).label("points"),
            func.sum(DuelParticipant.notes_played).label("notes"),
            func.sum(DuelParticipant.damage_dealt).label("damage"),
            func.max(_match_order()).label("last_match_at"),
        )
        .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
        .where(DuelParticipant.user_id == user_id, *ONLINE_MATCH_FILTERS)
    ).one()

    # The newest non-NULL rating, exactly like ``duel_service.current_rating``;
    # ``NULL`` here means the player never finished a rated duel.
    latest_rating = _latest_rating_subquery()
    rating = db.scalar(
        select(latest_rating.c.rating).where(latest_rating.c.user_id == user_id)
    )

    results = db.scalars(
        select(DuelParticipant.result)
        .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
        .where(DuelParticipant.user_id == user_id, *ONLINE_MATCH_FILTERS)
        .order_by(_match_order().asc(), DuelMatch.id.asc())
    ).all()
    best_streak = 0
    current = 0
    for value in results:
        if value == "win":
            current += 1
            best_streak = max(best_streak, current)
        else:
            current = 0

    matches = int(row.matches or 0)
    wins = int(row.wins or 0)
    notes = int(row.notes or 0)
    return {
        "matches": matches,
        "wins": wins,
        "losses": int(row.losses or 0),
        "draws": int(row.draws or 0),
        "win_rate": round(100.0 * wins / matches, 2) if matches else 0.0,
        "rating": int(rating) if rating is not None else None,
        "best_win_streak": best_streak,
        "accuracy": (
            round(float(row.points or 0) / (notes * 100.0) * 100.0, 2)
            if notes
            else 0.0
        ),
        "damage_dealt": round(float(row.damage or 0.0), 2),
        "xp_earned": int(row.xp or 0),
        "last_match_at": row.last_match_at,
    }


def battle_stats(db: Session, *, user_id: UUID) -> dict:
    """Totals of one account's Battle runs plus the per-tower progress list."""
    run_row = db.execute(
        select(
            func.count(CampaignStageRun.id).label("runs"),
            func.coalesce(func.sum(CampaignStageRun.xp_awarded), 0).label("xp"),
            func.max(CampaignStageRun.created_at).label("last_run_at"),
        ).where(CampaignStageRun.user_id == user_id)
    ).one()

    progress_rows = db.execute(
        select(
            UserCampaignProgress.campaign_id,
            UserCampaignProgress.best_stage,
            UserCampaignProgress.completion_count,
            UserCampaignProgress.completed_at,
        )
        .where(UserCampaignProgress.user_id == user_id)
        .order_by(UserCampaignProgress.campaign_id.asc())
    ).all()

    campaigns: dict[UUID, Campaign] = {}
    if progress_rows:
        campaigns = {
            campaign.id: campaign
            for campaign in db.scalars(
                campaign_query().where(
                    Campaign.id.in_([row.campaign_id for row in progress_rows])
                )
            ).all()
        }

    towers: list[dict] = []
    towers_completed = 0
    stages_cleared = 0
    for row in progress_rows:
        campaign = campaigns.get(row.campaign_id)
        if campaign is None:  # progress cascades with its campaign; defensive
            continue
        stage_count = public_stage_count(campaign)
        best_stage = int(row.best_stage or 0)
        if stage_count > 0:
            best_stage = min(best_stage, stage_count)
            stages_cleared += best_stage
        if row.completed_at is not None:
            towers_completed += 1
        towers.append(
            {
                "campaign_id": campaign.id,
                "name": campaign.name,
                "best_stage": best_stage,
                "stage_count": stage_count,
                "completions": int(row.completion_count or 0),
                "completed_at": row.completed_at,
            }
        )
    # ``progress_rows`` is ordered by id, which is stable but meaningless to a
    # human reader; the panel shows towers alphabetically.
    towers.sort(key=lambda item: (item["name"].casefold(), str(item["campaign_id"])))

    return {
        "towers_completed": towers_completed,
        "stages_cleared": stages_cleared,
        "runs": int(run_row.runs or 0),
        "xp_earned": int(run_row.xp or 0),
        "last_run_at": run_row.last_run_at,
        "towers": towers,
    }


def online_summary_by_user(
    db: Session, user_ids: list[UUID]
) -> dict[UUID, dict]:
    """Online rating and W/L counts for the users on one page of the table.

    One ``GROUP BY`` statement for the whole page; the rating comes from the
    same window-function subquery the leaderboard uses, joined once per user.
    """
    if not user_ids:
        return {}

    latest_rating = _latest_rating_subquery()
    rows = db.execute(
        select(
            DuelParticipant.user_id,
            func.count(DuelParticipant.id).label("matches"),
            func.sum(case((DuelParticipant.result == "win", 1), else_=0)).label(
                "wins"
            ),
            func.sum(case((DuelParticipant.result == "loss", 1), else_=0)).label(
                "losses"
            ),
            func.max(latest_rating.c.rating).label("rating"),
        )
        .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
        .outerjoin(latest_rating, latest_rating.c.user_id == DuelParticipant.user_id)
        .where(
            DuelParticipant.user_id.in_(user_ids),
            DuelParticipant.user_id.is_not(None),
            *ONLINE_MATCH_FILTERS,
        )
        .group_by(DuelParticipant.user_id)
    ).all()

    return {
        row.user_id: {
            "online_rating": int(row.rating) if row.rating is not None else None,
            "online_matches": int(row.matches or 0),
            "online_wins": int(row.wins or 0),
            "online_losses": int(row.losses or 0),
        }
        for row in rows
    }


def battle_summary_by_user(
    db: Session, user_ids: list[UUID]
) -> dict[UUID, dict]:
    """Completed towers and cleared stages for the users on one page.

    ``LEAST(best_stage, stage_count)`` mirrors the clamp of
    ``record_campaign_progress``, and a campaign without a published stage
    contributes nothing. One grouped statement for the whole page.
    """
    if not user_ids:
        return {}

    rows = db.execute(
        select(
            UserCampaignProgress.user_id,
            func.sum(
                func.least(
                    UserCampaignProgress.best_stage,
                    func.coalesce(_PUBLISHED_STAGE_COUNTS.c.stage_count, 0),
                )
            ).label("stages_cleared"),
            func.sum(
                case((UserCampaignProgress.completed_at.is_not(None), 1), else_=0)
            ).label("towers_completed"),
        )
        .outerjoin(
            _PUBLISHED_STAGE_COUNTS,
            _PUBLISHED_STAGE_COUNTS.c.campaign_id
            == UserCampaignProgress.campaign_id,
        )
        .where(UserCampaignProgress.user_id.in_(user_ids))
        .group_by(UserCampaignProgress.user_id)
    ).all()

    return {
        row.user_id: {
            "battle_towers_completed": int(row.towers_completed or 0),
            "battle_stages_cleared": int(row.stages_cleared or 0),
        }
        for row in rows
    }
