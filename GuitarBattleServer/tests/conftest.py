"""Shared fixtures for the GuitarBattle API test suite.

Test database strategy
----------------------
The suite must never touch the development or production database. It uses a
disposable PostgreSQL instance described by ``TEST_DATABASE_URL``:

    postgresql+psycopg://guitarbattle_test:guitarbattle_test@127.0.0.1:55432/guitarbattle_test

By default that is a throwaway container named ``guitarbattle-test-db`` on host
port 55432 (the compose database does not publish its port). If the container
is not present, the suite tries to start it when Docker is available. If the
database still cannot be reached, all database-backed tests are skipped with a
clear message; pure unit tests keep running.

The database name must contain ``test``; anything else is rejected so a typo in
``TEST_DATABASE_URL`` cannot wipe real data. The schema is rebuilt with the
real Alembic migrations once per session and every test starts with empty
tables.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any

import pytest
from sqlalchemy import create_engine, text
from sqlalchemy.engine import URL, make_url
from sqlalchemy.exc import SQLAlchemyError
from sqlalchemy.orm import sessionmaker
from sqlalchemy.pool import NullPool

SERVER_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TEST_DATABASE_URL = (
    "postgresql+psycopg://guitarbattle_test:guitarbattle_test"
    "@127.0.0.1:55432/guitarbattle_test"
)
TEST_CONTAINER_NAME = "guitarbattle-test-db"


def _normalize_test_url(raw_url: str) -> URL:
    url = make_url(raw_url)
    if url.drivername == "postgresql":
        url = url.set(drivername="postgresql+psycopg")
    if not url.drivername.startswith("postgresql"):
        raise pytest.UsageError(
            f"TEST_DATABASE_URL must point to PostgreSQL, got {url.drivername!r}"
        )
    if not url.database or "test" not in url.database.lower():
        raise pytest.UsageError(
            "Refusing to run tests: TEST_DATABASE_URL database name must contain "
            f"'test' (got {url.database!r}). Tests reset the schema."
        )
    return url


_RAW_TEST_URL = os.environ.get("TEST_DATABASE_URL")
TEST_DATABASE_URL = _normalize_test_url(_RAW_TEST_URL or DEFAULT_TEST_DATABASE_URL)
TEST_DATABASE_DSN = TEST_DATABASE_URL.render_as_string(hide_password=False)
_TEST_URL_IS_DEFAULT = _RAW_TEST_URL is None
SONG_STORAGE = Path(tempfile.mkdtemp(prefix="guitarbattle-test-files-"))
APP_AUDIO_STORAGE = Path(tempfile.mkdtemp(prefix="guitarbattle-test-app-audio-"))
GAME_BUILDS_STORAGE = Path(tempfile.mkdtemp(prefix="guitarbattle-test-game-builds-"))
GAMEPLAY_STORAGE = Path(tempfile.mkdtemp(prefix="guitarbattle-test-gameplay-"))
CAMPAIGN_STORAGE = Path(tempfile.mkdtemp(prefix="guitarbattle-test-campaigns-"))

# Configure the application through the environment *before* importing it, so
# settings.database_url and the SQLAlchemy engine point at the test database.
os.environ["APP_ENV"] = "test"
os.environ["LOG_LEVEL"] = "WARNING"
os.environ["SECRET_KEY"] = "guitarbattle-test-secret-key-not-for-production"
os.environ["ACCESS_TOKEN_EXPIRE_MINUTES"] = "60"
os.environ["SONG_FILES_DIR"] = str(SONG_STORAGE)
os.environ["APP_AUDIO_DIR"] = str(APP_AUDIO_STORAGE)
os.environ["GAME_BUILDS_DIR"] = str(GAME_BUILDS_STORAGE)
os.environ["GAMEPLAY_MEDIA_DIR"] = str(GAMEPLAY_STORAGE)
os.environ["CAMPAIGN_MEDIA_DIR"] = str(CAMPAIGN_STORAGE)
os.environ["POSTGRES_USER"] = TEST_DATABASE_URL.username or ""
os.environ["POSTGRES_PASSWORD"] = TEST_DATABASE_URL.password or ""
os.environ["POSTGRES_HOST"] = TEST_DATABASE_URL.host or "127.0.0.1"
os.environ["POSTGRES_PORT"] = str(TEST_DATABASE_URL.port or 5432)
os.environ["POSTGRES_DB"] = TEST_DATABASE_URL.database or ""

from fastapi.testclient import TestClient  # noqa: E402

from app import models  # noqa: F401,E402  (register every model on Base.metadata)
from app.database import Base, SessionLocal, engine as app_engine  # noqa: E402
from app.main import app  # noqa: E402

from tests import helpers  # noqa: E402


# ---------------------------------------------------------------------------
# Disposable database lifecycle
# ---------------------------------------------------------------------------
def _docker(*args: str, timeout: int = 30) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(
            ["docker", *args],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (FileNotFoundError, subprocess.SubprocessError, OSError):
        return None


def _container_is_running(name: str) -> bool | None:
    result = _docker("inspect", "-f", "{{.State.Running}}", name)
    if result is None or result.returncode != 0:
        return None
    return result.stdout.strip().lower() == "true"


def _start_disposable_container() -> bool:
    """Start (or reuse) the throwaway Postgres container on host port 55432."""
    state = _container_is_running(TEST_CONTAINER_NAME)
    if state is True:
        return True
    if state is False:
        result = _docker("start", TEST_CONTAINER_NAME, timeout=60)
        return result is not None and result.returncode == 0

    result = _docker(
        "run",
        "--rm",
        "-d",
        "--name",
        TEST_CONTAINER_NAME,
        "-e",
        f"POSTGRES_USER={TEST_DATABASE_URL.username}",
        "-e",
        f"POSTGRES_PASSWORD={TEST_DATABASE_URL.password}",
        "-e",
        f"POSTGRES_DB={TEST_DATABASE_URL.database}",
        "-p",
        f"127.0.0.1:{TEST_DATABASE_URL.port or 5432}:5432",
        "postgres:16-alpine",
        timeout=180,
    )
    if result is not None and result.returncode == 0:
        print(
            f"\n[pytest] started disposable PostgreSQL container "
            f"'{TEST_CONTAINER_NAME}' on 127.0.0.1:{TEST_DATABASE_URL.port}"
        )
        return True
    if result is not None and result.stderr:
        print(f"\n[pytest] could not start test database: {result.stderr.strip()}")
    return False


def _can_connect(url: str, *, attempts: int, delay: float) -> bool:
    for attempt in range(attempts):
        probe = create_engine(
            url,
            poolclass=NullPool,
            connect_args={"connect_timeout": 2},
        )
        try:
            with probe.connect() as connection:
                connection.execute(text("SELECT 1"))
            return True
        except SQLAlchemyError:
            if attempt + 1 < attempts:
                time.sleep(delay)
        finally:
            probe.dispose()
    return False


def _unavailable_message() -> str:
    return (
        f"PostgreSQL test database is not available at "
        f"{TEST_DATABASE_URL.host}:{TEST_DATABASE_URL.port} "
        f"(database {TEST_DATABASE_URL.database!r}). "
        "Start it with: docker run --rm -d --name "
        f"{TEST_CONTAINER_NAME} -e POSTGRES_USER={TEST_DATABASE_URL.username} "
        f"-e POSTGRES_PASSWORD={TEST_DATABASE_URL.password} "
        f"-e POSTGRES_DB={TEST_DATABASE_URL.database} "
        f"-p 127.0.0.1:{TEST_DATABASE_URL.port}:5432 postgres:16-alpine"
    )


def _ensure_database() -> bool:
    if _can_connect(TEST_DATABASE_DSN, attempts=1, delay=0):
        return True

    autostart = _TEST_URL_IS_DEFAULT and os.environ.get("TEST_DB_AUTOSTART", "1") != "0"
    if not autostart or not _start_disposable_container():
        return False

    return _can_connect(TEST_DATABASE_DSN, attempts=30, delay=1.0)


def _alembic_config() -> Any:
    from alembic.config import Config

    # No ini file: migrations/env.py skips logging configuration when
    # config_file_name is None, which keeps pytest's logging intact.
    config = Config()
    config.set_main_option("script_location", str(SERVER_ROOT / "migrations"))
    config.set_main_option("sqlalchemy.url", TEST_DATABASE_DSN.replace("%", "%%"))
    return config


def _prepare_schema() -> None:
    from alembic import command

    # Always start from a pristine schema so stale tables from an older branch
    # can never make a broken migration look green.
    app_engine.dispose()
    with app_engine.connect() as connection:
        connection = connection.execution_options(isolation_level="AUTOCOMMIT")
        connection.execute(text("DROP SCHEMA IF EXISTS public CASCADE"))
        connection.execute(text("CREATE SCHEMA public"))
    command.upgrade(_alembic_config(), "head")
    app_engine.dispose()


def _truncate_all() -> None:
    # ``achievements`` is a reference catalog seeded by migration 0008 and kept
    # in sync at startup; it is not per-test data and must survive truncation.
    # ``ui_sound_events`` is the fixed UI sound registry seeded by migration
    # 0032 — the admin panel and the manifest both read it, so it is reference
    # data as well.
    preserved = {"achievements", "ui_sound_events"}
    table_names = ", ".join(
        f'"{table.name}"'
        for table in Base.metadata.sorted_tables
        if table.name not in preserved
    )
    if not table_names:
        return
    with app_engine.begin() as connection:
        connection.execute(
            text(f"TRUNCATE TABLE {table_names} RESTART IDENTITY CASCADE")
        )


@pytest.fixture(scope="session")
def db_engine():
    """Session-wide database: fresh schema from Alembic, cleaned per test."""
    if not _ensure_database():
        pytest.skip(_unavailable_message())
    _prepare_schema()
    yield app_engine


@pytest.fixture()
def clean_db(db_engine):
    """Run a test against empty tables."""
    _truncate_all()
    yield


@pytest.fixture(autouse=True)
def clear_email_outbox():
    """Keep captured verification/reset emails isolated per test."""
    from app.services.email_service import clear_test_outbox

    clear_test_outbox()
    yield
    clear_test_outbox()


@pytest.fixture()
def db_session(clean_db):
    session = SessionLocal()
    try:
        yield session
    finally:
        session.close()


@pytest.fixture(scope="session")
def client():
    with TestClient(app) as test_client:
        yield test_client


@pytest.fixture()
def db_client(client, clean_db):
    """HTTP client for tests that need the disposable database."""
    return client


@pytest.fixture(scope="session", autouse=True)
def song_storage():
    SONG_STORAGE.mkdir(parents=True, exist_ok=True)
    yield SONG_STORAGE
    shutil.rmtree(SONG_STORAGE, ignore_errors=True)


@pytest.fixture(scope="session", autouse=True)
def app_audio_storage():
    APP_AUDIO_STORAGE.mkdir(parents=True, exist_ok=True)
    yield APP_AUDIO_STORAGE
    shutil.rmtree(APP_AUDIO_STORAGE, ignore_errors=True)


@pytest.fixture(scope="session", autouse=True)
def game_builds_storage():
    GAME_BUILDS_STORAGE.mkdir(parents=True, exist_ok=True)
    yield GAME_BUILDS_STORAGE
    shutil.rmtree(GAME_BUILDS_STORAGE, ignore_errors=True)


@pytest.fixture(scope="session", autouse=True)
def gameplay_storage():
    """Gameplay visuals uploaded by the tests; removed when the session ends."""
    GAMEPLAY_STORAGE.mkdir(parents=True, exist_ok=True)
    yield GAMEPLAY_STORAGE
    shutil.rmtree(GAMEPLAY_STORAGE, ignore_errors=True)


@pytest.fixture(scope="session", autouse=True)
def campaign_storage():
    """Campaign covers uploaded by the tests; removed when the session ends."""
    CAMPAIGN_STORAGE.mkdir(parents=True, exist_ok=True)
    yield CAMPAIGN_STORAGE
    shutil.rmtree(CAMPAIGN_STORAGE, ignore_errors=True)


# ---------------------------------------------------------------------------
# Domain fixtures
# ---------------------------------------------------------------------------
@pytest.fixture()
def user_account(db_client) -> dict[str, Any]:
    return helpers.register_user(db_client)


@pytest.fixture()
def admin_user(db_client, db_session) -> dict[str, Any]:
    from app.models.user import User

    account = helpers.register_user(db_client, username=helpers.unique("admin"))
    db_session.execute(
        User.__table__.update()
        .where(User.id == uuid.UUID(account["id"]))
        .values(is_admin=True)
    )
    db_session.commit()
    return account


@pytest.fixture()
def second_user(db_client) -> dict[str, Any]:
    return helpers.register_user(db_client, username=helpers.unique("user"))


@pytest.fixture()
def third_user(db_client) -> dict[str, Any]:
    return helpers.register_user(db_client, username=helpers.unique("user"))


@pytest.fixture()
def draft_song(db_client, admin_user) -> dict[str, Any]:
    return helpers.create_song(db_client, admin_user["headers"])


@pytest.fixture()
def published_song(db_client, admin_user) -> dict[str, Any]:
    song = helpers.create_song(db_client, admin_user["headers"])
    updated = helpers.publish_song(db_client, admin_user["headers"], song["id"])
    return {**song, **updated}


@pytest.fixture()
def second_published_song(db_client, admin_user) -> dict[str, Any]:
    song = helpers.create_song(
        db_client, admin_user["headers"], title="Second Test Song"
    )
    updated = helpers.publish_song(db_client, admin_user["headers"], song["id"])
    return {**song, **updated}


@pytest.fixture()
def today_daily_slot(db_client, admin_user, published_song) -> dict[str, Any]:
    """Schedule ``published_song`` for today's UTC weekday (the daily song)."""
    from app.services.progression import utc_today

    return helpers.schedule_daily(
        db_client,
        admin_user["headers"],
        utc_today().weekday(),
        published_song["id"],
    )
