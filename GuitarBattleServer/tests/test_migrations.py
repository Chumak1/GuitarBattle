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
STAGE_TABLE = "stage_levels"
BEFORE_STAGE_STANDALONE = "0024_duel_lobbies"
BEFORE_CHARACTER_UNLOCKS = "0028_campaign_song_bot_level"


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


def test_stage_levels_are_standalone_and_keep_rows(db_engine, clean_db):
    """0025 drops the stage -> character link without touching other tables."""
    config = _alembic_config()
    assert _has_table(db_engine, "ui_characters")
    assert "character_id" not in _columns(db_engine, STAGE_TABLE)
    # Song and player character selection stays in place.
    assert "character_id" in _columns(db_engine, "songs")
    assert "character_id" in _columns(db_engine, "users")
    assert "public_id" in _columns(db_engine, "ui_characters")

    stage_id = uuid.uuid4()
    with db_engine.begin() as connection:
        connection.execute(
            text(
                """
                INSERT INTO stage_levels (
                    id, name, background_key, content_type,
                    size_bytes, sha256
                )
                VALUES (
                    :id, 'Migration Stage', :key, 'image/png',
                    4, :sha
                )
                """
            ),
            {
                "id": stage_id,
                "key": f"{stage_id}.png",
                "sha": "0" * 64,
            },
        )

    try:
        command.downgrade(config, BEFORE_STAGE_STANDALONE)
        assert "character_id" in _columns(db_engine, STAGE_TABLE)
        with db_engine.connect() as connection:
            # The old assignment is unrecoverable; the column comes back NULL.
            assert (
                connection.scalar(
                    text(
                        "SELECT character_id FROM stage_levels WHERE id = :id"
                    ),
                    {"id": stage_id},
                )
                is None
            )

        command.upgrade(config, "head")
        assert "character_id" not in _columns(db_engine, STAGE_TABLE)
        with db_engine.connect() as connection:
            assert (
                connection.scalar(
                    text("SELECT count(*) FROM stage_levels WHERE id = :id"),
                    {"id": stage_id},
                )
                == 1
            )
            assert (
                connection.scalar(
                    text("SELECT name FROM stage_levels WHERE id = :id"),
                    {"id": stage_id},
                )
                == "Migration Stage"
            )
    finally:
        command.upgrade(config, "head")


def test_campaign_song_levels_are_backfilled_from_the_campaign(
    db_engine, clean_db
):
    """0028 keeps old towers on exactly the difficulty they already had."""
    campaign_id = uuid.uuid4()
    song_ids = [uuid.uuid4(), uuid.uuid4()]
    link_ids = [uuid.uuid4(), uuid.uuid4()]
    with db_engine.begin() as connection:
        for index, song_id in enumerate(song_ids):
            connection.execute(
                text(
                    """
                    INSERT INTO songs (
                        id, slug, title, genre, bpm, difficulty,
                        duration_seconds, version, is_published
                    )
                    VALUES (
                        :id, :slug, :title, 'Rock', 120, 3, 90, 1, false
                    )
                    """
                ),
                {
                    "id": song_id,
                    "slug": f"backfill-{song_id.hex[:12]}",
                    "title": f"Backfill Song {index}",
                },
            )
        connection.execute(
            text(
                """
                INSERT INTO campaigns (id, name, bot_level)
                VALUES (:id, 'Backfill tower', 2)
                """
            ),
            {"id": campaign_id},
        )
        for position, (link_id, song_id) in enumerate(zip(link_ids, song_ids)):
            connection.execute(
                text(
                    """
                    INSERT INTO campaign_songs (
                        id, campaign_id, song_id, position
                    )
                    VALUES (:id, :campaign_id, :song_id, :position)
                    """
                ),
                {
                    "id": link_id,
                    "campaign_id": campaign_id,
                    "song_id": song_id,
                    "position": position,
                },
            )

    config = _alembic_config()
    try:
        command.downgrade(config, "0027_duel_lobby_start_confirm")
        assert "bot_level" not in _columns(db_engine, "campaign_songs")

        command.upgrade(config, "head")
        assert "bot_level" in _columns(db_engine, "campaign_songs")
        with db_engine.connect() as connection:
            levels = connection.scalars(
                text(
                    "SELECT bot_level FROM campaign_songs "
                    "WHERE campaign_id = :id ORDER BY position"
                ),
                {"id": campaign_id},
            ).all()
        assert levels == [2, 2]
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


def test_statistics_excluded_column_round_trip(db_engine, clean_db):
    """0037 adds a NOT NULL boolean that defaults to "counted" for old rows."""
    config = _alembic_config()
    assert "statistics_excluded" in _columns(db_engine, "users")

    user_id = uuid.uuid4()
    with db_engine.begin() as connection:
        connection.execute(
            text(
                """
                INSERT INTO users (
                    id, username, email, password_hash, display_name,
                    email_verified, is_admin, is_active, token_version,
                    xp_total, level
                )
                VALUES (
                    :id, :username, :email, 'x', 'Stats User',
                    true, false, true, 0, 0, 1
                )
                """
            ),
            {
                "id": user_id,
                "username": f"stats_{user_id.hex[:12]}",
                "email": f"stats_{user_id.hex[:12]}@example.com",
            },
        )
        # The server default counts the account without an explicit value.
        assert (
            connection.scalar(
                text("SELECT statistics_excluded FROM users WHERE id = :id"),
                {"id": user_id},
            )
            is False
        )

    try:
        command.downgrade(config, "0036_artwork_avatars")
        assert "statistics_excluded" not in _columns(db_engine, "users")
        with db_engine.connect() as connection:
            assert (
                connection.scalar(
                    text("SELECT count(*) FROM users WHERE id = :id"),
                    {"id": user_id},
                )
                == 1
            )

        command.upgrade(config, "head")
        assert "statistics_excluded" in _columns(db_engine, "users")
        with db_engine.connect() as connection:
            # The flag cannot be restored by the downgrade; every account is
            # counted again, which is the safe default.
            assert (
                connection.scalar(
                    text("SELECT statistics_excluded FROM users WHERE id = :id"),
                    {"id": user_id},
                )
                is False
            )
    finally:
        command.upgrade(config, "head")


def test_character_unlocks_backfill_keeps_starters_and_selection(
    db_engine, clean_db
):
    """0029 gives every user the starters plus the character they already had."""
    character_ids = {"Ray": uuid.uuid4(), "Luna": uuid.uuid4(), "Vex": uuid.uuid4()}
    user_id = uuid.uuid4()
    with db_engine.begin() as connection:
        for name, character_id in character_ids.items():
            connection.execute(
                text(
                    """
                    INSERT INTO ui_characters (
                        id, name, sheet_key, content_type, size_bytes, sha256
                    )
                    VALUES (:id, :name, :key, 'image/png', 4, :sha)
                    """
                ),
                {
                    "id": character_id,
                    "name": name,
                    "key": f"{character_id}.png",
                    "sha": "0" * 64,
                },
            )
        connection.execute(
            text(
                """
                INSERT INTO users (
                    id, username, email, password_hash, display_name,
                    email_verified, is_admin, is_active, token_version,
                    xp_total, level, character_id
                )
                VALUES (
                    :id, :username, :email, 'x', 'Unlock User',
                    true, false, true, 0, 0, 1, :character_id
                )
                """
            ),
            {
                "id": user_id,
                "username": f"unlock_{user_id.hex[:12]}",
                "email": f"unlock_{user_id.hex[:12]}@example.com",
                "character_id": character_ids["Vex"],
            },
        )

    config = _alembic_config()
    try:
        command.downgrade(config, BEFORE_CHARACTER_UNLOCKS)
        assert not _has_table(db_engine, "user_characters")
        assert not _has_table(db_engine, "user_campaign_progress")
        assert "is_starter" not in _columns(db_engine, "ui_characters")
        assert "reward_character_id" not in _columns(db_engine, "campaigns")

        command.upgrade(config, "head")
        assert _has_table(db_engine, "user_characters")
        assert _has_table(db_engine, "user_campaign_progress")
        assert "is_starter" in _columns(db_engine, "ui_characters")

        with db_engine.connect() as connection:
            starters = connection.scalars(
                text(
                    "SELECT name FROM ui_characters "
                    "WHERE is_starter = true ORDER BY name"
                )
            ).all()
            assert starters == ["Luna", "Ray"]

            owned = connection.execute(
                text(
                    """
                    SELECT c.name, uc.source_campaign_id
                    FROM user_characters AS uc
                    JOIN ui_characters AS c ON c.id = uc.character_id
                    WHERE uc.user_id = :user_id
                    ORDER BY c.name
                    """
                ),
                {"user_id": user_id},
            ).all()
        # Starter rows plus the previously selected character, no duplicates,
        # and no provenance for either (they were not granted by a tower).
        assert [row[0] for row in owned] == ["Luna", "Ray", "Vex"]
        assert all(row[1] is None for row in owned)
    finally:
        command.upgrade(config, "head")
