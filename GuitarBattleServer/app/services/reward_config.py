"""Configurable reward tuning shared by Battle (tower) and Online duels.

Everything here is server-side truth: the client never sends a multiplier or an
"xp" value. The values are read from the environment once at import time so a
deployment can adjust the economy without a code change; every setting has a
sane default documented in ``GET /api/v1/gameplay/rules``.

The constants intentionally do **not** replace the existing scoring formula —
:mod:`app.services.progression` still owns ``MIN_COMPLETION_FOR_XP``,
``xp_for_attempt`` and the difficulty/tempo/content factors. This module only
adds the anti-farm multipliers:

* ``tower_repeat_*`` — Battle XP decay by the number of **full** tower
  completions before the current run (1st run 100%, 2nd 50%, 3rd 25% …);
* ``online_repeat_*`` — Online duel XP decay by finished duels of the same song
  within one UTC day (per player);
* ``pb_bonus_factor`` — one-time bonus for a strictly better accuracy/PB;
* ``normalization_min_completion`` / ``normalization_max_multiplier`` — guard
  rails of ``normalized_score`` (early ends are extrapolated, never inflated).
"""
from __future__ import annotations

import math
import os
from typing import Any, Final


def _float(name: str, default: float) -> float:
    raw = os.environ.get(name)
    if raw is None or not raw.strip():
        return default
    try:
        value = float(raw)
    except ValueError:
        return default
    return value if math.isfinite(value) else default


def _factors(name: str, default: tuple[float, ...]) -> tuple[float, ...]:
    raw = os.environ.get(name)
    if raw is None or not raw.strip():
        return default
    parsed: list[float] = []
    for chunk in raw.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        try:
            value = float(chunk)
        except ValueError:
            return default
        if not math.isfinite(value) or value < 0.0:
            return default
        parsed.append(value)
    return tuple(parsed) if parsed else default


#: Multiply the weapon/tower repeat factor by this every full completion.
TOWER_REPEAT_DECAY: float = _float("TOWER_REPEAT_DECAY", 0.5)
#: The tower factor never drops below this value.
TOWER_REPEAT_FLOOR: float = _float("TOWER_REPEAT_FLOOR", 0.05)

#: Online duel repeat slots by finished duels of the same song today.
ONLINE_REPEAT_FACTORS: tuple[float, ...] = _factors(
    "ONLINE_REPEAT_FACTORS", (1.0, 0.5, 0.25, 0.1)
)
#: Used after the last explicit online slot and as the general floor.
ONLINE_REPEAT_FLOOR: float = _float("ONLINE_REPEAT_FLOOR", 0.05)

#: Bonus as a fraction of the base XP for a strictly improved result.
PB_BONUS_FACTOR: float = _float("PB_BONUS_FACTOR", 0.5)

#: Normalization needs at least this effective completion (percentage).
NORMALIZATION_MIN_COMPLETION: float = _float("NORMALIZATION_MIN_COMPLETION", 10.0)
#: ``normalized_score`` is never more than actual × this value.
NORMALIZATION_MAX_MULTIPLIER: float = _float("NORMALIZATION_MAX_MULTIPLIER", 10.0)


def tower_repeat_factor(completions_before: int) -> float:
    """Factor of one Battle run given the full completions before it.

    ``0`` completions → ``1.0`` (every stage of the first run is full XP);
    ``1`` → ``decay``; ``2`` → ``decay ** 2`` … never below the floor.
    """
    count = max(0, int(completions_before))
    if count <= 0:
        return 1.0
    value = TOWER_REPEAT_DECAY**count
    return max(TOWER_REPEAT_FLOOR, value)


def online_repeat_factor(attempts_today: int) -> float:
    """Factor of an online duel by the finished duels of the song today."""
    index = max(0, int(attempts_today))
    if index < len(ONLINE_REPEAT_FACTORS):
        return ONLINE_REPEAT_FACTORS[index]
    return max(ONLINE_REPEAT_FLOOR, ONLINE_REPEAT_FACTORS[-1] if ONLINE_REPEAT_FACTORS else ONLINE_REPEAT_FLOOR)


def rules_payload() -> dict[str, Any]:
    """Read-only snapshot served by ``GET /api/v1/gameplay/rules``."""
    from app.services.progression import MIN_COMPLETION_FOR_XP

    return {
        "min_completion_for_xp": float(MIN_COMPLETION_FOR_XP),
        "tower_repeat_decay": TOWER_REPEAT_DECAY,
        "tower_repeat_floor": TOWER_REPEAT_FLOOR,
        "online_repeat_factors": list(ONLINE_REPEAT_FACTORS),
        "online_repeat_floor": ONLINE_REPEAT_FLOOR,
        "pb_bonus_factor": PB_BONUS_FACTOR,
        "normalization_min_completion": NORMALIZATION_MIN_COMPLETION,
        "normalization_max_multiplier": NORMALIZATION_MAX_MULTIPLIER,
    }


__all__: Final = [
    "TOWER_REPEAT_DECAY",
    "TOWER_REPEAT_FLOOR",
    "ONLINE_REPEAT_FACTORS",
    "ONLINE_REPEAT_FLOOR",
    "PB_BONUS_FACTOR",
    "NORMALIZATION_MIN_COMPLETION",
    "NORMALIZATION_MAX_MULTIPLIER",
    "tower_repeat_factor",
    "online_repeat_factor",
    "rules_payload",
]
