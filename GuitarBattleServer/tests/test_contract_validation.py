"""Contract tests for request schemas shared with the JUCE client.

These are pure unit tests (no database) so the documented JSON contract is
checked even when the disposable test database is unavailable.
"""
import uuid

import pytest
from pydantic import ValidationError

from app.schemas.admin_songs import AdminSongCreate, AdminSongUpdate
from app.schemas.games import GameSessionCreate, ScoreSubmit
from app.schemas.songs import SongListResponse

VALID_SCORE = {
    "score_points": 1200,
    "accuracy_percent": 97.5,
    "max_combo": 42,
    "perfect_count": 40,
    "good_count": 2,
    "miss_count": 1,
}


def test_valid_score_payload_is_accepted():
    score = ScoreSubmit.model_validate(VALID_SCORE)
    assert score.score_points == 1200
    assert score.accuracy_percent == 97.5


@pytest.mark.parametrize(
    "overrides",
    [
        {"score_points": -1},
        {"score_points": 100_000_001},
        {"accuracy_percent": -0.1},
        {"accuracy_percent": 100.1},
        {"max_combo": -1},
        {"perfect_count": -1},
        {"good_count": 1_000_001},
        {"miss_count": 1_000_001},
        {"score_points": "many"},
        {"accuracy_percent": None},
    ],
)
def test_invalid_score_payload_is_rejected(overrides):
    payload = {**VALID_SCORE, **overrides}
    with pytest.raises(ValidationError):
        ScoreSubmit.model_validate(payload)


def test_score_payload_requires_all_counters():
    with pytest.raises(ValidationError):
        ScoreSubmit.model_validate({"score_points": 1, "accuracy_percent": 100})


def test_session_payload_accepts_supported_speed_range():
    for speed in (50, 100, 120):
        session = GameSessionCreate.model_validate(
            {"song_id": str(uuid.uuid4()), "speed_percent": speed}
        )
        assert session.speed_percent == speed


def test_session_payload_daily_flag_defaults_to_false():
    song_id = str(uuid.uuid4())
    assert GameSessionCreate.model_validate(
        {"song_id": song_id, "speed_percent": 100}
    ).daily is False
    assert GameSessionCreate.model_validate(
        {"song_id": song_id, "speed_percent": 100, "daily": True}
    ).daily is True


def test_score_payload_progression_fields_are_optional():
    """Older clients send the seven legacy fields and keep working."""
    score = ScoreSubmit.model_validate(VALID_SCORE)
    assert score.completion_percent == 100
    assert score.avg_timing_error_ms is None
    assert score.notes_total is None
    assert score.notes_played is None


def test_score_payload_completion_is_clamped_by_note_counters():
    score = ScoreSubmit.model_validate(
        {
            **VALID_SCORE,
            "completion_percent": 100,
            "notes_total": 100,
            "notes_played": 40,
        }
    )
    assert score.completion_percent == pytest.approx(40.0)


@pytest.mark.parametrize(
    "overrides",
    [
        {"completion_percent": 100.1},
        {"completion_percent": -1},
        {"avg_timing_error_ms": -1},
        {"avg_timing_error_ms": 2001},
        {"notes_total": -1},
        {"notes_played": 1_000_001},
        {"notes_total": 10, "notes_played": 11},
    ],
)
def test_score_payload_rejects_invalid_progression_fields(overrides):
    with pytest.raises(ValidationError):
        ScoreSubmit.model_validate({**VALID_SCORE, **overrides})


@pytest.mark.parametrize("speed", [0, 49, 121, 1000])
def test_session_payload_rejects_unsupported_speed(speed):
    with pytest.raises(ValidationError):
        GameSessionCreate.model_validate(
            {"song_id": str(uuid.uuid4()), "speed_percent": speed}
        )


@pytest.mark.parametrize("song_id", ["", "not-a-uuid", "123"])
def test_session_payload_rejects_invalid_song_id(song_id):
    with pytest.raises(ValidationError):
        GameSessionCreate.model_validate({"song_id": song_id, "speed_percent": 100})


def test_admin_song_payload_normalizes_text():
    song = AdminSongCreate.model_validate(
        {
            "slug": "my-song",
            "title": "  My Song  ",
            "genre": "  Rock  ",
            "bpm": 120,
        }
    )
    assert song.slug == "my-song"
    assert song.title == "My Song"
    assert song.genre == "Rock"
    assert song.difficulty == 1
    assert song.duration_seconds == 60


def test_admin_song_slug_pattern_rejects_surrounding_whitespace():
    # Pydantic applies ``pattern`` before the trimming validator, so a slug
    # with spaces is rejected instead of silently normalized. Keep this pinned
    # because clients must send an already-clean slug.
    with pytest.raises(ValidationError):
        AdminSongCreate.model_validate(
            {"slug": "  my-song  ", "title": "My Song", "bpm": 120}
        )


@pytest.mark.parametrize(
    "overrides",
    [
        {"slug": "Bad Slug"},
        {"slug": "-leading-dash"},
        {"slug": "trailing-dash-"},
        {"slug": "double--dash"},
        {"slug": ""},
        {"title": "   "},
        {"genre": "   "},
        {"bpm": 0},
        {"bpm": 401},
        {"difficulty": 0},
        {"difficulty": 11},
        {"duration_seconds": 0},
        {"duration_seconds": 86_401},
    ],
)
def test_invalid_admin_song_payload_is_rejected(overrides):
    payload = {"slug": "valid-slug", "title": "Valid", "bpm": 120, **overrides}
    with pytest.raises(ValidationError):
        AdminSongCreate.model_validate(payload)


def test_admin_song_update_allows_partial_payload_but_rejects_blank_required_text():
    update = AdminSongUpdate.model_validate({"title": "New title"})
    assert update.title == "New title"
    assert update.bpm is None
    with pytest.raises(ValidationError):
        AdminSongUpdate.model_validate({"title": "   "})


def test_song_list_response_contract():
    response = SongListResponse.model_validate(
        {"items": [], "total": 0, "limit": 20, "offset": 0}
    )
    assert response.items == []
    assert response.total == 0
    with pytest.raises(ValidationError):
        SongListResponse.model_validate({"items": []})
