"""Song time signature (meter): schema, API, admin page and migration 0020.

The client places the metronome and the bar lines from these two fields, so
the tests cover the whole additive path:

* Pydantic: ``7/8`` is accepted, MIDI-invalid numerators/denominators are
  rejected with 422 before they reach the database;
* API: create/list/detail/update round-trip, ``PATCH null`` is refused and a
  row inserted without the columns falls back to ``4/4`` (server default);
* admin page: both form fields, the "Размер" column and the payload keys;
* migration: offline SQL of ``0019 -> 0020`` adds ``NOT NULL DEFAULT '4'``
  columns and both CHECK constraints, and the downgrade removes them.
"""
from __future__ import annotations

import io
import uuid
from pathlib import Path

import pytest
from alembic import command
from alembic.config import Config
from pydantic import ValidationError
from sqlalchemy import text

from app.schemas.admin_songs import AdminSongCreate, AdminSongUpdate
from tests import helpers
from tests.conftest import SERVER_ROOT

ADMIN_STATIC_DIR = SERVER_ROOT / "app" / "static" / "admin"
DENOMINATOR_ERROR = (
    "Time signature denominator must be one of 1, 2, 4, 8, 16, 32"
)


def _create_payload(**overrides) -> dict:
    payload = {
        "slug": "seven-eight",
        "title": "Seven Eight",
        "bpm": 140,
    }
    payload.update(overrides)
    return payload


# ---------------------------------------------------------------------------
# Pydantic schemas (no database)
# ---------------------------------------------------------------------------
def test_create_schema_accepts_seven_eight():
    song = AdminSongCreate(**_create_payload(
        time_signature_numerator=7, time_signature_denominator=8
    ))
    assert song.time_signature_numerator == 7
    assert song.time_signature_denominator == 8


def test_create_schema_defaults_to_four_four():
    song = AdminSongCreate(**_create_payload())
    assert song.time_signature_numerator == 4
    assert song.time_signature_denominator == 4


@pytest.mark.parametrize("numerator", [0, 33])
def test_create_schema_rejects_numerator_out_of_range(numerator):
    with pytest.raises(ValidationError):
        AdminSongCreate(**_create_payload(time_signature_numerator=numerator))


@pytest.mark.parametrize("denominator", [3, 5])
def test_create_schema_rejects_non_midi_denominator(denominator):
    with pytest.raises(ValidationError, match=DENOMINATOR_ERROR):
        AdminSongCreate(**_create_payload(time_signature_denominator=denominator))


@pytest.mark.parametrize("denominator", [1, 2, 4, 8, 16, 32])
def test_create_schema_accepts_every_midi_denominator(denominator):
    song = AdminSongCreate(**_create_payload(time_signature_denominator=denominator))
    assert song.time_signature_denominator == denominator


def test_update_schema_without_meter_keeps_fields_unset():
    changes = AdminSongUpdate(title="Renamed").model_dump(exclude_unset=True)
    assert changes == {"title": "Renamed"}
    assert "time_signature_numerator" not in changes
    assert "time_signature_denominator" not in changes


def test_update_schema_rejects_non_midi_denominator():
    with pytest.raises(ValidationError, match=DENOMINATOR_ERROR):
        AdminSongUpdate(time_signature_denominator=5)


# ---------------------------------------------------------------------------
# API round-trip (PostgreSQL)
# ---------------------------------------------------------------------------
@pytest.mark.db
def test_admin_and_public_api_expose_time_signature(db_client, admin_user):
    headers = admin_user["headers"]
    song = helpers.create_song(
        db_client,
        headers,
        time_signature_numerator=7,
        time_signature_denominator=8,
    )
    assert song["time_signature_numerator"] == 7
    assert song["time_signature_denominator"] == 8

    helpers.publish_song(db_client, headers, song["id"])

    listing = db_client.get(f"{helpers.API}/songs")
    assert listing.status_code == 200
    item = listing.json()["items"][0]
    assert item["time_signature_numerator"] == 7
    assert item["time_signature_denominator"] == 8

    detail = db_client.get(f"{helpers.API}/songs/{song['slug']}")
    assert detail.status_code == 200
    body = detail.json()
    assert body["time_signature_numerator"] == 7
    assert body["time_signature_denominator"] == 8

    updated = db_client.patch(
        f"{helpers.API}/admin/songs/{song['id']}",
        json={"time_signature_numerator": 3, "time_signature_denominator": 4},
        headers=headers,
    )
    assert updated.status_code == 200, updated.text
    assert updated.json()["time_signature_numerator"] == 3
    assert updated.json()["time_signature_denominator"] == 4

    public = db_client.get(f"{helpers.API}/songs/{song['slug']}").json()
    assert public["time_signature_numerator"] == 3
    assert public["time_signature_denominator"] == 4


@pytest.mark.db
def test_patch_rejects_null_time_signature(db_client, admin_user, draft_song):
    headers = admin_user["headers"]
    response = db_client.patch(
        f"{helpers.API}/admin/songs/{draft_song['id']}",
        json={"time_signature_numerator": None},
        headers=headers,
    )
    assert response.status_code == 422, response.text

    unchanged = db_client.get(
        f"{helpers.API}/admin/songs/{draft_song['id']}", headers=headers
    ).json()
    assert unchanged["time_signature_numerator"] == 4
    assert unchanged["time_signature_denominator"] == 4


@pytest.mark.db
def test_create_rejects_invalid_denominator_with_422(db_client, admin_user):
    payload = {
        "slug": helpers.unique_slug("meter"),
        "title": "Invalid Meter",
        "bpm": 120,
        "time_signature_numerator": 7,
        "time_signature_denominator": 5,
    }
    response = db_client.post(
        f"{helpers.API}/admin/songs", json=payload, headers=admin_user["headers"]
    )
    assert response.status_code == 422, response.text


@pytest.mark.db
def test_song_without_explicit_meter_defaults_to_four_four(
    db_client, db_engine, clean_db
):
    """Rows written by an older code path (no meter columns) become 4/4."""
    song_id = uuid.uuid4()
    slug = helpers.unique_slug("legacy-meter")
    with db_engine.begin() as connection:
        connection.execute(
            text(
                """
                INSERT INTO songs (
                    id, slug, title, genre, bpm, difficulty,
                    duration_seconds, version, is_published
                )
                VALUES (
                    :id, :slug, 'Legacy Song', 'Rock', 100, 2, 60, 1, true
                )
                """
            ),
            {"id": song_id, "slug": slug},
        )

    body = db_client.get(f"{helpers.API}/songs/{slug}")
    assert body.status_code == 200, body.text
    payload = body.json()
    assert payload["time_signature_numerator"] == 4
    assert payload["time_signature_denominator"] == 4


# ---------------------------------------------------------------------------
# Admin page (static assets)
# ---------------------------------------------------------------------------
def test_admin_songs_page_has_meter_column_and_form_fields():
    html = (ADMIN_STATIC_DIR / "index.html").read_text(encoding="utf-8")
    assert ">Размер</th>" in html
    assert 'id="songTimeSignatureNumerator"' in html
    assert 'id="songTimeSignatureDenominator"' in html
    assert 'type="number" min="1" max="32" value="4" required' in html
    assert '<option value="8">' in html
    # Songs table gained the meter column: nine cells in the loading row.
    assert 'colspan="9"' in html
    assert "admin.js?v=20261115-1" in html


def test_admin_script_sends_meter_in_payload():
    script = (ADMIN_STATIC_DIR / "admin.js").read_text(encoding="utf-8")
    assert "time_signature_numerator" in script
    assert "time_signature_denominator" in script
    assert 'el("songTimeSignatureNumerator").value' in script
    assert 'el("songTimeSignatureDenominator").value' in script


# ---------------------------------------------------------------------------
# Migration 0020 offline SQL
# ---------------------------------------------------------------------------
def _alembic_config() -> Config:
    config = Config()
    config.set_main_option("script_location", str(SERVER_ROOT / "migrations"))
    return config


def _offline_sql(direction: str) -> str:
    config = _alembic_config()
    buffer = io.StringIO()
    # ``Commands`` write offline SQL to ``Config.output_buffer``; without it the
    # statements go to stdout and pytest's capture would swallow them.
    config.output_buffer = buffer
    config.attributes["output_buffer"] = buffer
    if direction == "upgrade":
        command.upgrade(
            config, "0019_user_country:0020_song_time_signature", sql=True
        )
    else:
        command.downgrade(
            config, "0020_song_time_signature:0019_user_country", sql=True
        )
    return buffer.getvalue()


def test_migration_offline_adds_not_null_default_columns_and_checks():
    sql = _offline_sql("upgrade")
    assert (
        "ALTER TABLE songs ADD COLUMN time_signature_numerator "
        "SMALLINT DEFAULT '4' NOT NULL"
    ) in sql
    assert (
        "ALTER TABLE songs ADD COLUMN time_signature_denominator "
        "SMALLINT DEFAULT '4' NOT NULL"
    ) in sql
    assert "ck_songs_time_signature_numerator" in sql
    assert (
        "CHECK (time_signature_numerator >= 1 AND time_signature_numerator <= 32)"
        in sql
    )
    assert "ck_songs_time_signature_denominator" in sql
    assert "CHECK (time_signature_denominator IN (1, 2, 4, 8, 16, 32))" in sql


def test_migration_offline_downgrade_drops_checks_and_columns():
    sql = _offline_sql("downgrade")
    assert "DROP CONSTRAINT ck_songs_time_signature_denominator" in sql
    assert "DROP CONSTRAINT ck_songs_time_signature_numerator" in sql
    assert "DROP COLUMN time_signature_denominator" in sql
    assert "DROP COLUMN time_signature_numerator" in sql
