# API tests

Pytest suite for the FastAPI server. The tests use a **disposable** PostgreSQL
database and never touch the development or production database.

## Install

```powershell
cd D:\GuitarBattle\GuitarBattleServer
uv venv .venv-test
uv pip install --python .venv-test\Scripts\python.exe -r requirements-dev.txt
```

## Run

```powershell
.venv-test\Scripts\python -m pytest -q
```

The suite expects PostgreSQL at `127.0.0.1:55432` with database
`guitarbattle_test` (see `TEST_DATABASE_URL` below). By default it starts a
throwaway container automatically if Docker is available:

```powershell
docker run --rm -d --name guitarbattle-test-db `
  -e POSTGRES_USER=guitarbattle_test `
  -e POSTGRES_PASSWORD=guitarbattle_test `
  -e POSTGRES_DB=guitarbattle_test `
  -p 127.0.0.1:55432:5432 postgres:16-alpine
```

If the database is unavailable, database-backed tests are skipped with a clear
message; pure schema and MIDI-parser tests still run. When finished:

```powershell
docker rm -f guitarbattle-test-db
```

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `TEST_DATABASE_URL` | `postgresql+psycopg://guitarbattle_test:guitarbattle_test@127.0.0.1:55432/guitarbattle_test` | Test database. The database name must contain `test` or the run aborts. |
| `TEST_DB_AUTOSTART` | `1` | Set to `0` to disable starting the disposable container automatically. |
| `TEST_DB_CONTAINER` | `guitarbattle-test-db` | Container name used for start/reuse. |

The schema is rebuilt from the real Alembic migrations once per session
(`DROP SCHEMA public CASCADE` + `alembic upgrade head`), and every test starts
with empty tables. Song uploads are written to a temporary directory that is
removed after the session.

## Coverage

- `test_health.py` — `/health/live`, `/health/ready`, service metadata.
- `test_auth.py` — register/duplicate/login/me/profile update, inactive users.
- `test_email_service.py` — SMTP STARTTLS/SMTPS, auth, message headers,
  delivery errors and development/production/test behavior (no network).
- `test_contract_validation.py` — request schema bounds shared with the client.
- `test_songs.py` — public catalog, admin draft/publish/unpublish, MIDI upload,
  analysis and public download.
- `test_games.py` — sessions, score submission, history, leaderboards.
- `test_midi_parser.py` — generated MIDI files and malformed input.
- `test_achievement_catalog.py` — achievement display texts are English/ASCII,
  codes, `goal`, `xp_bonus` and `category` stay pinned.

Tests marked `db` require the disposable database. Run only unit tests with:

```powershell
.venv-test\Scripts\python -m pytest -q -m "not db"
```
