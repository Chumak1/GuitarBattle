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

## Public pages

- `GET /` — the landing page at `app/static/landing/index.html` (hero, project overview, how it works, features, requirements, CTA).
- `GET /login`, `GET /register` — sign-in, registration with email confirmation, and password reset at `app/static/app/auth.html`.
- `GET /account` — the signed-in player page at `app/static/app/account.html`: global rating, attempt history, username/display name and password changes.
- `GET /admin/` — the admin SPA at `app/static/admin/`.

All of them are served by FastAPI from inside the API image. Caddy only needs to reverse-proxy `fretfight.online` to `127.0.0.1:8000`; no separate static-site root is required.

Registration, email confirmation and password reset need SMTP. In development without `SMTP_*` the message (including the 6-digit code) is written to the `api` container log; with `APP_ENV=production` and no SMTP these endpoints return 503, so configure `SMTP_HOST`, `SMTP_PORT`, `SMTP_USERNAME`, `SMTP_PASSWORD`, `SMTP_SECURITY` in the production `.env`.

## Deploying to production

Production lives on the Yandex Cloud VM `guitarbattle-prod` (`ubuntu@84.201.151.183`), directory `/opt/guitarbattle/GuitarBattleServer`, behind Caddy. From the repository root:

```powershell
pwsh -File GuitarBattleServer/scripts/deploy-prod.ps1
```

The script packs the server directory (never `.env`), uploads it over SSH, keeps a timestamped `GuitarBattleServer.backup-*` copy of the previous release, syncs the files, rebuilds the `api` image, restarts the stack and waits for `https://fretfight.online/health/ready`. Use `-NoBuild` to sync files without restarting containers. The production `.env` stays on the server untouched; add new variables there manually before deploying code that needs them.


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
- `POST /auth/register` — `{ "username": "player_1", "email": "player@example.com", "password": "at-least-10-characters" }`; creates an unverified account and sends a 6-digit code.
- `POST /auth/login` — `{ "identifier": "player_1_or_email", "password": "..." }`; returns a Bearer access token only after the email is verified.
- `POST /auth/verify-email` — `{ "email": "...", "code": "123456" }`; confirms the address and returns an access token.
- `POST /auth/resend-verification` — `{ "email": "..." }`; always returns 202 and sends a new code when appropriate.
- `POST /auth/forgot-password` — `{ "email": "..." }`; always returns 202 and sends a reset code when appropriate.
- `POST /auth/reset-password` — `{ "email": "...", "code": "123456", "new_password": "..." }`; returns an access token.
- `GET /auth/me` — authenticated profile.
- `PATCH /auth/me` — authenticated partial profile update: `{ "display_name": "Player One" }` and/or `{ "username": "player_one" }` (3–32 characters `[A-Za-z0-9_.-]`, lowercased, 409 if taken).
- `POST /auth/change-password` — Bearer, `{ "current_password": "...", "new_password": "..." }`; returns 204.

Passwords are Argon2-hashed and access tokens are JWTs. Codes are stored only as HMAC-SHA256 hashes, expire after 15 minutes, are single-use, and allow at most five failed attempts. Registration is rejected with 503 if the verification email cannot be sent, so an unreachable account is not created. Changing or resetting a password increments the user's token version and invalidates all older JWTs. Existing accounts are marked verified by migration `0005` so they are not locked out when this feature is deployed.

### Email delivery (SMTP)

Set these variables in `.env` (see `.env.example`) and restart the `api` container:

```text
SMTP_HOST=smtp.example.com
SMTP_PORT=587
SMTP_USERNAME=noreply@fretfight.online
SMTP_PASSWORD=your-app-password
SMTP_SECURITY=starttls      # starttls | ssl | none
SMTP_FROM_EMAIL=noreply@fretfight.online
SMTP_FROM_NAME=GuitarBattle
```

`SMTP_SECURITY=ssl` is typically used with port 465; `starttls` with port 587. If `SMTP_HOST` is empty in development, the code is written to the `api` container log instead of being sent. Production does not fall back to logs: registration returns 503 until SMTP is configured. Never enable log fallback on a public deployment because the code is account-access material.

The API currently relies on this application-level resend cooldown and code attempt limit. Put a rate limiter (Caddy, nginx, or a dedicated service) in front of `/auth/*` before opening registration to a large public audience.

### Game sessions and scores
All endpoints below except leaderboard require `Authorization: Bearer <access_token>`:
- `POST /games/sessions` — start a session. Body: `{ "song_id": "<published-song-uuid>", "speed_percent": 100 }`.
- `POST /games/sessions/{session_id}/finish` — submit final score and mark the session finished. Body example:
  `{ "score_points": 1200, "accuracy_percent": 92.5, "max_combo": 14, "perfect_count": 8, "good_count": 4, "miss_count": 2 }`
- `GET /games/history` - current player's recent sessions.
- `GET /games/best-scores` - signed-in player's best score for every published song, plus the personal rating total (`rating_points` = sum of those best scores). Used by the client to show a best score next to each catalogue row.

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

The updated `Source/MainComponent.cpp` adds **LOAD ONLINE SONG**, **SIGN IN**, **CREATE ACCOUNT**, **VERIFY EMAIL**, and **FORGOT PASSWORD?** controls on the welcome/song-selection screens. Registration asks for username, email, and password, then opens a 6-digit confirmation dialog; the code can be resent from the same dialog. Password recovery uses a second 6-digit code and a new password, then signs the player in. The client requests the public catalog from `ApiClient::baseUrl` (`https://fretfight.online/api/v1` in production), downloads published MIDI and `drums.mp3` / `bass.mp3` / `guitar.mp3` / `others.mp3` stems into the current user's application-data cache, then passes the cached MIDI file through the existing `MidiParser`. Instrumental MP3 stems are loaded into the existing four-channel mixer; the microphone/pitch-detection path remains separate from backing-track playback. After sign-in, a player starting an online song gets an authenticated game session; the final score is submitted when the song finishes and the global ranking is refreshed. Network requests are made from UI/game-state code, never from the real-time audio callback. The access token remains in memory only and is lost when the application exits.

For another server address, change the `apiBase` and download URL base in `MainComponent.cpp`. The current integration selects the test song `pridumay-svetly-mir` when available, otherwise the first published catalog item. Existing local song selection and local best-score storage remain intact.

The JUCE source archive supplied for this update does not include the `.jucer`/Visual Studio project files. If Projucer does not automatically discover new source files, no new C++ translation units were added for the catalog feature; only existing `MainComponent.h/.cpp` were changed. Build the existing Visual Studio configuration after copying the updated `Source` directory.

## Database migrations

Migration `0004_competitions` is retained only as historical database migration metadata; competition endpoints are not registered in the API. Existing songs, users, MIDI files, and score/session tables are retained. Back up the database before deploying to any non-disposable environment.


### Глобальный рейтинг игроков

`GET /api/v1/leaderboard/global?limit=10&offset=0` — публичный рейтинг пользователей. Для каждой пары `user_id + song_id` учитывается только лучший результат; `rating_points` — сумма лучших результатов по опубликованным песням (повторные и более слабые попытки рейтинг не увеличивают); `games_played` — количество песен, давших вклад в рейтинг. На главном экране JUCE рейтинг открывается/обновляется кнопкой **REFRESH RATING**.
