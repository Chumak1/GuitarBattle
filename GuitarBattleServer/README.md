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
- `GET /admin/` — the admin SPA at `app/static/admin/`. It has five sections:
  songs, «Daily Challenge», landing CMS, game builds and «Статистика». The
  statistics tab (`app/static/admin/admin-statistics.js`, panel
  `#sectionStatistics`) shows app time, sessions and game launches per account
  plus game aggregates; it loads lazily on the first visit and refreshes on
  every later visit. Every number comes from the admin-only
  `/admin/statistics/*` API, and an expired token returns the SPA to its single
  login view (`window.FFAdmin.handleUnauthorized`). The Daily Challenge tab
  (`app/static/admin/admin-daily.js`, panel `#sectionDaily`) holds the weekly
  schedule: one song per weekday with «Назначить» / «Очистить». There is no
  separate `/statistics` page.

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
- `GET /songs?limit=20&offset=0` — published songs only. Songs that hold a Daily
  Challenge slot (`songs.daily_weekday IS NOT NULL`) are not listed here.
- `GET /songs/{slug}` — details and public file metadata. It still resolves a
  daily-challenge song: the client requests it without a token before playing
  the official attempt, and the assets must stay downloadable by slug.
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

The server sends verification and password-reset codes over SMTP. Yandex Cloud
Postbox is the reference production provider; the full setup, DNS records and
troubleshooting guide is in [`docs/YANDEX_POSTBOX.md`](docs/YANDEX_POSTBOX.md).
The `fretfight.online` address is verified in Postbox (Easy DKIM, status
`SUCCESS`), and real delivery was tested: SPF, DKIM and DMARC pass at
mail-tester.com. The registration -> email code -> verification flow also
passed. For production, set `APP_ENV=production` and use the production
secrets/HTTPS domain; see the remaining checklist in that document.

Set these variables in `.env` (see `.env.example`) and restart the `api` container:

```text
SMTP_HOST=postbox.cloud.yandex.net
SMTP_PORT=587
SMTP_USERNAME=<YANDEX_CLOUD_API_KEY_ID>
SMTP_PASSWORD=<YANDEX_CLOUD_API_KEY_SECRET>
SMTP_SECURITY=starttls
SMTP_FROM_EMAIL=noreply@fretfight.online
SMTP_FROM_NAME=GuitarBattle
```

`SMTP_USERNAME` is the ID of a Yandex Cloud API key with the `yc.postbox.send`
scope; `SMTP_PASSWORD` is its secret part. `SMTP_SECURITY=ssl` is typically used
with port 465; `starttls` with port 587. If `SMTP_HOST` is empty in development,
the code is written to the `api` container log instead of being sent. Production
does not fall back to logs: registration returns 503 until SMTP is configured.
Never enable log fallback on a public deployment because the code is
account-access material.

To verify connectivity and credentials without sending a message, run
`python scripts/check_smtp.py`; add `--send --to <mailbox>` for a real test
message. The script never prints `SMTP_PASSWORD`.

On a production server, check the effective configuration after every deploy:

```powershell
docker compose exec -T api python -c "from app.config import settings as s; print('app_env=', s.app_env); print('smtp_configured=', bool(s.smtp_host)); print('smtp_port=', s.smtp_port); print('smtp_security=', s.smtp_security); print('smtp_username_set=', bool(s.smtp_username)); print('smtp_password_set=', bool(s.smtp_password))"
docker compose exec -T api python scripts/check_smtp.py
```

Changing `SMTP_*` in `.env` requires recreating the container
(`docker compose up -d api`), not just `docker compose restart api`: a restart
keeps the environment captured when the container was created. The API logs
the effective email mode at startup without printing credentials. A `503` from
`/auth/register` means the verification email could not be handed to Postbox;
`docker compose logs api` contains the SMTP exception, and
`scripts/check_smtp.py` shows the SMTP/AUTH response.

The API currently relies on this application-level resend cooldown and code attempt limit. Put a rate limiter (Caddy, nginx, or a dedicated service) in front of `/auth/*` before opening registration to a large public audience.

### Game sessions and scores
All endpoints below except leaderboard require `Authorization: Bearer <access_token>`:
- `POST /games/sessions` — start a session. Body: `{ "song_id": "<published-song-uuid>", "speed_percent": 100, "daily": false }`. `daily: true` asks for the official daily-challenge attempt (see below) and creates a session with `mode: "daily"`.
- `POST /games/sessions/{session_id}/finish` — submit final score and mark the session finished. Body example:
  `{ "score_points": 1200, "accuracy_percent": 92.5, "max_combo": 14, "perfect_count": 8, "good_count": 4, "miss_count": 2 }`
  Optional progression fields: `completion_percent` (0–100, default 100), `avg_timing_error_ms` (0–2000), `notes_total`, `notes_played`. The response contains the score fields plus `xp_earned`, `xp_total`, `level`, `level_up`, `xp_to_next`, `xp_remaining`, `personal_best_updated`, `mastery` and `achievements_awarded`; a client that only reads the score fields keeps working.
- `GET /games/history` - current player's recent sessions.
- `GET /games/best-scores` - signed-in player's best score for every song outside the Daily Challenge schedule, plus the personal rating total (`rating_points` = sum of those best scores). Used by the client to show a best score next to each catalogue row; daily songs never feed the ordinary rating. Only `mode: "normal"` sessions count, and songs that were later unpublished or archived stay in the list — hiding a song never removes earned points.

Important: score submission is still client-reported. Progression clamps everything
it can (`completion_percent` never exceeds `notes_played / notes_total`, XP is capped
at 600 per attempt, achievements are derived from server-side state), but a modified
client can still report accuracy it did not play. Competitive/public deployment should
add replay or signed gameplay records.

### Progression, achievements and daily challenge

Migration `0008_progression` adds the whole progression model: `users.xp_total` /
`users.level`, progression columns on `scores` (`completion_percent`,
`avg_timing_error_ms`, `notes_total`, `notes_played`), `game_sessions.mode`
(`normal` / `daily`), `song_best_scores`, the `xp_awards` ledger, the
`achievements` catalog with `user_achievements`, and `daily_challenges` /
`daily_results`. Existing players start at level 1 with 0 XP and every historical
score counts as a 100 % completion, so no backfill script is needed.

Formulas (single implementation: `app/services/progression.py`; the tests in
`tests/test_progression.py` pin them):

| Value | Rule |
|---|---|
| Level cost | `XP_REQ(L) = round(50 * L ** 1.5)` for the step `L -> L+1`; `level_for_xp` is the largest `L` whose cumulative cost fits |
| Attempt XP | `round(100 * score * difficulty * tempo * content * repeat)`, clamped to `[3, 600]`; `0` when completion < 50 % |
| `score` | accuracy / 100 — the maximum score of a song is not known server-side, so accuracy is the only fair normalisation and `score_points` is never used for XP |
| `difficulty` | 1→0.8, 2→1.0, 3→1.2, 4→1.35, 5→1.5, >5→`min(1.8, 1.5 + 0.05*(d-5))`, <1→0.6 |
| `tempo` | `clamp(0.90, 1.15, 0.90 + (bpm - 60) / 120 * 0.25)` — at most +15 % |
| `content` | `clamp(0.6, 1.4, sqrt(duration_seconds / 90)) * completion / 100` |
| `repeat` | N-th **normal** attempt of the same song today (UTC): 1.0, 0.5, 0.25, 0.0 — the 4th and later attempts give no XP |
| Mastery | `clamp(0, 100, best_accuracy + (difficulty - 1) * 2)`; `<60` Novice, `<75` Learner, `<87` Guitarist, `<95` Pro, `>=95` Master |
| Genre mastery | mean mastery of the player's personal bests in that genre |

Achievement XP is a fixed bonus: it is added to `users.xp_total` and never
multiplied by `repeat`. The catalog (27 rows) lives in
`app/services/achievement_catalog.py`, is seeded by the migration and re-synced on
startup, so code and database cannot drift apart.

Endpoints (all require Bearer):

- `GET /profile/progression` — level, XP, mastered songs, unlocked/total
  achievements, last 10 personal bests, mastery per song, mastery per genre and
  the statistics block.
- `GET /profile/personal-bests?limit=10&offset=0` — personal bests of every played song; hidden (unpublished or archived) songs stay in the list so their mastery and rating are never lost.
- `GET /achievements` — the catalog with `progress`, `unlocked`, `awarded_at`
  and the icon pair `icon` (file name) / `icon_url`
  (`/media/achievements/<icon>`).
- `GET /profile/users/{id}` — public player card; each awarded achievement also
  carries `icon` and `icon_url`.
- `GET /daily/today` — today's challenge: `date`, `weekday` (0 = Monday …
  6 = Sunday, UTC), the scheduled `song` or `null` when the slot is free or the
  scheduled song is not published, whether the official attempt is used, the
  official result and the top 5.
- `GET /daily/leaderboard?date=YYYY-MM-DD` — official results of a day.
- `GET /daily/history?limit=20&offset=0` — the player's official daily attempts,
  newest day first: `date`, `song_id`, `song_slug`, `song_title`, `artist`,
  `score_points`, `accuracy_percent`, `avg_timing_error_ms`, `rank` (place among
  all results of that day) and `participants`.

Statistics block (last 100 finished attempts with `completion_percent > 25`):
`accuracy` (mean accuracy), `timing` (`100 - clamp(0, 100, avg_timing_error_ms / 1.5)`,
`null` without timing data), `speed` (`clamp(0, 100, (avg_speed - 50) / 70 * 100)`),
`consistency` (`100 - min(100, stddev(accuracy of the last 20 attempts))`, `0` with
fewer than 2 samples), `versatility`
(`100 * (0.6 * distinct_songs / published_songs + 0.4 * distinct_genres / published_genres)`),
`samples`.

Daily challenge rules (migration `0009_daily_slots`):

- The schedule is explicit: `daily_slots` holds one song per weekday (`0` =
  Monday … `6` = Sunday, UTC) and `songs.daily_weekday` mirrors it. The public
  catalogue, the global rating and `/games/best-scores` ignore songs with a
  slot; a daily song is still playable as a practice session
  (`mode: "normal"`), and its files stay downloadable by slug.
- The song of a date is resolved from that weekday's slot on the first request
  for the date and frozen in `daily_challenges`, so changing or clearing a slot
  never rewrites a day that already started. A slot pointing at a draft behaves
  like a free slot until the song is published.
- `POST /games/sessions` with `daily: true` requires the frozen song of today and
  an unused official attempt, otherwise `409`. Practice sessions on the same
  song stay `mode: "normal"` and never enter the daily ranking.
- `finish` of a daily session inserts `daily_results` (unique `(date, user_id)`);
  a second official attempt returns `409`. Timing, score and accuracy are stored
  for the daily ranking, which sorts by score desc, then accuracy desc.

Admin schedule (`is_admin` required):

- `GET /admin/daily` — seven slots: `{weekday, weekday_name, song: {id, slug,
  title, artist, genre, bpm, difficulty, duration_seconds, is_published} | null,
  updated_at}`.
- `PUT /admin/daily/{weekday}` — body `{"song_id": "<uuid>"}` assigns,
  `{"song_id": null}` clears. Weekday outside 0..6 → `422`, unknown song → `404`.
- Conflict policy: the assignment **moves** the song. If it already occupied
  another weekday, that slot is cleared; if the target weekday held a different
  song, that song loses its slot and returns to the ordinary catalogue. No
  `409` for that case, so replacing a week does not require manual cleanup.
- `daily_weekday` is part of the admin song payloads
  (`POST/PATCH /admin/songs`, list and detail responses) and is synced with
  `daily_slots` in the same transaction: setting it schedules the song, sending
  `null` removes it from the schedule.

Known limitations:

- No background scheduler: `daily_first_place` ("Daily Champion") is granted on the
  next `/profile/progression`, `/achievements` or `/daily/leaderboard` call after
  the day has ended, not at midnight.
- The genre achievements adapt to the real catalog: the effective threshold is
  `min(goal, published genres)` and when two achievements collapse onto the same
  threshold only the first one stays attainable (the rest remain locked and their
  description says how many genres exist). With the current two-genre catalog
  `genres_3` is reachable, `genres_5` / `genres_8` are not.
- `xp_to_next` and `xp_remaining` are the same number (XP left before the next
  level); both names are returned for client convenience.
- `song_best_scores` and the global leaderboard are independent: the leaderboard
  keeps its own `max(score_points)` aggregation, so its rules did not change.

### Scope note

Competition rooms and lobby endpoints are not enabled in the API router; the current implementation is limited to accounts, online song catalog, game results, history, leaderboards and progression. The historical database migration is retained so existing databases can still upgrade without rewriting migration history.

### Activity telemetry and admin statistics

The desktop client reports app activity; the server owns all time arithmetic and
never trusts a client-reported duration:

- `POST /activity/heartbeat` — Bearer. Body:
  `{ "client_session_id": "<per-process uuid, <=64 chars>", "app_version": "1.0.0", "platform": "windows", "final": false }`.
  The first call creates `app_sessions` (`started_at = last_seen_at = now`), later
  calls only move `last_seen_at`; `final: true` sets `ended_at`. The pair
  `(user_id, client_session_id)` is unique, so retries cannot duplicate a session.
  Response: `{ "session_id": "<uuid>", "started_at": "...", "last_seen_at": "..." }`.
- `POST /activity/game-launch` — Bearer. Body:
  `{ "client_session_id": "...", "game_key": "song", "song_id": "<song uuid>", "mode": "single" }`
  (`game_key` examples: `song`, `daily_challenge`, `practice`, `local:<slug>`).
  Returns `201 {"id": "<uuid>"}`. An unknown `song_id` or a missing app session is
  stored as `NULL` instead of failing the request — telemetry must never break the game.

Administrator-only aggregates (`Bearer` with `is_admin`):

- `GET /admin/statistics/overview?days=30` — `users_total`, `users_active_24h`,
  `users_active_7d`, `sessions_total`, `app_seconds_total`, `avg_session_seconds`,
  `launches_total`, `launches_24h`, `generated_at`. `days` (1–365) limits sessions
  and launches; `users_total` always counts all accounts. Activity is a heartbeat
  or a launch inside the window.
- `GET /admin/statistics/users?search=&sort=last_seen&order=desc&limit=50&offset=0`
  — one row per account: `user_id`, `username`, `display_name`, `email`,
  `is_active`, `created_at`, `app_seconds_total`, `sessions_count`,
  `last_seen_at`, `launches_count`, `last_launch_at`, `games_distinct`,
  `scores_count`, `best_score`, plus `total`/`limit`/`offset`.
  `search` is a case-insensitive substring of username, display name or email.
  `sort` is one of `last_seen`, `username`, `display_name`, `email`, `created_at`,
  `app_seconds`, `sessions_count`, `launches_count`, `scores_count`;
  `order` is `asc` or `desc` and defaults to `asc` for the text columns and
  `desc` for numbers and dates. Text sorts are case-insensitive, empty values go
  last in both directions, and `username`/`id` break ties so paging is stable.
  `scores_count`/`best_score` come from the `scores` rows reached through
  `game_sessions` (`best_score` is `null` when the player has no results).
- `DELETE /admin/users/{user_id}` — hard delete of one account (204). Answers
  400 `"You cannot delete your own account"` for the signed-in administrator,
  404 for an unknown id and 409 if related data ever blocks the delete. Every
  foreign key to `users.id` is `ON DELETE CASCADE`, so game sessions and their
  scores, app sessions, game launches, email codes and competitions are removed
  with the account. The audit log line contains ids only.
- `GET /admin/statistics/games?days=30&limit=100` — launches grouped by
  `game_key` with `title`, `launches`, `unique_users`, `last_launch_at`. The title
  is the song title of the newest launch that has a `song_id`; keys without a song
  keep the key itself.

A session's duration is `coalesce(ended_at, last_seen_at) - started_at`, clamped
at zero, so a crashed client still produces usable numbers. All aggregates are
computed in SQL (grouped subqueries joined once per request), never per row.

The admin UI for these numbers is the «Статистика» tab of `/admin/`
(`app/static/admin/admin-statistics.js`), not a standalone page. Table headers
sort server-side (click toggles asc/desc, ▲/▼ marks the active column), the
Scores column shows the number of results with the personal best underneath,
and each row has a delete button (confirmation shows the login and email; the
signed-in administrator's own row is disabled).

### Administrator song workflow
Every `/admin/songs` route requires a Bearer token for a user whose `is_admin` flag is true:
- `GET /admin/songs`
- `POST /admin/songs`
- `GET /admin/songs/{song_id}`
- `PATCH /admin/songs/{song_id}`
- `POST /admin/songs/{song_id}/publish`
- `POST /admin/songs/{song_id}/unpublish`
- `POST /admin/songs/{song_id}/archive` — hide the song, keep every result
- `POST /admin/songs/{song_id}/restore` — bring an archived song back as a draft
- `DELETE /admin/songs/{song_id}` — archives by default; `?permanent=true` physically deletes only a song without user results
- MIDI upload, download and analysis endpoints under `/admin/songs/{song_id}/files/...`
- `POST /admin/songs/{song_id}/files/audio/{role}` — upload an instrumental MP3. Supported roles: `drums`, `bass`, `guitar`, `others`; maximum size is 100 MiB.
- `POST /admin/songs/{song_id}/files/{file_id}/activate` — publish any stored version of a role again. The other versions of that role become an archive in the same transaction and `songs.version` is bumped.

Grant the first administrator locally after registering an account (substitute the email and use the DB name/user from your own `.env`):

```powershell
docker compose exec db psql -U <POSTGRES_USER> -d <POSTGRES_DB> -c "UPDATE users SET is_admin = TRUE WHERE lower(email) = lower('admin@example.com');"
```

Log in again after the role change, then use the new access token in Swagger's Authorize dialog.

#### Song lifecycle: publish, archive, delete

`songs.archived_at` separates *availability* from *result retention* (migration
`0012_song_archive`):

| State | Public `GET /songs` | `GET /songs/{slug}` / downloads | New sessions | Results, PB, mastery, rating |
|---|---|---|---|---|
| Draft (`is_published = false`) | no | 404 | 404 | kept |
| Published | yes | yes | yes | kept |
| Archived (`archived_at IS NOT NULL`) | no | 404 | 404 | **kept** |

- `DELETE /admin/songs/{id}` **archives** the song: `archived_at` is set,
  `is_published` becomes `false` and the Daily Challenge slot is cleared. The row
  keeps its stable `id`, so `song_best_scores`, `game_sessions`/`scores`,
  `xp_awards`, `user_achievements`, `daily_challenges`/`daily_results` and the
  activity history stay attached and readable. The web admin shows the “В архиве”
  badge, the archive filter and a restore action.
- Archiving is idempotent and can be repeated; it never rewrites aggregates, so
  rating points cannot be lost or double-counted.
- `DELETE /admin/songs/{id}?permanent=true` physically removes the row, but only
  when no `game_sessions`, `song_best_scores` or `daily_challenges` reference it
  (`409` with the counts otherwise). The database enforces the same rule:
  `song_best_scores.song_id`, `daily_slots.song_id`, `game_sessions.song_id` and
  `daily_challenges.song_id` are `ON DELETE RESTRICT`; `game_launches.song_id`
  stays `ON DELETE SET NULL` so launch statistics survive with `song_id = null`.
  Deleting a never-played song still leaves its uploaded files on disk — remove
  them manually if the song must disappear completely.
- `POST /admin/songs/{id}/archive` and `POST /admin/songs/{id}/restore` are the
  explicit routes the admin UI uses; `DELETE` delegates to archiving.

To add a track, open `http://localhost:8000/docs`, authorize as admin, choose `POST /api/v1/admin/songs/{song_id}/files/audio/{role}`, enter the song UUID and one role (`drums`, `bass`, `guitar`, or `others`), then attach an `.mp3` file. The MIDI remains uploaded through `POST /api/v1/admin/songs/{song_id}/files/midi`. Upload aligned stems exported from the same timeline/start point so the four mixer channels remain synchronized.

Every upload is kept as a version and exactly one file per `(song, role)` is active
(`song_files.is_active`); the public API serves only active files. A new upload
becomes active automatically, and the admin file dialog shows the badge
“Активен” / “Архив” with a “Сделать активной” button for older versions, so a bad
upload can be rolled back without re-uploading the file. `GET /admin/songs/{id}`
returns every version in `files[]` with `is_active`; `GET /admin/songs/{id}/files`
returns the same list.

## Landing page CMS, game builds and GeoIP

Three extra directories are mounted into the API container:

| Directory | Volume | Used for |
|---|---|---|
| `/data/landing_media` | `landing_media_data` | Hero image/video uploaded from the admin panel, served at `/media/landing/<name>`. Max 50 MiB (`MAX_LANDING_MEDIA_BYTES`). |
| `/data/game_builds` | `game_builds_data` | Desktop builds (`.exe`, `.zip`, `.msi`), one active build served at `/api/v1/downloads/game`. Max 500 MiB (`MAX_GAME_BUILD_BYTES`). |
| `/data/geoip` | `geoip_data` | DB-IP Lite country database for `GET /api/v1/geo`. |

Both upload directories are backed by named volumes, so `docker compose up --build` keeps the files; do not replace the volumes. The landing page reads its content from `GET /api/v1/landing` (blocks, hero background and the active build), so text and the download link change without a redeploy.

Administrator endpoints (`Bearer` token with `is_admin`, same as `/admin/songs`):

- `GET /api/v1/admin/site/landing`, `PUT /api/v1/admin/site/landing` — read/replace the CMS blocks (max 256 KiB serialized, 5000 characters per string, nesting depth 8).
- `POST /api/v1/admin/site/landing/background` (multipart `file`), `DELETE /api/v1/admin/site/landing/background` — hero JPEG/PNG/WebP/GIF/MP4/WebM.
- `GET /api/v1/admin/site/game-builds`, `POST /api/v1/admin/site/game-builds` (multipart `file` + optional `version`), `POST /api/v1/admin/site/game-builds/{id}/activate`, `DELETE /api/v1/admin/site/game-builds/{id}`.

`GET /api/v1/geo` returns `{"country": "DE", "language": "en"}` for a public client IP (CIS countries get `ru`). Without the database it returns `null` values and the page falls back to its default language. Download or refresh the database with:

```powershell
# Download straight to the default GEOIP_DB_PATH (works inside the container).
python scripts/fetch_geoip.py

# Or download on the host and copy the file into the geoip_data volume.
python scripts/fetch_geoip.py --out .\geoip\dbip-country-lite.mmdb
docker compose cp .\geoip\dbip-country-lite.mmdb api:/data/geoip/dbip-country-lite.mmdb
```

The script fetches `dbip-country-lite-{YYYY-MM}.mmdb.gz` for the current month, falls back to the previous month when it is not published yet, decompresses it and replaces the file atomically. It prints only the destination path.

## JUCE client

The updated `Source/MainComponent.cpp` adds **LOAD ONLINE SONG**, **SIGN IN**, **CREATE ACCOUNT**, **VERIFY EMAIL**, and **FORGOT PASSWORD?** controls on the welcome/song-selection screens. Registration asks for username, email, and password, then opens a 6-digit confirmation dialog; the code can be resent from the same dialog. Password recovery uses a second 6-digit code and a new password, then signs the player in. The client requests the public catalog from `ApiClient::baseUrl` (`https://fretfight.online/api/v1` in production), downloads published MIDI and `drums.mp3` / `bass.mp3` / `guitar.mp3` / `others.mp3` stems into the current user's application-data cache, then passes the cached MIDI file through the existing `MidiParser`. Instrumental MP3 stems are loaded into the existing four-channel mixer; the microphone/pitch-detection path remains separate from backing-track playback. After sign-in, a player starting an online song gets an authenticated game session; the final score is submitted when the song finishes and the global ranking is refreshed. Network requests are made from UI/game-state code, never from the real-time audio callback. The access token remains in memory only and is lost when the application exits.

For another server address, change the `apiBase` and download URL base in `MainComponent.cpp`. The current integration selects the test song `pridumay-svetly-mir` when available, otherwise the first published catalog item. Existing local song selection and local best-score storage remain intact.

The JUCE source archive supplied for this update does not include the `.jucer`/Visual Studio project files. If Projucer does not automatically discover new source files, no new C++ translation units were added for the catalog feature; only existing `MainComponent.h/.cpp` were changed. Build the existing Visual Studio configuration after copying the updated `Source` directory.

## Database migrations

Migration `0004_competitions` is retained only as historical database migration metadata; competition endpoints are not registered in the API. Existing songs, users, MIDI files, and score/session tables are retained. Back up the database before deploying to any non-disposable environment.

Migration `0007_app_statistics` adds `app_sessions` and `game_launches` (unique
`(user_id, client_session_id)`, `ON DELETE CASCADE` from `users`,
`ON DELETE SET NULL` for `app_session_id`/`song_id`). Both tables are written only
by the telemetry endpoints and are safe to truncate if the history is not needed.

Migration `0008_progression` adds the progression tables and seeds the
`achievements` catalog. It is reversible (`alembic downgrade 0007_app_statistics`);
downgrading drops per-player XP, personal bests, achievements and daily results,
while `scores` / `game_sessions` keep their historical rows minus the new columns.

Migration `0009_daily_slots` adds the weekly Daily Challenge schedule:
`songs.daily_weekday` (smallint `NULL` or 0..6 with a check constraint and an
index) and the `daily_slots` table (`weekday` primary key with a 0..6 check,
`song_id` `ON DELETE CASCADE`, `updated_at`). It creates no rows: the schedule
starts empty and existing `daily_challenges` / `daily_results` rows are kept as
historical facts. Downgrade
(`alembic downgrade 0008_progression`) drops the table, the index and the column.

Migration `0010_achievement_icons` adds `achievements.icon` (`String(128)`,
`NOT NULL`): the file name of a bundled SVG. The 14 icons live in
`app/static/media/achievements/` and are served by the API at
`/media/achievements/<icon>` (public, no token, `image/svg+xml`, baked into the
image — no volume). The migration backfills all 27 catalog rows; the application
re-syncs the same values on every start. Icons are free Tilda Icons, attribution
“Icons are provided by Tilda Publishing” (https://tilda.cc) is shown in the
account page footer. Downgrade drops the column.

Migration `0011_asset_versions` adds `song_files.is_active` (`BOOLEAN NOT NULL
DEFAULT TRUE`) and the index `(song_id, role, is_active)`. The same migration
repairs the 27.10 19:26–19:31 bad uploads: every file created at or after
`2026-10-27 19:00+00` is archived, the earliest remaining file of each role
becomes the active version again, and `songs.version` is bumped for every song
whose published asset set changed (the client caches downloads by that number).
Downgrade drops the index and the column; archived versions are not restored.


### Глобальный рейтинг игроков

`GET /api/v1/leaderboard/global?limit=10&offset=0` — публичный рейтинг пользователей. Для каждой пары `user_id + song_id` учитывается только лучший результат; `rating_points` — сумма лучших результатов по песням, у которых не задан `daily_weekday` (повторные и более слабые попытки рейтинг не увеличивают, официальные daily-попытки в обычный рейтинг не попадают — учитываются только сессии с `mode = "normal"`); `games_played` — количество песен, давших вклад в рейтинг. Скрытые песни (снятые с публикации или архивированные) продолжают учитываться: скрытие не отбирает уже заработанные очки, а поскольку на пару `user_id + song_id` всегда приходится ровно одна строка лучшего результата, скрытие/восстановление не может начислить очки дважды. На главном экране JUCE рейтинг открывается/обновляется кнопкой **REFRESH RATING**.
