"""Alembic smoke tests for the schema additions.

The suite rebuilds the whole schema from the real migrations once per session,
so ``head`` is already applied here. These tests verify that the revisions are
reversible and do not touch existing data:

* empty database: downgrading to the pre-app-audio revision removes the tables
  added by 0013 and 0014, upgrading restores them;
* populated database: a song and a user row survive the round-trip untouched.

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
BEFORE_APP_AUDIO = "0012_song_archive"


def _alembic_config() -> Config:
    config = Config()
    config.set_main_option("script_location", str(SERVER_ROOT / "migrations"))
    config.set_main_option("sqlalchemy.url", TEST_DATABASE_DSN.replace("%", "%%"))
    return config


def _has_table(engine, name: str) -> bool:
    return inspect(engine).has_table(name)


def _columns(engine, table: str) -> set[str]:
    # A fresh inspector per call: Alembic DDL invalidates the cached reflection.
    return {column["name"] for column in inspect(engine).get_columns(table)}


def test_migration_round_trip_on_empty_database(db_engine, clean_db):
    config = _alembic_config()
    assert _has_table(db_engine, TABLE)
    assert _has_table(db_engine, "refresh_sessions")
    assert "privacy_consent_at" in _columns(db_engine, "users")
    try:
        command.downgrade(config, BEFORE_APP_AUDIO)
        assert not _has_table(db_engine, TABLE)
        assert not _has_table(db_engine, "refresh_sessions")
        assert "privacy_consent_at" not in _columns(db_engine, "users")

        command.upgrade(config, "head")
        assert _has_table(db_engine, TABLE)
        assert _has_table(db_engine, "refresh_sessions")
        assert "privacy_consent_at" in _columns(db_engine, "users")
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
        command.downgrade(config, BEFORE_APP_AUDIO)
        assert not _has_table(db_engine, TABLE)
        with db_engine.connect() as connection:
            assert (
                connection.scalar(
                    text("SELECT count(*) FROM songs WHERE id = :id"),
                    {"id": song_id},
                )
                == 1
            )

        command.upgrade(config, "head")
        assert _has_table(db_engine, TABLE)
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


def test_migration_round_trip_keeps_users_and_clears_only_new_column(db_engine, clean_db):
    user_id = uuid.uuid4()
    with db_engine.begin() as connection:
        connection.execute(
            text(
                """
                INSERT INTO users (
                    id, username, email, password_hash, display_name,
                    email_verified, is_admin, is_active, token_version,
                    xp_total, level, privacy_consent_at
                )
                VALUES (
                    :id, :username, :email, 'x', 'Migration User',
                    true, false, true, 0, 10, 1, now()
                )
                """
            ),
            {
                "id": user_id,
                "username": f"migration_{user_id.hex[:12]}",
                "email": f"migration_{user_id.hex[:12]}@example.com",
            },
        )

    config = _alembic_config()
    try:
        command.downgrade(config, BEFORE_APP_AUDIO)
        with db_engine.connect() as connection:
            assert (
                connection.scalar(
                    text("SELECT count(*) FROM users WHERE id = :id"),
                    {"id": user_id},
                )
                == 1
            )

        command.upgrade(config, "head")
        with db_engine.connect() as connection:
            # The account survives; the consent date cannot be restored by the
            # downgrade, which is expected and only affects the new column.
            assert (
                connection.scalar(
                    text("SELECT count(*) FROM users WHERE id = :id"),
                    {"id": user_id},
                )
                == 1
            )
            assert (
                connection.scalar(
                    text("SELECT privacy_consent_at FROM users WHERE id = :id"),
                    {"id": user_id},
                )
                is None
            )
            assert (
                connection.scalar(text("SELECT count(*) FROM refresh_sessions")) == 0
            )
    finally:
        command.upgrade(config, "head")
