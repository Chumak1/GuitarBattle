"""Alembic smoke tests for the app audio migration.

The suite rebuilds the whole schema from the real migrations once per session,
so ``head`` is already applied here. These tests verify that the new revision
is reversible and does not touch existing song data:

* empty database: ``downgrade -1`` removes the table, ``upgrade head`` restores it;
* populated database: a song row survives the round-trip untouched.

A failed assertion still restores ``head`` in ``finally`` so later tests do not
run against a half-downgraded schema.
"""
from __future__ import annotations

import uuid

import pytest
from alembic import command
from alembic.config import Config
from sqlalchemy import inspect, text

from tests.conftest import SERVER_ROOT, TEST_DATABASE_DSN

pytestmark = pytest.mark.db

TABLE = "app_audio_assets"


def _alembic_config() -> Config:
    config = Config()
    config.set_main_option("script_location", str(SERVER_ROOT / "migrations"))
    config.set_main_option("sqlalchemy.url", TEST_DATABASE_DSN.replace("%", "%%"))
    return config


def _has_table(engine) -> bool:
    return inspect(engine).has_table(TABLE)


def test_migration_round_trip_on_empty_database(db_engine, clean_db):
    config = _alembic_config()
    assert _has_table(db_engine)
    try:
        command.downgrade(config, "-1")
        assert not _has_table(db_engine)

        command.upgrade(config, "head")
        assert _has_table(db_engine)
    finally:
        command.upgrade(config, "head")


def test_migration_round_trip_keeps_song_rows(db_engine, clean_db):
    song_id = uuid.uuid4()
    with db_engine.begin() as connection:
        connection.execute(
            text(
                """
                INSERT INTO songs (
                    id, slug, title, genre, bpm, difficulty,
                    duration_seconds, version, is_published
                )
                VALUES (
                    :id, :slug, 'Migration Test Song', 'Rock', 120, 3, 90, 1, false
                )
                """
            ),
            {"id": song_id, "slug": f"migration-test-{song_id.hex[:12]}"},
        )

    config = _alembic_config()
    try:
        command.downgrade(config, "-1")
        assert not _has_table(db_engine)
        with db_engine.connect() as connection:
            assert (
                connection.scalar(
                    text("SELECT count(*) FROM songs WHERE id = :id"),
                    {"id": song_id},
                )
                == 1
            )

        command.upgrade(config, "head")
        assert _has_table(db_engine)
        with db_engine.connect() as connection:
            assert (
                connection.scalar(
                    text("SELECT count(*) FROM songs WHERE id = :id"),
                    {"id": song_id},
                )
                == 1
            )
            assert (
                connection.scalar(text(f"SELECT count(*) FROM {TABLE}")) == 0
            )
    finally:
        command.upgrade(config, "head")
