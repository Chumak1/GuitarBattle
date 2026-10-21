# GuitarBattle Server — catalog, accounts, scores and leaderboard

FastAPI + PostgreSQL + SQLAlchemy + Alembic + Docker Compose.

## Start / update locally

1. Keep your existing `.env` and PostgreSQL Docker volume.
2. Copy updated source files into the existing project.
3. From the server project directory run:

   ```powershell
   docker compose up --build
   ```

   The entrypoint runs `alembic upgrade head` before starting the API.
4. Check `http://localhost:8000/health/ready` and `http://localhost:8000/docs` (docs are disabled when `APP_ENV=production`).

**Do not run `docker compose down -v`** during an update; that deletes the database and stored song files. Do not put real secrets in source control. If a database password or JWT secret has ever been pasted into a chat/log, rotate it and update `.env`.

## API overview

All paths below use `/api/v1`.

### Public song catalog
- `GET /songs?limit=20&offset=0` — published songs only.
- `GET /songs/{slug}` — details and public file metadata.
- `GET /songs/{slug}/files/{file_id}/download` — download MIDI or an instrumental MP3 stem for a published song.
- `GET /leaderboard?limit=20&offset=0` — global top scores.
- `GET /leaderboard?song_slug=pridumay-svetly-mir` — scores for one song.

The API returns JSON in UTF-8. PowerShell 5.1 can misdecode response bodies when using `Invoke-RestMethod` on some responses; to inspect Russian text, save response bytes with `curl.exe -o` and read using `[System.IO.File]::ReadAllText(path, [System.Text.Encoding]::UTF8)`.

### Accounts
- `POST /auth/register` — `{ "username": "player_1", "email": "player@example.com", "password": "at-least-10-characters" }`
- `POST /auth/login` — `{ "identifier": "player_1", "password": "..." }`; returns a Bearer access token.
- `GET /auth/me` — authenticated profile.
- `PATCH /auth/me` — authenticated display name update, `{ "display_name": "Player One" }`.

Passwords are Argon2-hashed and access tokens are JWTs. Registration creates a non-admin account. Email verification, password reset, login throttling, and external email delivery are not implemented. Do not expose this development deployment directly to the public internet without adding those controls, HTTPS, rate limiting, backups, and monitoring.

### Game sessions and scores
All endpoints below except leaderboard require `Authorization: Bearer <access_token>`:
- `POST /games/sessions` — start a session. Body: `{ "song_id": "<published-song-uuid>", "speed_percent": 100 }`.
- `POST /games/sessions/{session_id}/finish` — submit final score and mark the session finished. Body example:
  `{ "score_points": 1200, "accuracy_percent": 92.5, "max_combo": 14, "perfect_count": 8, "good_count": 4, "miss_count": 2 }`
- `GET /games/history` — current player's recent sessions.

Important: score submission is currently client-reported. This is suitable for local development and basic leaderboards, but scores are not cheat-resistant; competitive/public deployment should add server-side validation, replay/event validation or signed gameplay records.

### Scope note

Competition rooms and lobby endpoints are not enabled in the API router; the current implementation is limited to accounts, online song catalog, game results, history, and leaderboards. The historical database migration is retained so existing databases can still upgrade without rewriting migration history.

### Administrator song workflow
Every `/admin/songs` route requires a Bearer token for a user whose `is_admin` flag is true:
- `GET /admin/songs`
- `POST /admin/songs`
- `GET /admin/songs/{song_id}`
- `PATCH /admin/songs/{song_id}`
- `POST /admin/songs/{song_id}/publish`
- `POST /admin/songs/{song_id}/unpublish`
- `DELETE /admin/songs/{song_id}`
- MIDI upload, download and analysis endpoints under `/admin/songs/{song_id}/files/...`
- `POST /admin/songs/{song_id}/files/audio/{role}` — upload an instrumental MP3. Supported roles: `drums`, `bass`, `guitar`, `others`; maximum size is 100 MiB.

Grant the first administrator locally after registering an account (substitute the email and use the DB name/user from your own `.env`):

```powershell
docker compose exec db psql -U <POSTGRES_USER> -d <POSTGRES_DB> -c "UPDATE users SET is_admin = TRUE WHERE lower(email) = lower('admin@example.com');"
```

Log in again after the role change, then use the new access token in Swagger's Authorize dialog.

To add a track, open `http://localhost:8000/docs`, authorize as admin, choose `POST /api/v1/admin/songs/{song_id}/files/audio/{role}`, enter the song UUID and one role (`drums`, `bass`, `guitar`, or `others`), then attach an `.mp3` file. The MIDI remains uploaded through `POST /api/v1/admin/songs/{song_id}/files/midi`. Upload aligned stems exported from the same timeline/start point so the four mixer channels remain synchronized.

## JUCE client

The updated `Source/MainComponent.cpp` adds **LOAD ONLINE SONG**, **LOGIN**, and **REGISTER** controls on the song selection screen. It requests the public catalog from `http://localhost:8000/api/v1`, downloads a published MIDI file and any available `drums.mp3`, `bass.mp3`, `guitar.mp3`, and `others.mp3` stems into the current user's application-data cache, then passes the cached MIDI file through the existing `MidiParser`. Instrumental MP3 stems are loaded into the existing four-channel mixer; the microphone/pitch-detection path remains separate from backing-track playback. After login/registration, a signed-in player starting an online song gets an authenticated game session; the final score is submitted when the song finishes and the global ranking is refreshed. Network requests are made from UI/game-state code, never from the real-time audio callback. The access token remains in memory only and is lost when the application exits.

For another server address, change the `apiBase` and download URL base in `MainComponent.cpp`. The current integration selects the test song `pridumay-svetly-mir` when available, otherwise the first published catalog item. Existing local song selection and local best-score storage remain intact.

The JUCE source archive supplied for this update does not include the `.jucer`/Visual Studio project files. If Projucer does not automatically discover new source files, no new C++ translation units were added for the catalog feature; only existing `MainComponent.h/.cpp` were changed. Build the existing Visual Studio configuration after copying the updated `Source` directory.

## Database migrations

Migration `0004_competitions` is retained only as historical database migration metadata; competition endpoints are not registered in the API. Existing songs, users, MIDI files, and score/session tables are retained. Back up the database before deploying to any non-disposable environment.


### Глобальный рейтинг игроков

`GET /api/v1/leaderboard/global?limit=10&offset=0` — публичный рейтинг пользователей. `rating_points` — сумма очков за завершённые игры на опубликованных песнях; `games_played` — количество учтённых результатов. На главном экране JUCE рейтинг открывается/обновляется кнопкой **REFRESH RATING**.
