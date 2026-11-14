"""Authoritative Guitar Duels rules.

This module is a Python port of the client rules in
``GuitarBattle/Source/Gameplay/DuelEngine.h`` and ``DuelMatch.h``. The server
must never trust a client-reported HP, damage or result, so every number here
is derived from the note qualities only.

Rules (``rules_version = 3``)
-----------------------------
* Quality points: ``perfect = 100``, ``great = 85``, ``good = 70``, ``miss = 0``.
* Every note compares the two sides:
  - both miss -> ``both_miss``, no damage;
  - the same quality (and both hit) -> ``block``, no damage;
  - otherwise the better side deals
    ``clamp(abs(points_a - points_b) * 0.10, 1.0, 10.0)`` base damage.
* Version 3 scales that base damage by the chart length, so a 3x longer song
  does not end the duel 3x faster: charts of at most ``DAMAGE_REFERENCE_NOTES``
  (48) notes keep the full damage, longer charts deal
  ``max(0.20, 48 / note_count)`` of it. The scaled value is **not** clamped
  again, so a long chart can deal less than one HP per note. Version 2 and
  older matches keep unscaled damage: the version is frozen on the match.
* HP starts at ``100`` and is clamped to ``[0, 100]`` after every exchange.
* A side at ``0`` HP loses immediately; if the chart ends without a knockout,
  the higher remaining HP wins and equal HP is a draw.
* XP: ``20 + perfect*5 + great*3 + good*2 + (win ? 40 : 0)``. This is the same
  formula the client shows on its results screen (``DuelMatch::getRewards``).
* Rating: Elo with ``K = 32`` and a start rating of ``1000``; only finished
  online human-vs-human matches change it. Bots never do.

The bot is deterministic: the same ``seed`` and note index always produce the
same quality, so a bot match can be replayed and audited server-side. The hash
is bit-for-bit the C++ ``duelBotHash`` (32-bit unsigned arithmetic).
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from enum import IntEnum
from typing import Any, Final

#: The only rules version implemented by this module. Stored on every match so
#: a future balance change can be rolled out without rewriting old results.
#: Version 2 raised the damage scale to match the client build 1.2.0
#: (``DuelConfig``: 0.10 per point, clamp ``[1.0, 10.0]``).
#: Version 3 scales damage by the frozen chart length
#: (``damage_scale_for_note_count``) as implemented by the client
#: ``duelConfigForNoteCount``; older matches keep unscaled damage.
RULES_VERSION: Final[int] = 3

#: HP both sides start with.
MAX_HEALTH: Final[float] = 100.0

#: Quality points used by the damage formula.
QUALITY_POINTS: Final[dict[str, float]] = {
    "perfect": 100.0,
    "great": 85.0,
    "good": 70.0,
    "miss": 0.0,
}

DAMAGE_PER_POINT: Final[float] = 0.100
MIN_DAMAGE: Final[float] = 1.0
MAX_DAMAGE: Final[float] = 10.0

#: Charts at or below this length keep the full damage. Longer charts scale
#: down so the total possible damage stays comparable across songs
#: (``duelDamageReferenceNotes`` in the client).
DAMAGE_REFERENCE_NOTES: Final[int] = 48

#: Longest charts deal at most this fraction of the base damage per note
#: (``duelDamageMinScale`` in the client).
DAMAGE_MIN_SCALE: Final[float] = 0.20

#: Floating-point tolerance for "the two sides played the same quality".
_SAME_POINTS_EPSILON: Final[float] = 0.0001

#: XP constants (identical to ``DuelMatch::getRewards``).
XP_PARTICIPATION: Final[int] = 20
XP_PER_PERFECT: Final[int] = 5
XP_PER_GREAT: Final[int] = 3
XP_PER_GOOD: Final[int] = 2
XP_WIN_BONUS: Final[int] = 40

#: Elo.
ELO_START_RATING: Final[int] = 1000
ELO_K_FACTOR: Final[int] = 32


class DuelQuality(IntEnum):
    """Quality as the duel sees it (values match the C++ enum)."""

    MISS = 0
    GOOD = 1
    GREAT = 2
    PERFECT = 3

    @classmethod
    def from_token(cls, token: str) -> "DuelQuality":
        """Parse ``perfect``/``great``/``good``/``miss`` (case-insensitive)."""
        key = str(token or "").strip().lower()
        try:
            return _QUALITY_BY_TOKEN[key]
        except KeyError as exc:
            raise ValueError(f"Unknown duel quality: {token!r}") from exc

    @property
    def token(self) -> str:
        return _QUALITY_TOKENS[self]


_QUALITY_BY_TOKEN: Final[dict[str, DuelQuality]] = {
    "miss": DuelQuality.MISS,
    "good": DuelQuality.GOOD,
    "great": DuelQuality.GREAT,
    "perfect": DuelQuality.PERFECT,
}
_QUALITY_TOKENS: Final[dict[DuelQuality, str]] = {
    quality: token for token, quality in _QUALITY_BY_TOKEN.items()
}


class DuelExchange(IntEnum):
    """Outcome of one chart note (values match ``DuelExchange`` in C++)."""

    BOTH_MISS = 0
    BLOCK = 1
    PLAYER_HIT = 2
    OPPONENT_HIT = 3

    @property
    def token(self) -> str:
        return self.name.lower()


#: ``player`` is the side that created the match (or the human in a bot match);
#: ``opponent`` is the joiner (or the bot).
SIDES: Final[tuple[str, str]] = ("player", "opponent")


def other_side(side: str) -> str:
    return "opponent" if side == "player" else "player"


def clamp(low: float, high: float, value: float) -> float:
    if value < low:
        return low
    if value > high:
        return high
    return value


def damage_scale_for_note_count(note_count: int) -> float:
    """Chart-length damage factor, port of ``duelDamageScaleForNoteCount``.

    Charts of at most :data:`DAMAGE_REFERENCE_NOTES` notes keep the full
    damage (``1.0``). Longer charts deal ``48 / note_count`` of it, never less
    than :data:`DAMAGE_MIN_SCALE` (``0.20``). The factor multiplies the clamped
    base damage; it is not clamped again afterwards.
    """
    try:
        count = int(note_count)
    except (TypeError, ValueError):
        return 1.0
    if count <= DAMAGE_REFERENCE_NOTES:
        return 1.0
    return clamp(DAMAGE_MIN_SCALE, 1.0, DAMAGE_REFERENCE_NOTES / float(count))


def quality_points(quality: DuelQuality | str) -> float:
    token = quality.token if isinstance(quality, DuelQuality) else str(quality).lower()
    return QUALITY_POINTS.get(token, 0.0)


@dataclass(frozen=True)
class DuelNoteResult:
    """Damage and exchange of one note. ``damage_to_opponent`` is dealt by the
    ``player`` side, ``damage_to_player`` by the ``opponent`` side."""

    exchange: DuelExchange
    damage_to_opponent: float = 0.0
    damage_to_player: float = 0.0

    @property
    def blocked(self) -> bool:
        return self.exchange is DuelExchange.BLOCK


def resolve_note(
    player_quality: DuelQuality | str,
    opponent_quality: DuelQuality | str,
    *,
    damage_scale: float = 1.0,
) -> DuelNoteResult:
    """Resolve one note exactly like ``DuelEngine::resolve``.

    ``damage_scale`` is the chart-length factor from
    :func:`damage_scale_for_note_count` (the C++ ``DuelConfig::damageScale``).
    It multiplies the clamped base damage and is **not** clamped again, so a
    long chart may deal less than :data:`MIN_DAMAGE` per note. The default
    ``1.0`` keeps the pre-v3 behaviour for direct callers and old matches.

    The result is symmetric: swapping the inputs swaps the two damage fields
    and the exchange side.
    """
    player_points = quality_points(player_quality)
    opponent_points = quality_points(opponent_quality)

    if player_points <= 0.0 and opponent_points <= 0.0:
        return DuelNoteResult(DuelExchange.BOTH_MISS)

    difference = player_points - opponent_points

    if abs(difference) < _SAME_POINTS_EPSILON:
        return DuelNoteResult(DuelExchange.BLOCK)

    # A negative or non-finite factor would be meaningless; never boost damage
    # through it accidentally.
    scale = 1.0 if damage_scale is None else float(damage_scale)
    if not math.isfinite(scale):
        scale = 1.0
    scale = max(0.0, scale)

    damage = (
        clamp(
            MIN_DAMAGE,
            MAX_DAMAGE,
            abs(difference) * DAMAGE_PER_POINT,
        )
        * scale
    )

    if difference > 0.0:
        return DuelNoteResult(DuelExchange.PLAYER_HIT, damage_to_opponent=damage)
    return DuelNoteResult(DuelExchange.OPPONENT_HIT, damage_to_player=damage)


def apply_damage(health: float, damage: float) -> float:
    """Subtract damage and clamp the result to ``[0, MAX_HEALTH]``."""
    return clamp(0.0, MAX_HEALTH, float(health) - max(0.0, float(damage)))


# ---------------------------------------------------------------------------
# Bot profile and deterministic quality
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class DuelBotProfile:
    level: str
    name: str
    avatar: str
    hit_rate: float
    perfect_share: float
    great_share: float


BOT_PROFILES: Final[dict[str, DuelBotProfile]] = {
    "easy": DuelBotProfile("easy", "RUSTY", "bot-easy", 0.25, 0.20, 0.30),
    "normal": DuelBotProfile("normal", "MIDNIGHT", "bot-normal", 0.65, 0.35, 0.35),
    "hard": DuelBotProfile("hard", "VOLT", "bot-hard", 0.90, 0.55, 0.30),
}

_MASK32: Final[int] = 0xFFFFFFFF


def bot_profile(level: str) -> DuelBotProfile:
    return BOT_PROFILES.get(str(level or "").strip().lower(), BOT_PROFILES["easy"])


def duel_bot_hash(seed: int, note_index: int) -> int:
    """32-bit unsigned hash, bit-for-bit ``duelBotHash`` from the client."""
    value = (
        (int(seed) & _MASK32) * 0x9E3779B9
        + (int(note_index) & _MASK32) * 0x85EBCA6B
    ) & _MASK32
    value ^= value >> 16
    value = (value * 0x7FEB352D) & _MASK32
    value ^= value >> 15
    value = (value * 0x846CA68B) & _MASK32
    value ^= value >> 16
    return value & _MASK32


def duel_bot_quality(level: str, seed: int, note_index: int) -> DuelQuality:
    """Deterministic bot quality for one chart note."""
    profile = bot_profile(level)
    hashed = duel_bot_hash(seed, note_index)

    hit_roll = (hashed & 0xFFFF) / 65535.0
    if hit_roll >= profile.hit_rate:
        return DuelQuality.MISS

    quality_roll = ((hashed >> 16) & 0xFFFF) / 65535.0
    if quality_roll < profile.perfect_share:
        return DuelQuality.PERFECT
    if quality_roll < profile.perfect_share + profile.great_share:
        return DuelQuality.GREAT
    return DuelQuality.GOOD


def bot_qualities(level: str, seed: int, note_count: int) -> list[DuelQuality]:
    """The whole bot performance; used by tests and by ``finish`` replays."""
    return [duel_bot_quality(level, seed, index) for index in range(max(0, note_count))]


# ---------------------------------------------------------------------------
# Scoring helpers
# ---------------------------------------------------------------------------
def accuracy_factor(perfect: int, great: int, good: int, notes: int) -> float:
    """Client formula: ``(perfect + great*0.85 + good*0.70) / notes``."""
    if notes <= 0:
        return 0.0
    value = (
        float(perfect) + float(great) * 0.85 + float(good) * 0.70
    ) / float(notes)
    return clamp(0.0, 1.0, value)


def xp_for_duel(
    *,
    perfect: int,
    great: int,
    good: int,
    result: str | None,
) -> int:
    """XP for one finished duel, matching the client's rewards formula.

    ``result`` is ``win``/``loss``/``draw``. Only a win adds the bonus; a draw
    or a loss still receives participation and per-note XP.
    """
    xp = (
        XP_PARTICIPATION
        + max(0, int(perfect)) * XP_PER_PERFECT
        + max(0, int(great)) * XP_PER_GREAT
        + max(0, int(good)) * XP_PER_GOOD
    )
    if result == "win":
        xp += XP_WIN_BONUS
    return max(0, int(xp))


def _round_half_away(value: float) -> int:
    """Round ``.5`` away from zero (C++ ``std::lround`` / client expectation)."""
    if value >= 0:
        return int(math.floor(value + 0.5))
    return int(math.ceil(value - 0.5))


def expected_score(rating_a: int, rating_b: int) -> float:
    """Elo expectation of side A."""
    return 1.0 / (1.0 + 10.0 ** ((int(rating_b) - int(rating_a)) / 400.0))


def elo_delta(
    rating_a: int,
    rating_b: int,
    score_a: float,
    *,
    k_factor: int = ELO_K_FACTOR,
) -> int:
    """Rating change of side A; side B always receives the negated value.

    ``score_a`` is ``1.0`` for a win, ``0.5`` for a draw, ``0.0`` for a loss.
    """
    raw = float(k_factor) * (float(score_a) - expected_score(rating_a, rating_b))
    return _round_half_away(raw)


def apply_elo(
    rating_a: int,
    rating_b: int,
    *,
    winner: str | None,
) -> tuple[int, int, int, int]:
    """Return ``(before_a, after_a, before_b, after_b)`` for one match.

    ``winner`` is ``"player"``, ``"opponent"`` or ``None`` for a draw. Both
    after-values are clamped to ``>= 0`` and use the same delta, so the rating
    pool is conserved unless clamping kicks in at zero.
    """
    before_a = max(0, int(rating_a))
    before_b = max(0, int(rating_b))

    if winner == "player":
        score_a = 1.0
    elif winner == "opponent":
        score_a = 0.0
    else:
        score_a = 0.5

    delta = elo_delta(before_a, before_b, score_a)
    after_a = max(0, before_a + delta)
    after_b = max(0, before_b - delta)
    return before_a, after_a, before_b, after_b


# ---------------------------------------------------------------------------
# JSON helpers used by the API and the WebSocket layer
# ---------------------------------------------------------------------------
def stats_payload(participant: Any) -> dict[str, Any]:
    """Public counters of a participant row (model or duck-typed object)."""
    notes = int(getattr(participant, "notes_played", 0) or 0)
    perfect = int(getattr(participant, "perfect_count", 0) or 0)
    great = int(getattr(participant, "great_count", 0) or 0)
    good = int(getattr(participant, "good_count", 0) or 0)
    miss = int(getattr(participant, "miss_count", 0) or 0)
    return {
        "perfect": perfect,
        "great": great,
        "good": good,
        "miss": miss,
        "notes": notes,
        "accuracy": round(accuracy_factor(perfect, great, good, notes) * 100.0, 2),
    }
