"""Read-only reward rules exposed by ``GET /api/v1/gameplay/rules``.

The values come from :mod:`app.services.reward_config` and are the same numbers
the server applies when it computes XP. Clients may display them; they must not
send them back — every award is recomputed server-side.
"""
from pydantic import BaseModel


class GameplayRulesResponse(BaseModel):
    #: Attempts below this completion never award XP (solo, Battle and Online).
    min_completion_for_xp: float
    #: Battle (tower) XP decay per full completion: factor = decay ** n.
    tower_repeat_decay: float
    tower_repeat_floor: float
    #: Online duel repeat slots by finished duels of the song today, then floor.
    online_repeat_factors: list[float]
    online_repeat_floor: float
    #: One-time bonus for a strictly better result, as a fraction of base XP.
    pb_bonus_factor: float
    #: ``normalized_score`` is ``NULL`` below this effective completion.
    normalization_min_completion: float
    #: Hard cap of the normalization multiplier (normalized <= actual × this).
    normalization_max_multiplier: float
