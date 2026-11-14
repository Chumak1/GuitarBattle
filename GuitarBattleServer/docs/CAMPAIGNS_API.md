# Campaigns API ("Tower riffs")

A campaign is an administrator-authored run: a name, a **default** bot
difficulty, and an ordered list of songs. Every stage may override the default
with its own bot difficulty. The number of stages equals the number of songs in
the list, and there is no limit on the number of campaigns.

Migrations: **`0026_campaigns`**, **`0028_campaign_song_bot_level`**,
**`0029_character_unlocks`** (reward character, per-user progress and character
collection) and **`0034_campaign_cover`** (cover image URL and description).

## Data model

| Table | Columns |
|---|---|
| `campaigns` | `id UUID PK`, `name varchar(120) NOT NULL UNIQUE`, `bot_level smallint NOT NULL CHECK 0..2`, `reward_character_id UUID NULL FK ui_characters(id) ON DELETE SET NULL`, `reward_condition varchar(200) NULL`, `cover_image_url varchar(500) NULL`, `description varchar(400) NULL`, `created_at`, `updated_at` |
| `campaign_songs` | `id UUID PK`, `campaign_id UUID FK campaigns(id) ON DELETE CASCADE`, `song_id UUID FK songs(id) ON DELETE CASCADE`, `position int NOT NULL CHECK >= 0`, `bot_level smallint NULL CHECK 0..2`, `UNIQUE(campaign_id, song_id)`, `UNIQUE(campaign_id, position)`, indexes on `campaign_id` and `song_id` |
| `user_characters` | `user_id UUID FK users(id) ON DELETE CASCADE`, `character_id UUID FK ui_characters(id) ON DELETE CASCADE`, `unlocked_at timestamptz NOT NULL`, `source_campaign_id UUID NULL FK campaigns(id) ON DELETE SET NULL`, PK `(user_id, character_id)` |
| `user_campaign_progress` | `user_id UUID FK users(id) ON DELETE CASCADE`, `campaign_id UUID FK campaigns(id) ON DELETE CASCADE`, `best_stage int NOT NULL DEFAULT 0 CHECK >= 0`, `completed_at timestamptz NULL`, `updated_at timestamptz NOT NULL`, PK `(user_id, campaign_id)` |

`bot_level`: `0` = EASY, `1` = NORMAL, `2` = HARD. The admin UI shows those
labels; the desktop client receives the number.

`campaigns.bot_level` is the **default** used for newly added stages and the
fallback when `campaign_songs.bot_level` is `NULL`. Migration `0028` backfilled
every existing stage with its campaign's level, so towers created before the
migration kept exactly the behaviour they had.

`position` is 0-based. The order of the request list becomes `position` 0, 1, 2…

## Campaign JSON

Both the public and the admin endpoints return the same object:

```json
{
  "id": "3f0c2b42-6a1e-4d3e-9b9c-0f1a2b3c4d5e",
  "name": "Tower riffs",
  "bot_level": 1,
  "song_count": 2,
  "songs": [
    {
      "song_id": "8e7d6c5b-4a39-4827-9615-0413f2e1d0c9",
      "slug": "first-song",
      "title": "First Song",
      "difficulty": 3,
      "is_published": true,
      "position": 0,
      "bot_level": 0
    },
    {
      "song_id": "1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d",
      "slug": "second-song",
      "title": "Second Song",
      "difficulty": 4,
      "is_published": true,
      "position": 1,
      "bot_level": 2
    }
  ],
  "reward_character_id": 12,
  "reward_character_name": "Jackson",
  "reward_condition": "Complete Jackson Tower",
  "cover_image_url": "/media/campaigns/cover-4a1b2c3d-....png",
  "description": "Three stages of riffs, from warm-up to the Jackson solo.",
  "created_at": "2026-11-09T10:00:00Z",
  "updated_at": "2026-11-09T10:05:00Z"
}
```

List responses wrap the items: `{ "items": [ ... ] }`.

Notes:

- `song_count` is `len(songs)`. For the **public** endpoint that counts only
  published stages; for the admin endpoint it counts every configured stage.
- `songs` is always sorted by `position` (ascending, 0-based).
- `songs[].bot_level` is the **effective** level: the stored per-stage value,
  or `campaigns.bot_level` when the stage has none. Always `0..2`.
- An explicit `position` is not sent by the client; it is assigned by the
  server from the request order.
- `reward_character_id` is the **public integer** character id on the public
  endpoint and the internal UUID on the admin endpoint
  (`AdminCampaignItem`). `reward_character_name` is the display name;
  `reward_condition` is the administrator text or the derived
  `Complete <campaign name>`. All three are `null` when the tower rewards
  nothing. They are additive: an older client ignores them.

## Character unlocks

A character is owned when it is a *starter* (`ui_characters.is_starter`, set for
`Ray` and `Luna`) or when a `user_characters` row exists. Registration inserts
the starter rows explicitly; migration `0029` backfills every existing account
with the starters and with the character it already had selected, so no profile
loses its choice when ownership starts being enforced.

`GET /api/v1/characters` accepts an **optional** Bearer token and additionally
returns, per character:

| Field | Meaning |
|---|---|
| `unlocked` | the requesting player may select it; anonymous requests see only starters as unlocked |
| `unlock_campaign_id` | public UUID of the tower that rewards it, if any |
| `unlock_campaign_name` | the tower name |
| `unlock_condition` | custom administrator text or `Complete <campaign name>` |

`PATCH /api/v1/auth/me` rejects a character the player does not own with
**422 `{"detail":"Character is locked"}`**. An unknown id keeps
`Unknown character_id`; `character_id: null` still clears the selection.

## Progress and rewards

### `GET /api/v1/campaigns/progress`

Bearer. Server-side progress of the signed-in player:

```json
{"items": [{"campaign_id": "...", "best_stage": 1, "stage_count": 3, "completed": false}]}
```

`stage_count` counts **published** stages only (the same list the client plays).
`best_stage` is the stage of the **current run** (`0..stage_count`), i.e.
`user_campaign_progress.best_stage % stage_count`; the value `stage_count`
means the run is finished. `completed` is history and stays `true` after a
tower has been cleared once, even when a new run is on stage 0.

### `POST /api/v1/campaigns/{campaign_id}/progress`

Bearer, body `{"best_stage": <int >= 0>}`. `extra="forbid"`: a `character_id`
or any other field is **422**. The server:

1. clamps the value to the published stage count;
2. stores `max(existing, incoming)` (progress never decreases);
3. sets `completed_at` when the final published stage is cleared;
4. grants `campaigns.reward_character_id` idempotently
   (`ON CONFLICT DO NOTHING`, `source_campaign_id` = this tower).

Response:

```json
{
  "campaign_id": "...",
  "best_stage": 3,
  "stage_count": 3,
  "completed": true,
  "unlocked_character": {"id": 12, "name": "Jackson", "sheet_url": "/media/characters/....png"},
  "already_owned": false
}
```

- `unlocked_character` is filled **only on the transition to completion** and
  only when the reward was newly granted;
- if the reward was already owned, it is `null` and `already_owned` is `true`;
- a tower without `reward_character_id` completes with a `null` unlock;
- replaying a completed tower grants nothing twice.

The client cannot request a specific unlock: it only reports how many stages it
cleared, and the server decides whether the tower is finished and which
character (if any) is granted. A client that sends a forged `best_stage` only
marks its own progress; the reward still comes from the tower configuration.
The stored counter is monotonic and clamped to the published stage count; the
answer shows the current run (`displayed_stage`), so a report of `3` on a
3-stage tower answers `best_stage: 3, completed: true`.

### `POST /api/v1/campaigns/{campaign_id}/reset`

Bearer, no body. The client's **RESET** button now starts a new run on the
server: the raw `user_campaign_progress.best_stage` counter is set back to `0`,
so the next reported stage is `0` again.

* `completed_at` and `completion_count` are **kept** (history and the repeat XP
  factor); the server never re-grants a reward.
* No XP or rating changes, and there is no body to send.
* Idempotent: a second call returns the same answer. A player who never played
  the tower gets no progress row.
* Unknown campaign → `404`; anonymous → `401`.

Response (`CampaignProgressResult`):

```json
{
  "campaign_id": "...",
  "best_stage": 0,
  "stage_count": 3,
  "completed": true,
  "unlocked_character": null,
  "already_owned": false
}
```

`completed` mirrors the kept `completed_at`; `stage_count` is the current
number of published stages.

## Public endpoint

### `GET /api/v1/campaigns`

Anonymous. Returns `{ "items": [Campaign, ...] }`.

- Only **published** songs (`is_published = true`) are included.
- Campaigns are ordered by `name`, then by `id` as a stable tie-breaker.
- **Decision:** a campaign whose published-song list is empty is **omitted**
  from the public response. It stays visible in the admin panel and reappears
  automatically when one of its songs is published again.

## Admin endpoints

All require `Authorization: Bearer <access_token>` of a user with
`is_admin = true` (the same `require_admin` dependency as `/admin/songs`).
Anonymous → 401, non-admin → 403.

| Method | Path | Body | Response |
|---|---|---|---|
| GET | `/api/v1/admin/campaigns` | — | `{items: [Campaign]}` (all stages, including drafts) |
| POST | `/api/v1/admin/campaigns` | `{name, bot_level, reward_character_id?, reward_condition?, description?}` | 201 `Campaign` (empty song list); 422 unknown reward character |
| GET | `/api/v1/admin/campaigns/{id}` | — | `Campaign`; 404 unknown |
| PATCH | `/api/v1/admin/campaigns/{id}` | `{name?, bot_level?, reward_character_id?, reward_condition?, description?}` | `Campaign`; 404 unknown; 409 duplicate name; 422 null/out-of-range/unknown reward |
| DELETE | `/api/v1/admin/campaigns/{id}` | — | 204; 404 unknown |
| POST | `/api/v1/admin/campaigns/{id}/cover` | multipart `file` (PNG/JPEG/WebP) | 201 `Campaign` with the new `cover_image_url`; 415 unsupported type; 413 over 2 MiB |
| DELETE | `/api/v1/admin/campaigns/{id}/cover` | — | `Campaign` with `cover_image_url: null` |
| PUT | `/api/v1/admin/campaigns/{id}/songs` | `{songs: [{song_id, bot_level?}]}` **или** legacy `{song_ids: [uuid, ...]}` | `Campaign` after the atomic replacement; 404 unknown campaign/song; 422 duplicate ids, out-of-range level, both/neither shape |
| PATCH | `/api/v1/admin/campaigns/{id}/songs/{song_id}` | `{bot_level: 0..2}` | `Campaign`; 404 unknown campaign or a song that is not a stage; 422 out-of-range level |

### Cover image

Uploads are validated by the **file signature** (PNG, JPEG or WebP), never by
the client `content_type`; any other type is **415** and a file over
`MAX_CAMPAIGN_COVER_BYTES` (default 2 MiB) is **413** before anything is
written. The file is stored under `CAMPAIGN_MEDIA_DIR` (default
`/data/landing_media/campaigns`, inside the landing media volume) and published
as an origin-relative URL: `cover_image_url = "/media/campaigns/cover-<uuid>.<ext>"`.
The desktop client downloads `ApiClient::serverOrigin + cover_image_url`.

Every upload gets a new uuid name, so replacing a cover returns a **new URL**
(clients cache by URL) and the previous file is deleted after the new URL is
committed. `DELETE .../cover` removes the file and clears the column; both
routes are idempotent. Deleting a campaign also removes its cover file.
`description` is trimmed; an empty string (or `null`) clears it, more than 400
characters is **422**. The cover has no JSON request field: it is set only by
the upload route.

### Name uniqueness

`campaigns.name` is unique in the database. The API additionally compares names
case-insensitively, so `Tower riffs` and `tower RIFFS` are the same campaign and
the second attempt returns **409 `Campaign name already exists`**. Names are
trimmed; an all-space name is rejected with 422.

### Replacing the ordered list

`PUT /admin/campaigns/{id}/songs` replaces the whole list in one transaction.
Exactly one of two shapes must be sent; sending both or neither is **422**.

Per-stage form (current):

```json
{"songs": [{"song_id": "...", "bot_level": 0}, {"song_id": "...", "bot_level": 2}]}
```

- array order = stage order (`songs[0]` is position `0`);
- an omitted/`null` `bot_level` stores `NULL`, i.e. "use the campaign
  default";
- an explicit `bot_level` outside `0..2` → **422**;

Legacy form (older admin pages):

```json
{"song_ids": ["...", "..."]}
```

- the per-stage level of a song that is already in the campaign is preserved
  (matched by `song_id`);
- a newly added song is stored with `NULL`, i.e. the campaign default;

Common rules:

- duplicates (same UUID twice) → **422**, the previous list is unchanged;
- an unknown song UUID → **404**, the previous list is unchanged;
- `{"songs": []}` / `{"song_ids": []}` clears the campaign; it stays in the
  admin list and disappears from the public list;
- the campaign's `updated_at` is bumped.

The endpoint validates everything **before** deleting the old rows, so a
rejected request never loses the configured stages.

### Changing one stage's level

`PATCH /admin/campaigns/{id}/songs/{song_id}` with `{"bot_level": 0|1|2}`
changes a single stage in place: the order, positions and every other stage are
untouched, and `campaigns.updated_at` is bumped. A `song_id` that is not a stage
of that campaign returns **404**; an out-of-range level returns **422**. The
admin SPA uses the extended `PUT` for `SAVE`; this endpoint exists for small
targeted updates.

## Deletion rules

- Deleting a campaign cascades to `campaign_songs`.
- Permanently deleting a song (`DELETE /admin/songs/{id}?permanent=true`, only
  possible while no player result references it) cascades to its
  `campaign_songs` rows. Archiving a song keeps the stage; the public endpoint
  filters it out because archived songs are unpublished.

## Admin SPA

Tab «КАМПАНИИ / TOWER RIFFS» (`app/static/admin/admin-campaigns.js`, panel
`#sectionCampaigns`):

- create form: name + default bot difficulty (`EASY` / `NORMAL` / `HARD`,
  submitted as `0` / `1` / `2`);
- list table: name, default bot difficulty, song count, edit/delete;
- editor: name, default bot difficulty (`Сложность по умолчанию` — applies to
  new stages), optional `Описание` (textarea, `PATCH` `description`, empty
  clears it), cover block (thumbnail preview or a placeholder, file input,
  `Загрузить обложку` → `POST .../cover`, `Удалить обложку` → `DELETE .../cover`;
  a client-side 2 MiB check rejects oversized files before upload and every
  upload returns a new URL, so the preview reloads), reward character
  (`Персонаж-награда`, options from
  `GET /admin/ui-levels`, internal UUID plus `— без награды —`), optional
  `Условие разблокировки` (empty = derived `Complete <name>`; the placeholder
  shows it), song picker from
  `GET /admin/songs?limit=200&offset=0&archived=false` (title — slug · Dn),
  `ADD`, ordered list with a per-stage `EASY`/`NORMAL`/`HARD` select and
  up/down/remove, `SAVE` (`PATCH` metadata + reward + description when changed
  + `PUT` songs with the per-stage levels in the current order) and `DELETE`
  campaign.
- the list row shows the reward character name under the campaign name.

`GET /admin/songs` accepts `limit` up to **200** (raised from 100 for this
picker; this only widens the accepted range).

## Battle stage results (client 1.8.0+, migration 0030)

`POST /api/v1/campaigns/{campaign_id}/stages/{position}/result` (Bearer) is the
authoritative Battle reward path:

```json
{"run_id": "UUID", "won": true, "score_points": 400, "accuracy_percent": 100.0,
 "perfect_count": 200, "great_count": 0, "good_count": 0, "miss_count": 0,
 "completion_percent": 33.3333, "notes_total": 600, "notes_played": 200}
```

Response: `run_id`, `already_processed`, `won`, `stage_count`, `best_stage`,
`completed`, `score_points`, `normalized_score`, `completion_percent`,
`xp_earned`, `xp_breakdown`, `xp_total`, `level`, `level_up`, `pb_improved`,
`repeat_factor`, `unlocked_character`.

* `position == best_stage % stage_count` (a finished tower starts its next run
  at stage 0); otherwise `409 stage_not_current`. No published stages → `409
  no_stages`. Unknown campaign → 404. The response reports the same value
  (`displayed_stage`), so replayed stages never look like a finished tower.
* `(user_id, run_id)` is unique: retrying the same run returns the stored row
  with `already_processed=true`, without XP or progress changes.
* A loss stores the run (and `normalized_score`) but awards nothing and resets
  the run: `best_stage` becomes `0`, so the next attempt starts at the first
  stage. `completed_at` and `completion_count` are history/anti-farm data and
  are kept.
* XP uses the unchanged solo formula and the tower repeat factor of
  `user_campaign_progress.completion_count`; see `docs/REWARDS_API.md` and
  `GET /api/v1/gameplay/rules`.
* `completion_count` is the number of full completions (backfilled to 1 for
  towers completed before migration 0030), so a replayed tower already starts on
  the 50% slot.
* `POST /campaigns/{id}/progress` is unchanged and still used for local-progress
  sync; it never awards XP.
