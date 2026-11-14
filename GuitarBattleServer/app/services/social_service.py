"""Server-side rules for the "Friends" social slice.

This module is the single source of truth for the social graph. The API layer
only translates HTTP to calls here.

Design notes
------------
* **No parallel identity or rating.** A player card aggregates ``users``,
  ``duel_participants``/``duel_matches`` (rating, peak rating, duel totals and
  accuracy), ``song_best_scores``, ``user_achievements``/``achievements``,
  ``user_campaign_progress`` and ``user_characters``/``ui_characters``. Nothing
  is copied into the social tables.
* **Friendship is mutual and stored once.** The canonical row is
  ``(min(a, b), max(a, b))``; the database check constraint enforces the order.
* **Presence is derived from real data only.** ``duel_participants.last_seen_at``
  within :data:`PRESENCE_ONLINE_WINDOW` is *online*; an active waiting match or
  duel lobby is *in_lobby*, a playing match is *in_match*. A player who turned
  ``show_online`` off is reported as *hidden* to everybody else. Presence is
  never invented from a login or an HTTP request.
* **Ownership is enforced here.** Accept/decline check the addressee, cancel
  checks the sender, block/unblock and rival changes always act on the caller.
* **Notifications are idempotent.** ``UNIQUE(user_id, dedupe_key)`` plus
  ``ON CONFLICT DO NOTHING`` mean a retried mutation never delivers twice.
"""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
from typing import Any, Iterable, Sequence
from uuid import UUID

from sqlalchemy import case, func, or_, select, update
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.models.character_unlocks import UserCampaignProgress
from app.models.duel import DuelLobby, DuelMatch, DuelParticipant
from app.models.gameplay_visuals import UiCharacter
from app.models.guitar_skills import UserGuitarSkills
from app.models.progression import Achievement, SongBestScore, UserAchievement
from app.models.social import (
    FriendRequest,
    Friendship,
    PlayerActivity,
    SocialNotification,
    UserBlock,
    UserPrivacy,
    UserRival,
)
from app.models.user import User
from app.services import duel_rules
from app.services.duel_service import _latest_rating_subquery
from app.services.gameplay_media import character_sheet_url

#: How long after the last heartbeat a duel participant still counts as online.
PRESENCE_ONLINE_WINDOW = timedelta(seconds=90)

#: A missing ``user_privacy`` row means these values (everything visible).
PRIVACY_DEFAULTS: dict[str, Any] = {
    "request_policy": "everyone",
    "show_online": True,
    "show_profile": True,
    "show_stats": True,
    "show_activity": True,
    "allow_challenges": True,
}

REQUEST_POLICIES = ("everyone", "friends", "nobody")
ACTIVITY_KINDS = (
    "record",
    "tower",
    "character",
    "level",
    "duel_win",
    "achievement",
)


class SocialError(Exception):
    """A domain error with an HTTP status and a stable machine code."""

    def __init__(self, status_code: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.code = code
        self.message = message

    def detail(self) -> dict[str, str]:
        return {"code": self.code, "message": self.message}


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def _now() -> datetime:
    return datetime.now(timezone.utc)


def _aware(value: datetime | None) -> datetime | None:
    if value is None:
        return None
    return value if value.tzinfo is not None else value.replace(tzinfo=timezone.utc)


def _unique(ids: Iterable[UUID]) -> list[UUID]:
    return list(dict.fromkeys(ids))


def active_user(db: Session, user_id: UUID) -> User:
    user = db.get(User, user_id)
    if user is None or not user.is_active:
        raise SocialError(404, "user_not_found", "Player not found")
    return user


def _pair(a: UUID, b: UUID) -> tuple[UUID, UUID]:
    """Canonical friendship key: the smaller UUID first (matches the DB rule)."""
    return (a, b) if str(a) < str(b) else (b, a)


def _like_pattern(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
    return f"%{escaped}%"


# ---------------------------------------------------------------------------
# Privacy
# ---------------------------------------------------------------------------
def privacy_row(db: Session, user_id: UUID) -> UserPrivacy | None:
    return db.get(UserPrivacy, user_id)


def privacy_settings(db: Session, user_id: UUID) -> dict[str, Any]:
    """The effective settings; a missing row is "all defaults"."""
    row = privacy_row(db, user_id)
    if row is None:
        return dict(PRIVACY_DEFAULTS)
    return {
        "request_policy": row.request_policy,
        "show_online": bool(row.show_online),
        "show_profile": bool(row.show_profile),
        "show_stats": bool(row.show_stats),
        "show_activity": bool(row.show_activity),
        "allow_challenges": bool(row.allow_challenges),
    }


def ensure_privacy_row(db: Session, user_id: UUID) -> UserPrivacy:
    """Return the settings row, creating the default one on first use.

    ``ON CONFLICT DO NOTHING`` makes two concurrent first requests safe: the
    loser of the race simply reads the row that already exists.
    """
    row = privacy_row(db, user_id)
    if row is None:
        db.execute(
            pg_insert(UserPrivacy.__table__)
            .values(user_id=user_id)
            .on_conflict_do_nothing(index_elements=["user_id"])
        )
        db.flush()
        db.expire_all()
        row = db.get(UserPrivacy, user_id)
        if row is None:  # pragma: no cover - impossible after the upsert
            raise SocialError(
                500, "privacy_unavailable", "Could not load privacy settings"
            )
    return row


def update_privacy(
    db: Session, user_id: UUID, changes: dict[str, Any]
) -> dict[str, Any]:
    row = ensure_privacy_row(db, user_id)
    for key, value in changes.items():
        if value is None or key not in PRIVACY_DEFAULTS:
            continue
        if key == "request_policy" and value not in REQUEST_POLICIES:
            raise SocialError(
                422, "invalid_request_policy", "Unknown request policy"
            )
        setattr(row, key, value)
    db.commit()
    db.refresh(row)
    return privacy_settings(db, user_id)


def _show_activity_ids(db: Session, user_ids: Sequence[UUID]) -> set[UUID]:
    """Of ``user_ids``, those whose activity is allowed to be seen."""
    ids = _unique(user_ids)
    if not ids:
        return set()
    hidden = set(
        db.scalars(
            select(UserPrivacy.user_id).where(
                UserPrivacy.user_id.in_(ids),
                UserPrivacy.show_activity.is_(False),
            )
        ).all()
    )
    return {user_id for user_id in ids if user_id not in hidden}


def _show_stats(privacy: dict[str, Any], viewer_id: UUID, target_id: UUID) -> bool:
    return viewer_id == target_id or bool(privacy["show_stats"])


# ---------------------------------------------------------------------------
# Friendship / block primitives
# ---------------------------------------------------------------------------
def friend_ids(db: Session, user_id: UUID) -> list[UUID]:
    """Every friend of ``user_id``, from both columns of the canonical pair."""
    rows = db.execute(
        select(Friendship.user_id, Friendship.friend_id).where(
            or_(Friendship.user_id == user_id, Friendship.friend_id == user_id)
        )
    ).all()
    return [
        friend_id if left == user_id else left
        for left, friend_id in rows
        if (friend_id if left == user_id else left) != user_id
    ]


def are_friends(db: Session, a: UUID, b: UUID) -> bool:
    if a == b:
        return False
    user_id, friend_id = _pair(a, b)
    return (
        db.scalar(
            select(Friendship.id).where(
                Friendship.user_id == user_id,
                Friendship.friend_id == friend_id,
            )
        )
        is not None
    )


def block_exists(db: Session, a: UUID, b: UUID) -> bool:
    """True when either side blocked the other."""
    if a == b:
        return False
    return (
        db.scalar(
            select(UserBlock.id).where(
                or_(
                    (UserBlock.blocker_id == a) & (UserBlock.blocked_id == b),
                    (UserBlock.blocker_id == b) & (UserBlock.blocked_id == a),
                )
            )
        )
        is not None
    )


def blocked_user_ids(db: Session, user_id: UUID) -> set[UUID]:
    rows = db.execute(
        select(UserBlock.blocker_id, UserBlock.blocked_id).where(
            or_(
                UserBlock.blocker_id == user_id,
                UserBlock.blocked_id == user_id,
            )
        )
    ).all()
    return {
        other
        for blocker, blocked in rows
        for other in (blocker, blocked)
        if other != user_id
    }


def relationship(db: Session, viewer_id: UUID, target_id: UUID) -> str:
    if viewer_id == target_id:
        return "self"
    if block_exists(db, viewer_id, target_id):
        return "blocked"
    if are_friends(db, viewer_id, target_id):
        return "friend"
    pending = db.scalar(
        select(FriendRequest).where(
            FriendRequest.status == "pending",
            or_(
                (FriendRequest.from_user_id == target_id)
                & (FriendRequest.to_user_id == viewer_id),
                (FriendRequest.from_user_id == viewer_id)
                & (FriendRequest.to_user_id == target_id),
            ),
        )
    )
    if pending is not None:
        return "incoming" if pending.from_user_id == target_id else "outgoing"
    return "none"


def _pending_request(
    db: Session, from_user_id: UUID, to_user_id: UUID
) -> FriendRequest | None:
    return db.scalar(
        select(FriendRequest).where(
            FriendRequest.status == "pending",
            or_(
                (FriendRequest.from_user_id == from_user_id)
                & (FriendRequest.to_user_id == to_user_id),
                (FriendRequest.from_user_id == to_user_id)
                & (FriendRequest.to_user_id == from_user_id),
            ),
        )
    )


def _cancel_pending_requests(
    db: Session, a: UUID, b: UUID, *, now: datetime
) -> None:
    db.execute(
        update(FriendRequest)
        .where(
            FriendRequest.status == "pending",
            or_(
                (FriendRequest.from_user_id == a) & (FriendRequest.to_user_id == b),
                (FriendRequest.from_user_id == b) & (FriendRequest.to_user_id == a),
            ),
        )
        .values(status="cancelled", responded_at=now)
    )


# ---------------------------------------------------------------------------
# Presence (derived from duel data, never invented)
# ---------------------------------------------------------------------------
def presence_map(
    db: Session,
    user_ids: Sequence[UUID],
    *,
    now: datetime | None = None,
) -> dict[UUID, str]:
    """Presence of every requested account: online / in_lobby / in_match / offline."""
    ids = _unique(user_ids)
    if not ids:
        return {}
    now = now or _now()
    cutoff = now - PRESENCE_ONLINE_WINDOW
    result: dict[UUID, str] = {user_id: "offline" for user_id in ids}

    # Heartbeats of a duel participant. ``last_seen_at`` is the only online
    # signal; a recent login or HTTP request is deliberately ignored.
    heartbeats = db.execute(
        select(
            DuelParticipant.user_id,
            func.max(DuelParticipant.last_seen_at),
        )
        .where(
            DuelParticipant.user_id.in_(ids),
            DuelParticipant.last_seen_at.is_not(None),
        )
        .group_by(DuelParticipant.user_id)
    ).all()
    for user_id, seen_at in heartbeats:
        value = _aware(seen_at)
        if user_id is not None and value is not None and value >= cutoff:
            result[user_id] = "online"

    # An active match: waiting = lobby/queue, playing = in the match itself.
    match_rows = db.execute(
        select(DuelMatch.status, DuelParticipant.user_id)
        .join(DuelParticipant, DuelParticipant.match_id == DuelMatch.id)
        .where(
            DuelParticipant.user_id.in_(ids),
            DuelMatch.status.in_(("waiting", "playing")),
        )
    ).all()
    for status, user_id in match_rows:
        if user_id is None:
            continue
        if status == "playing":
            result[user_id] = "in_match"
        elif result[user_id] != "in_match":
            result[user_id] = "in_lobby"

    # A shared duel lobby that has not started yet.
    lobby_rows = db.execute(
        select(DuelLobby.host_user_id, DuelLobby.guest_user_id).where(
            DuelLobby.status == "waiting",
            or_(
                DuelLobby.host_user_id.in_(ids),
                DuelLobby.guest_user_id.in_(ids),
            ),
        )
    ).all()
    for host_id, guest_id in lobby_rows:
        for user_id in (host_id, guest_id):
            if user_id in result and result[user_id] != "in_match":
                result[user_id] = "in_lobby"

    return result


def presence_for(
    db: Session,
    viewer_id: UUID,
    target_id: UUID,
    privacy: dict[str, Any],
    *,
    now: datetime | None = None,
) -> str:
    if viewer_id != target_id and not privacy["show_online"]:
        return "hidden"
    return presence_map(db, [target_id], now=now)[target_id]


# ---------------------------------------------------------------------------
# Ratings, stats and cards
# ---------------------------------------------------------------------------
def rating_map(db: Session, user_ids: Sequence[UUID]) -> dict[UUID, int]:
    ids = _unique(user_ids)
    if not ids:
        return {}
    latest = _latest_rating_subquery()
    rows = db.execute(
        select(latest.c.user_id, latest.c.rating).where(
            latest.c.user_id.in_(ids)
        )
    ).all()
    return {user_id: int(rating) for user_id, rating in rows}


def peak_rating_map(db: Session, user_ids: Sequence[UUID]) -> dict[UUID, int]:
    ids = _unique(user_ids)
    if not ids:
        return {}
    rows = db.execute(
        select(
            DuelParticipant.user_id, func.max(DuelParticipant.rating_after)
        )
        .where(
            DuelParticipant.user_id.in_(ids),
            DuelParticipant.is_bot.is_(False),
            DuelParticipant.rating_after.is_not(None),
        )
        .group_by(DuelParticipant.user_id)
    ).all()
    return {user_id: int(value) for user_id, value in rows}


def _duel_stats_map(
    db: Session, user_ids: Sequence[UUID]
) -> dict[UUID, dict[str, Any]]:
    """Wins/losses/draws/duels/win rate/best accuracy from duel participants."""
    ids = _unique(user_ids)
    if not ids:
        return {}
    points = (
        DuelParticipant.perfect_count * 100
        + DuelParticipant.great_count * 85
        + DuelParticipant.good_count * 70
    )
    accuracy = case(
        (
            DuelParticipant.notes_played > 0,
            points * 1.0 / DuelParticipant.notes_played,
        ),
        else_=None,
    )
    rows = db.execute(
        select(
            DuelParticipant.user_id,
            func.count(DuelParticipant.id).label("duels"),
            func.sum(
                case((DuelParticipant.result == "win", 1), else_=0)
            ).label("wins"),
            func.sum(
                case((DuelParticipant.result == "loss", 1), else_=0)
            ).label("losses"),
            func.sum(
                case((DuelParticipant.result == "draw", 1), else_=0)
            ).label("draws"),
            func.max(accuracy).label("best_accuracy"),
        )
        .join(DuelMatch, DuelMatch.id == DuelParticipant.match_id)
        .where(
            DuelParticipant.user_id.in_(ids),
            DuelParticipant.is_bot.is_(False),
            DuelMatch.status == "finished",
        )
        .group_by(DuelParticipant.user_id)
    ).all()

    result: dict[UUID, dict[str, Any]] = {}
    for row in rows:
        duels = int(row.duels or 0)
        wins = int(row.wins or 0)
        result[row.user_id] = {
            "duels": duels,
            "wins": wins,
            "losses": int(row.losses or 0),
            "draws": int(row.draws or 0),
            "win_rate": round(100.0 * wins / duels, 2) if duels else 0.0,
            "best_accuracy": round(float(row.best_accuracy or 0.0), 2),
        }
    return result


def _songs_mastered_map(
    db: Session, user_ids: Sequence[UUID]
) -> dict[UUID, int]:
    ids = _unique(user_ids)
    if not ids:
        return {}
    rows = db.execute(
        select(SongBestScore.user_id, func.count(SongBestScore.id))
        .where(SongBestScore.user_id.in_(ids))
        .group_by(SongBestScore.user_id)
    ).all()
    return {user_id: int(count) for user_id, count in rows}


def _achievement_counts(
    db: Session, user_ids: Sequence[UUID]
) -> tuple[int, dict[UUID, int]]:
    total = int(
        db.scalar(
            select(func.count(Achievement.code)).where(
                Achievement.is_active.is_(True)
            )
        )
        or 0
    )
    ids = _unique(user_ids)
    if not ids:
        return total, {}
    rows = db.execute(
        select(UserAchievement.user_id, func.count(UserAchievement.id))
        .where(UserAchievement.user_id.in_(ids))
        .group_by(UserAchievement.user_id)
    ).all()
    return total, {user_id: int(count) for user_id, count in rows}


def _towers_completed_map(
    db: Session, user_ids: Sequence[UUID]
) -> dict[UUID, int]:
    ids = _unique(user_ids)
    if not ids:
        return {}
    rows = db.execute(
        select(
            UserCampaignProgress.user_id,
            func.sum(UserCampaignProgress.completion_count),
        )
        .where(UserCampaignProgress.user_id.in_(ids))
        .group_by(UserCampaignProgress.user_id)
    ).all()
    return {user_id: int(total or 0) for user_id, total in rows}


def stats_map(
    db: Session, user_ids: Sequence[UUID]
) -> dict[UUID, dict[str, Any]]:
    """The extended statistics block of every requested account."""
    ids = _unique(user_ids)
    if not ids:
        return {}
    duel_rows = _duel_stats_map(db, ids)
    mastered = _songs_mastered_map(db, ids)
    achievements_total, unlocked = _achievement_counts(db, ids)
    towers = _towers_completed_map(db, ids)
    result: dict[UUID, dict[str, Any]] = {}
    for user_id in ids:
        base = duel_rows.get(
            user_id,
            {
                "duels": 0,
                "wins": 0,
                "losses": 0,
                "draws": 0,
                "win_rate": 0.0,
                "best_accuracy": 0.0,
            },
        )
        result[user_id] = {
            **base,
            "songs_mastered": mastered.get(user_id, 0),
            "achievements_unlocked": unlocked.get(user_id, 0),
            "achievements_total": achievements_total,
            "towers_completed": towers.get(user_id, 0),
        }
    return result


def stats_block(db: Session, user_id: UUID) -> dict[str, Any]:
    return stats_map(db, [user_id])[user_id]


def player_stats(
    db: Session, viewer: User, target: User
) -> dict[str, Any]:
    """The statistics block with the privacy rules of :func:`public_card`."""
    if viewer.id != target.id:
        privacy = privacy_settings(db, target.id)
        if block_exists(db, viewer.id, target.id):
            raise SocialError(
                403, "stats_hidden", "This player hides their statistics"
            )
        if not privacy["show_profile"] and not are_friends(
            db, viewer.id, target.id
        ):
            raise SocialError(
                403, "profile_hidden", "This player hides their profile"
            )
        if not privacy["show_stats"]:
            raise SocialError(
                403, "stats_hidden", "This player hides their statistics"
            )
    return stats_block(db, target.id)


def _display_name(user: User) -> str:
    return user.display_name or user.username


def compact_payload(
    user: User,
    *,
    rating: int,
    presence: str,
    relationship_value: str | None = None,
) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "user_id": user.id,
        "username": user.username,
        "display_name": _display_name(user),
        "level": int(user.level or 1),
        "rating": rating,
        "presence": presence,
    }
    if relationship_value is not None:
        payload["relationship"] = relationship_value
    return payload


def compact_cards(
    db: Session,
    viewer: User,
    users: Sequence[User],
    *,
    now: datetime | None = None,
) -> list[dict[str, Any]]:
    """Compact cards with rating, presence and relationship for each user."""
    if not users:
        return []
    ids = [user.id for user in users]
    ratings = rating_map(db, ids)
    presence = presence_map(db, ids, now=now)
    privacy = {user_id: privacy_settings(db, user_id) for user_id in ids}
    blocked = blocked_user_ids(db, viewer.id)
    friend_set = set(friend_ids(db, viewer.id))
    pending = _pending_relationship_map(db, viewer.id, ids)
    cards: list[dict[str, Any]] = []
    for user in users:
        if user.id == viewer.id:
            relation = "self"
        elif user.id in blocked:
            relation = "blocked"
        elif user.id in friend_set:
            relation = "friend"
        else:
            relation = pending.get(user.id, "none")
        shown_presence = presence[user.id]
        if (
            user.id != viewer.id
            and not privacy[user.id]["show_online"]
        ):
            shown_presence = "hidden"
        cards.append(
            compact_payload(
                user,
                rating=ratings.get(user.id, duel_rules.ELO_START_RATING),
                presence=shown_presence,
                relationship_value=relation,
            )
        )
    return cards


def _pending_relationship_map(
    db: Session, viewer_id: UUID, target_ids: Sequence[UUID]
) -> dict[UUID, str]:
    """incoming/outgoing of the viewer for every target with a pending request."""
    ids = _unique(target_ids)
    if not ids:
        return {}
    rows = db.execute(
        select(FriendRequest.from_user_id, FriendRequest.to_user_id).where(
            FriendRequest.status == "pending",
            or_(
                (FriendRequest.from_user_id == viewer_id)
                & (FriendRequest.to_user_id.in_(ids)),
                (FriendRequest.to_user_id == viewer_id)
                & (FriendRequest.from_user_id.in_(ids)),
            ),
        )
    ).all()
    result: dict[UUID, str] = {}
    for from_user_id, to_user_id in rows:
        result[to_user_id if from_user_id == viewer_id else from_user_id] = (
            "outgoing" if from_user_id == viewer_id else "incoming"
        )
    return result


def public_card(
    db: Session,
    viewer: User,
    target: User,
    *,
    now: datetime | None = None,
) -> dict[str, Any]:
    """The aggregated public card of ``target`` as seen by ``viewer``."""
    relation = relationship(db, viewer.id, target.id)
    privacy = privacy_settings(db, target.id)
    is_self = viewer.id == target.id
    if (
        not is_self
        and relation not in ("friend", "blocked")
        and not privacy["show_profile"]
    ):
        raise SocialError(
            403, "profile_hidden", "This player hides their profile"
        )

    character = None
    if target.character_id is not None:
        character = db.get(UiCharacter, target.character_id)

    ratings = rating_map(db, [target.id])
    peaks = peak_rating_map(db, [target.id])
    presence = presence_for(db, viewer.id, target.id, privacy, now=now)
    skills = db.get(UserGuitarSkills, target.id)

    stats = None
    if relation != "blocked" and _show_stats(privacy, viewer.id, target.id):
        stats = stats_block(db, target.id)

    return {
        "user_id": target.id,
        "username": target.username,
        "display_name": _display_name(target),
        "level": int(target.level or 1),
        "xp_total": int(target.xp_total or 0),
        "rating": ratings.get(target.id, duel_rules.ELO_START_RATING),
        "peak_rating": peaks.get(
            target.id, duel_rules.ELO_START_RATING
        ),
        "guitar_skills": (
            round(float(skills.guitar_skills), 1) if skills is not None else None
        ),
        "guitar_skills_rank": (
            int(skills.rank) if skills is not None and skills.rank is not None else None
        ),
        "provisional": bool(skills.provisional) if skills is not None else None,
        "character_name": character.name if character is not None else None,
        "character_sheet_url": (
            character_sheet_url(character.sheet_key)
            if character is not None
            else None
        ),
        "presence": presence,
        "relationship": relation,
        "stats": stats,
        # Blocking a player disables challenges between the pair; it is derived
        # from the block itself, so blocking never mutates stored settings.
        "can_challenge": (
            relation != "blocked"
            and bool(privacy["allow_challenges"])
        ),
    }


# ---------------------------------------------------------------------------
# Search / friends / requests
# ---------------------------------------------------------------------------
def search_players(
    db: Session,
    viewer: User,
    *,
    query: str,
    limit: int,
    offset: int,
) -> tuple[list[dict[str, Any]], int]:
    needle = query.strip()
    if not needle:
        return [], 0
    pattern = _like_pattern(needle)
    excluded = blocked_user_ids(db, viewer.id)
    filters = [
        User.is_active.is_(True),
        User.id != viewer.id,
        or_(
            User.username.ilike(pattern, escape="\\"),
            User.display_name.ilike(pattern, escape="\\"),
        ),
    ]
    if excluded:
        filters.append(User.id.not_in(excluded))
    total = int(
        db.scalar(select(func.count(User.id)).where(*filters)) or 0
    )
    users = list(
        db.scalars(
            select(User)
            .where(*filters)
            .order_by(User.username.asc(), User.id.asc())
            .limit(limit)
            .offset(offset)
        ).all()
    )
    return compact_cards(db, viewer, users), total


def list_friends(
    db: Session, viewer: User, *, limit: int, offset: int
) -> tuple[list[dict[str, Any]], int]:
    ids = friend_ids(db, viewer.id)
    if not ids:
        return [], 0
    total = int(
        db.scalar(
            select(func.count(User.id)).where(
                User.id.in_(ids), User.is_active.is_(True)
            )
        )
        or 0
    )
    users = list(
        db.scalars(
            select(User)
            .where(User.id.in_(ids), User.is_active.is_(True))
            .order_by(User.username.asc(), User.id.asc())
            .limit(limit)
            .offset(offset)
        ).all()
    )
    return compact_cards(db, viewer, users), total


def request_payload(
    request: FriendRequest, other: User, card: dict[str, Any]
) -> dict[str, Any]:
    return {
        "id": request.id,
        "from_user_id": request.from_user_id,
        "to_user_id": request.to_user_id,
        "status": request.status,
        "created_at": request.created_at,
        "responded_at": request.responded_at,
        "user": card,
        "direction": "outgoing" if other.id == request.to_user_id else "incoming",
    }


def list_requests(
    db: Session,
    viewer: User,
    *,
    direction: str,
    limit: int,
    offset: int,
) -> tuple[list[dict[str, Any]], int]:
    if direction == "incoming":
        condition = FriendRequest.to_user_id == viewer.id
        other_column = FriendRequest.from_user_id
    else:
        condition = FriendRequest.from_user_id == viewer.id
        other_column = FriendRequest.to_user_id
    filters = (FriendRequest.status == "pending", condition)
    total = int(
        db.scalar(
            select(func.count(FriendRequest.id)).where(*filters)
        )
        or 0
    )
    rows = db.execute(
        select(FriendRequest, other_column)
        .where(*filters)
        .order_by(FriendRequest.created_at.desc(), FriendRequest.id.desc())
        .limit(limit)
        .offset(offset)
    ).all()
    if not rows:
        return [], total
    other_ids = _unique(row[1] for row in rows)
    users = {
        user.id: user
        for user in db.scalars(select(User).where(User.id.in_(other_ids)))
    }
    cards = {
        card["user_id"]: card
        for card in compact_cards(
            db, viewer, [users[user_id] for user_id in other_ids if user_id in users]
        )
    }
    items: list[dict[str, Any]] = []
    for request, other_id in rows:
        other = users.get(other_id)
        if other is None:
            continue
        card = cards.get(other.id)
        if card is None:
            continue
        items.append(request_payload(request, other, card))
    return items, total


# ---------------------------------------------------------------------------
# Mutations
# ---------------------------------------------------------------------------
def _self_error(code: str) -> SocialError:
    return SocialError(422, code, "You cannot target yourself")


def send_friend_request(
    db: Session, sender: User, target_id: UUID
) -> dict[str, Any]:
    if sender.id == target_id:
        raise _self_error("cannot_add_self")
    target = active_user(db, target_id)
    if block_exists(db, sender.id, target.id):
        raise SocialError(409, "blocked", "One of the players blocked the other")
    if are_friends(db, sender.id, target.id):
        raise SocialError(409, "already_friends", "You are already friends")
    if _pending_request(db, sender.id, target.id) is not None:
        raise SocialError(
            409, "request_pending", "A friend request is already pending"
        )
    policy = privacy_settings(db, target.id)["request_policy"]
    if policy == "nobody":
        raise SocialError(
            409,
            "requests_disabled",
            "This player does not accept friend requests",
        )
    if policy == "friends":
        raise SocialError(
            409,
            "requests_friends_only",
            "This player accepts requests from friends only",
        )

    request = FriendRequest(
        from_user_id=sender.id, to_user_id=target.id, status="pending"
    )
    db.add(request)
    try:
        db.flush()
    except IntegrityError as exc:  # concurrent duplicate pending request
        db.rollback()
        raise SocialError(
            409, "request_pending", "A friend request is already pending"
        ) from exc

    push_notification(
        db,
        target.id,
        "friend_request",
        {
            "request_id": str(request.id),
            "user_id": str(sender.id),
            "username": sender.username,
            "display_name": _display_name(sender),
        },
        f"friend_request:{request.id}",
    )
    db.commit()
    db.refresh(request)

    card = compact_cards(db, sender, [target])[0]
    return request_payload(request, target, card)


def remove_friend(db: Session, user: User, other_id: UUID) -> bool:
    if user.id == other_id:
        raise _self_error("cannot_remove_self")
    other = db.get(User, other_id)
    if other is None:
        raise SocialError(404, "user_not_found", "Player not found")
    left, right = _pair(user.id, other_id)
    deleted = db.execute(
        Friendship.__table__.delete().where(
            Friendship.user_id == left, Friendship.friend_id == right
        )
    ).rowcount
    # DELETE is idempotent: a player with no friendship simply removes nothing.
    db.commit()
    return bool(deleted)


def _request_for(db: Session, request_id: UUID) -> FriendRequest:
    request = db.get(FriendRequest, request_id)
    if request is None:
        raise SocialError(404, "request_not_found", "Friend request not found")
    return request


def accept_request(db: Session, user: User, request_id: UUID) -> FriendRequest:
    request = _request_for(db, request_id)
    if request.to_user_id != user.id:
        raise SocialError(
            403, "not_addressee", "Only the addressee can accept this request"
        )
    if request.status == "accepted":
        # Idempotent retry: make sure the friendship exists and return the row.
        _create_friendship(db, request.from_user_id, request.to_user_id)
        db.commit()
        return request
    if request.status != "pending":
        raise SocialError(
            409, "request_not_pending", "This request is no longer pending"
        )
    if block_exists(db, request.from_user_id, request.to_user_id):
        raise SocialError(409, "blocked", "One of the players blocked the other")

    now = _now()
    request.status = "accepted"
    request.responded_at = now
    _create_friendship(db, request.from_user_id, request.to_user_id)
    # The other direction must not stay actionable; accepting resolves the pair.
    db.execute(
        update(FriendRequest)
        .where(
            FriendRequest.status == "pending",
            FriendRequest.from_user_id == request.to_user_id,
            FriendRequest.to_user_id == request.from_user_id,
        )
        .values(status="cancelled", responded_at=now)
    )
    push_notification(
        db,
        request.from_user_id,
        "friend_accepted",
        {
            "request_id": str(request.id),
            "user_id": str(request.to_user_id),
            "username": user.username,
            "display_name": _display_name(user),
        },
        f"friend_accepted:{request.id}",
    )
    db.commit()
    db.refresh(request)
    return request


def decline_request(db: Session, user: User, request_id: UUID) -> FriendRequest:
    request = _request_for(db, request_id)
    if request.to_user_id != user.id:
        raise SocialError(
            403, "not_addressee", "Only the addressee can decline this request"
        )
    if request.status == "declined":
        return request
    if request.status != "pending":
        raise SocialError(
            409, "request_not_pending", "This request is no longer pending"
        )
    request.status = "declined"
    request.responded_at = _now()
    db.commit()
    db.refresh(request)
    return request


def cancel_request(db: Session, user: User, request_id: UUID) -> FriendRequest:
    request = _request_for(db, request_id)
    if request.from_user_id != user.id:
        raise SocialError(
            403, "not_sender", "Only the sender can cancel this request"
        )
    if request.status == "cancelled":
        # Idempotent retry of a cancel.
        return request
    if request.status != "pending":
        raise SocialError(
            409, "request_not_pending", "This request is no longer pending"
        )
    request.status = "cancelled"
    request.responded_at = _now()
    db.commit()
    db.refresh(request)
    return request


def _create_friendship(db: Session, a: UUID, b: UUID) -> None:
    left, right = _pair(a, b)
    db.execute(
        pg_insert(Friendship.__table__)
        .values(user_id=left, friend_id=right)
        .on_conflict_do_nothing(index_elements=["user_id", "friend_id"])
    )
    db.flush()


def block_user(db: Session, user: User, target_id: UUID) -> None:
    if user.id == target_id:
        raise _self_error("cannot_block_self")
    target = active_user(db, target_id)
    now = _now()
    db.execute(
        pg_insert(UserBlock.__table__)
        .values(blocker_id=user.id, blocked_id=target.id)
        .on_conflict_do_nothing(index_elements=["blocker_id", "blocked_id"])
    )
    # Blocking ends the friendship and every pending request in both
    # directions; challenges between the pair are disabled by the derived
    # ``can_challenge`` flag (see :func:`public_card`), so no settings are
    # silently rewritten.
    left, right = _pair(user.id, target.id)
    db.execute(
        Friendship.__table__.delete().where(
            Friendship.user_id == left, Friendship.friend_id == right
        )
    )
    _cancel_pending_requests(db, user.id, target.id, now=now)
    db.execute(
        UserRival.__table__.delete().where(
            or_(
                (UserRival.user_id == user.id)
                & (UserRival.rival_user_id == target.id),
                (UserRival.user_id == target.id)
                & (UserRival.rival_user_id == user.id),
            )
        )
    )
    db.commit()


def unblock_user(db: Session, user: User, target_id: UUID) -> None:
    if user.id == target_id:
        raise _self_error("cannot_unblock_self")
    db.execute(
        UserBlock.__table__.delete().where(
            UserBlock.blocker_id == user.id, UserBlock.blocked_id == target_id
        )
    )
    db.commit()


# ---------------------------------------------------------------------------
# Leaderboard
# ---------------------------------------------------------------------------
def leaderboard(
    db: Session,
    viewer: User,
    *,
    sort: str,
    limit: int,
    offset: int,
) -> tuple[list[dict[str, Any]], int]:
    """Friends plus the caller, ordered by one of the supported keys."""
    ids = _unique([viewer.id, *friend_ids(db, viewer.id)])
    users = list(
        db.scalars(
            select(User).where(User.id.in_(ids), User.is_active.is_(True))
        ).all()
    )
    if not users:
        return [], 0
    user_ids = [user.id for user in users]
    ratings = rating_map(db, user_ids)
    presence = presence_map(db, user_ids)
    privacy = {user_id: privacy_settings(db, user_id) for user_id in user_ids}
    stats = stats_map(db, user_ids)
    skills = {
        row.user_id: row
        for row in db.scalars(
            select(UserGuitarSkills).where(UserGuitarSkills.user_id.in_(user_ids))
        ).all()
    }

    rows: list[dict[str, Any]] = []
    for user in users:
        user_stats = stats[user.id]
        shown_presence = presence[user.id]
        if user.id != viewer.id and not privacy[user.id]["show_online"]:
            shown_presence = "hidden"
        user_skills = skills.get(user.id)
        rows.append(
            {
                "user_id": user.id,
                "username": user.username,
                "display_name": _display_name(user),
                "level": int(user.level or 1),
                "xp_total": int(user.xp_total or 0),
                "rating": ratings.get(user.id, duel_rules.ELO_START_RATING),
                "guitar_skills": (
                    round(float(user_skills.guitar_skills), 1)
                    if user_skills is not None
                    else None
                ),
                "provisional": (
                    bool(user_skills.provisional)
                    if user_skills is not None
                    else None
                ),
                "wins": user_stats["wins"],
                "losses": user_stats["losses"],
                "draws": user_stats["draws"],
                "duels": user_stats["duels"],
                "win_rate": user_stats["win_rate"],
                "presence": shown_presence,
                "is_self": user.id == viewer.id,
            }
        )

    if sort == "level":
        rows.sort(
            key=lambda row: (
                -row["level"],
                -row["xp_total"],
                row["username"],
                str(row["user_id"]),
            )
        )
    elif sort == "wins":
        rows.sort(
            key=lambda row: (
                -row["wins"],
                -row["duels"],
                row["username"],
                str(row["user_id"]),
            )
        )
    elif sort == "win_rate":
        rows.sort(
            key=lambda row: (
                -row["win_rate"],
                -row["wins"],
                row["username"],
                str(row["user_id"]),
            )
        )
    elif sort == "guitar_skills":
        rows.sort(
            key=lambda row: (
                -(row["guitar_skills"] if row["guitar_skills"] is not None else -1.0),
                -row["level"],
                -row["xp_total"],
                row["username"],
                str(row["user_id"]),
            )
        )
    else:
        rows.sort(
            key=lambda row: (
                -row["rating"],
                row["username"],
                str(row["user_id"]),
            )
        )

    total = len(rows)
    window = rows[offset : offset + limit]
    for position, row in enumerate(window):
        row["rank"] = offset + position + 1
    return window, total


# ---------------------------------------------------------------------------
# Rival
# ---------------------------------------------------------------------------
def get_rival(db: Session, viewer: User) -> dict[str, Any] | None:
    row = db.get(UserRival, viewer.id)
    if row is None:
        return None
    rival = db.get(User, row.rival_user_id)
    if rival is None or not rival.is_active:
        return None
    return {
        "user": compact_cards(db, viewer, [rival])[0],
        "updated_at": row.updated_at,
    }


def set_rival(db: Session, viewer: User, rival_id: UUID) -> dict[str, Any]:
    if viewer.id == rival_id:
        raise _self_error("cannot_rival_self")
    rival = active_user(db, rival_id)
    if not are_friends(db, viewer.id, rival.id):
        raise SocialError(409, "rival_not_friend", "Your rival must be a friend")
    db.execute(
        pg_insert(UserRival.__table__)
        .values(user_id=viewer.id, rival_user_id=rival.id)
        .on_conflict_do_update(
            index_elements=["user_id"],
            set_={"rival_user_id": rival.id, "updated_at": _now()},
        )
    )
    db.commit()
    selected = get_rival(db, viewer)
    assert selected is not None
    return selected


def clear_rival(db: Session, viewer: User) -> None:
    db.execute(
        UserRival.__table__.delete().where(UserRival.user_id == viewer.id)
    )
    db.commit()


# ---------------------------------------------------------------------------
# Activity feed
# ---------------------------------------------------------------------------
def record_activity(
    db: Session,
    *,
    user_id: UUID,
    kind: str,
    payload: dict[str, Any] | None = None,
    visible: bool = True,
    notify_friends: bool = True,
) -> PlayerActivity:
    """Store one feed event and (optionally) notify friends who allow activity.

    This is the additive integration point for league/record events: a module
    that awards a record or a trophy calls this and the friends' feed follows.
    The caller owns the transaction and must commit.
    """
    if kind not in ACTIVITY_KINDS:
        raise SocialError(422, "invalid_activity_kind", "Unknown activity kind")
    activity = PlayerActivity(
        user_id=user_id,
        kind=kind,
        payload=payload or {},
        visible=visible,
    )
    db.add(activity)
    db.flush()
    if notify_friends and visible:
        _notify_friends_about_activity(db, activity)
    return activity


def _notify_friends_about_activity(
    db: Session, activity: PlayerActivity
) -> None:
    friends = _show_activity_ids(db, friend_ids(db, activity.user_id))
    if not friends:
        return
    actor = db.get(User, activity.user_id)
    for friend_id in friends:
        push_notification(
            db,
            friend_id,
            f"activity_{activity.kind}",
            {
                "activity_id": str(activity.id),
                "user_id": str(activity.user_id),
                "username": actor.username if actor is not None else None,
                "display_name": _display_name(actor) if actor is not None else None,
                "kind": activity.kind,
                "payload": activity.payload,
            },
            f"activity:{activity.id}",
        )


def activity_feed(
    db: Session,
    viewer: User,
    *,
    limit: int,
    offset: int,
) -> tuple[list[dict[str, Any]], int]:
    friends = _show_activity_ids(db, friend_ids(db, viewer.id))
    if not friends:
        return [], 0
    filters = (
        PlayerActivity.user_id.in_(friends),
        PlayerActivity.visible.is_(True),
    )
    total = int(
        db.scalar(
            select(func.count(PlayerActivity.id)).where(*filters)
        )
        or 0
    )
    rows = list(
        db.scalars(
            select(PlayerActivity)
            .where(*filters)
            .order_by(
                PlayerActivity.created_at.desc(), PlayerActivity.id.desc()
            )
            .limit(limit)
            .offset(offset)
        ).all()
    )
    if not rows:
        return [], total
    actors = {
        user.id: user
        for user in db.scalars(
            select(User).where(
                User.id.in_(_unique(row.user_id for row in rows))
            )
        )
    }
    cards = {
        card["user_id"]: card
        for card in compact_cards(
            db, viewer, [actors[key] for key in actors]
        )
    }
    items: list[dict[str, Any]] = []
    for row in rows:
        card = cards.get(row.user_id)
        if card is None:
            continue
        items.append(
            {
                "id": row.id,
                "kind": row.kind,
                "payload": row.payload or {},
                "created_at": row.created_at,
                "user": card,
            }
        )
    return items, total


# ---------------------------------------------------------------------------
# Notifications
# ---------------------------------------------------------------------------
def push_notification(
    db: Session,
    user_id: UUID,
    kind: str,
    payload: dict[str, Any],
    dedupe_key: str,
) -> bool:
    """Deliver one notification; returns False when it was already delivered."""
    inserted = db.execute(
        pg_insert(SocialNotification.__table__)
        .values(
            user_id=user_id,
            kind=kind,
            payload=payload,
            dedupe_key=dedupe_key,
        )
        .on_conflict_do_nothing(index_elements=["user_id", "dedupe_key"])
        .returning(SocialNotification.id)
    ).first()
    return inserted is not None


def list_notifications(
    db: Session,
    viewer: User,
    *,
    unread_only: bool,
    limit: int,
    offset: int,
) -> tuple[list[SocialNotification], int, int]:
    filters = [SocialNotification.user_id == viewer.id]
    if unread_only:
        filters.append(SocialNotification.read_at.is_(None))
    total = int(
        db.scalar(
            select(func.count(SocialNotification.id)).where(*filters)
        )
        or 0
    )
    unread = int(
        db.scalar(
            select(func.count(SocialNotification.id)).where(
                SocialNotification.user_id == viewer.id,
                SocialNotification.read_at.is_(None),
            )
        )
        or 0
    )
    rows = list(
        db.scalars(
            select(SocialNotification)
            .where(*filters)
            .order_by(
                SocialNotification.created_at.desc(),
                SocialNotification.id.desc(),
            )
            .limit(limit)
            .offset(offset)
        ).all()
    )
    return rows, total, unread


def mark_notifications_read(
    db: Session,
    viewer: User,
    *,
    ids: Sequence[UUID] | None,
    all_unread: bool,
) -> int:
    now = _now()
    filters = [
        SocialNotification.user_id == viewer.id,
        SocialNotification.read_at.is_(None),
    ]
    if not all_unread:
        if not ids:
            raise SocialError(
                422, "no_notifications_selected", "Pass ids or all=true"
            )
        filters.append(SocialNotification.id.in_(list(ids)))
    result = db.execute(
        update(SocialNotification)
        .where(*filters)
        .values(read_at=now)
    )
    db.commit()
    return int(result.rowcount or 0)
