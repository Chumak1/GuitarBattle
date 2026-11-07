"""Pure unit tests for the Guitar Duels rules and rating helpers.

These tests never touch PostgreSQL: they pin the Python port to the client
``DuelEngine.h`` semantics (quality points, damage clamp, symmetry, bot hash)
and to the XP/Elo formulas documented in ``docs/DUELS_API.md``.
"""
from __future__ import annotations

import itertools

import pytest

from app.services import duel_rules
from app.services.duel_rules import DuelExchange, DuelQuality


QUALITIES = (
    DuelQuality.PERFECT,
    DuelQuality.GREAT,
    DuelQuality.GOOD,
    DuelQuality.MISS,
)


def test_quality_points_match_the_client():
    assert duel_rules.quality_points(DuelQuality.PERFECT) == 100.0
    assert duel_rules.quality_points(DuelQuality.GREAT) == 85.0
    assert duel_rules.quality_points(DuelQuality.GOOD) == 70.0
    assert duel_rules.quality_points(DuelQuality.MISS) == 0.0
    assert DuelQuality.from_token("PERFECT") is DuelQuality.PERFECT
    with pytest.raises(ValueError):
        DuelQuality.from_token("almost")


def test_damage_constants_match_the_client_build():
    # Keep in sync with GuitarBattle/Source/Gameplay/DuelEngine.h (DuelConfig).
    assert duel_rules.RULES_VERSION == 2
    assert duel_rules.MAX_HEALTH == 100.0
    assert duel_rules.DAMAGE_PER_POINT == 0.10
    assert duel_rules.MIN_DAMAGE == 1.0
    assert duel_rules.MAX_DAMAGE == 10.0


def test_both_miss_is_a_no_damage_exchange():
    result = duel_rules.resolve_note(DuelQuality.MISS, DuelQuality.MISS)
    assert result.exchange is DuelExchange.BOTH_MISS
    assert result.damage_to_player == 0.0
    assert result.damage_to_opponent == 0.0


@pytest.mark.parametrize(
    "quality", [DuelQuality.PERFECT, DuelQuality.GREAT, DuelQuality.GOOD]
)
def test_equal_qualities_block(quality):
    result = duel_rules.resolve_note(quality, quality)
    assert result.exchange is DuelExchange.BLOCK
    assert result.blocked is True
    assert result.damage_to_player == 0.0
    assert result.damage_to_opponent == 0.0


def test_player_hit_and_opponent_hit_are_symmetric():
    player = duel_rules.resolve_note(DuelQuality.PERFECT, DuelQuality.GOOD)
    assert player.exchange is DuelExchange.PLAYER_HIT
    assert player.damage_to_opponent == pytest.approx(3.0)
    assert player.damage_to_player == 0.0

    opponent = duel_rules.resolve_note(DuelQuality.GOOD, DuelQuality.PERFECT)
    assert opponent.exchange is DuelExchange.OPPONENT_HIT
    assert opponent.damage_to_player == pytest.approx(3.0)
    assert opponent.damage_to_opponent == 0.0


@pytest.mark.parametrize("left,right", list(itertools.product(QUALITIES, repeat=2)))
def test_every_pair_is_symmetric(left, right):
    forward = duel_rules.resolve_note(left, right)
    backward = duel_rules.resolve_note(right, left)
    assert forward.damage_to_opponent == pytest.approx(backward.damage_to_player)
    assert forward.damage_to_player == pytest.approx(backward.damage_to_opponent)
    swapped = {
        DuelExchange.PLAYER_HIT: DuelExchange.OPPONENT_HIT,
        DuelExchange.OPPONENT_HIT: DuelExchange.PLAYER_HIT,
        DuelExchange.BLOCK: DuelExchange.BLOCK,
        DuelExchange.BOTH_MISS: DuelExchange.BOTH_MISS,
    }[forward.exchange]
    assert backward.exchange is swapped


def test_one_sided_hit_uses_the_full_quality_difference():
    # Perfect (100) vs Miss (0): 100 * 0.10 = 10.0 (the ceiling)
    result = duel_rules.resolve_note(DuelQuality.PERFECT, DuelQuality.MISS)
    assert result.damage_to_opponent == pytest.approx(10.0)

    # Great (85) vs Miss: 85 * 0.10 = 8.5
    result = duel_rules.resolve_note(DuelQuality.GREAT, DuelQuality.MISS)
    assert result.damage_to_opponent == pytest.approx(8.5)

    # Good (70) vs Miss: 70 * 0.10 = 7.0
    result = duel_rules.resolve_note(DuelQuality.GOOD, DuelQuality.MISS)
    assert result.damage_to_opponent == pytest.approx(7.0)


def test_damage_is_clamped_to_the_minimum():
    # Great (85) vs Good (70): 15 * 0.10 = 1.5, above the 1.0 floor.
    result = duel_rules.resolve_note(DuelQuality.GREAT, DuelQuality.GOOD)
    assert result.damage_to_opponent == pytest.approx(1.5)

    # No quality pair can produce less than the floor, so pin the helper.
    assert duel_rules.clamp(duel_rules.MIN_DAMAGE, duel_rules.MAX_DAMAGE, 0.2) == 1.0


def test_damage_never_exceeds_the_maximum():
    # Perfect vs Miss is exactly the documented 10.0 ceiling.
    assert duel_rules.clamp(duel_rules.MIN_DAMAGE, duel_rules.MAX_DAMAGE, 99.0) == 10.0
    result = duel_rules.resolve_note(DuelQuality.PERFECT, DuelQuality.MISS)
    assert result.damage_to_opponent == pytest.approx(duel_rules.MAX_DAMAGE)
    assert result.damage_to_opponent <= duel_rules.MAX_DAMAGE


@pytest.mark.parametrize(
    "health,damage,expected",
    [
        (100.0, 0.0, 100.0),
        (100.0, 3.0, 97.0),
        (100.0, 250.0, 0.0),
        (2.0, 3.0, 0.0),
        (0.0, 1.0, 0.0),
        (50.0, -5.0, 50.0),
    ],
)
def test_health_stays_inside_zero_and_one_hundred(health, damage, expected):
    assert duel_rules.apply_damage(health, damage) == pytest.approx(expected)


def test_bot_hash_is_the_documented_32_bit_function():
    # Values pinned against the C++ implementation: changing the hash silently
    # would make stored bot matches unreplayable.
    assert duel_rules.duel_bot_hash(0, 0) == 0
    assert duel_rules.duel_bot_hash(123_456_789, 42) == 2_935_934_280


def test_bot_is_deterministic_and_levels_are_ordered():
    first = duel_rules.bot_qualities("hard", 42, 500)
    second = duel_rules.bot_qualities("hard", 42, 500)
    assert first == second
    assert set(first) <= set(QUALITIES)

    def hit_rate(level: str) -> float:
        values = duel_rules.bot_qualities(level, 7, 1_000)
        return sum(1 for value in values if value is not DuelQuality.MISS) / 1_000.0

    easy = hit_rate("easy")
    normal = hit_rate("normal")
    hard = hit_rate("hard")
    assert easy < normal < hard
    assert 0.15 < easy < 0.35
    assert 0.55 < normal < 0.75
    assert 0.80 < hard < 0.98


def test_accuracy_uses_the_client_formula():
    # 1 perfect, 1 great, 1 good, 1 miss over 4 notes:
    # (1 + 0.85 + 0.70) / 4 = 0.6375 -> 63.75 %
    assert duel_rules.accuracy_factor(1, 1, 1, 4) == pytest.approx(0.6375)
    assert duel_rules.accuracy_factor(0, 0, 0, 0) == 0.0


def test_xp_matches_the_client_rewards_formula():
    # 20 + 2*5 + 1*3 + 3*2 + 40 = 79
    assert (
        duel_rules.xp_for_duel(perfect=2, great=1, good=3, result="win") == 79
    )
    assert (
        duel_rules.xp_for_duel(perfect=2, great=1, good=3, result="loss") == 39
    )
    assert (
        duel_rules.xp_for_duel(perfect=2, great=1, good=3, result="draw") == 39
    )
    assert duel_rules.xp_for_duel(perfect=0, great=0, good=0, result="loss") == 20


def test_elo_equal_ratings_win_and_loss():
    before_a, after_a, before_b, after_b = duel_rules.apply_elo(
        1000, 1000, winner="player"
    )
    assert (before_a, after_a) == (1000, 1016)
    assert (before_b, after_b) == (1000, 984)
    assert after_a + after_b == before_a + before_b

    before_a, after_a, before_b, after_b = duel_rules.apply_elo(
        1000, 1000, winner="opponent"
    )
    assert (after_a, after_b) == (984, 1016)
    assert after_a + after_b == before_a + before_b


def test_elo_draw_keeps_equal_ratings():
    _, after_a, _, after_b = duel_rules.apply_elo(1200, 1200, winner=None)
    assert (after_a, after_b) == (1200, 1200)


def test_elo_upset_moves_more_than_a_favourite_win():
    _, favourite_after, _, _ = duel_rules.apply_elo(1400, 1000, winner="player")
    _, _favourite_loss, _, underdog_after = duel_rules.apply_elo(
        1400, 1000, winner="opponent"
    )
    assert favourite_after - 1400 < 16
    assert underdog_after - 1000 > 16


def test_elo_never_goes_negative_and_conserves_the_pool():
    before_a, after_a, before_b, after_b = duel_rules.apply_elo(
        5, 1200, winner="opponent"
    )
    assert after_a >= 0
    assert after_b >= 0
    # Clamping at zero is the only case where the sum may differ.
    assert after_a + after_b <= before_a + before_b
