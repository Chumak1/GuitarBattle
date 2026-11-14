# Gameplay visuals: UI levels and stage levels

Admin-managed visuals that let a catalogue song point at a character spritesheet
and a stage background. The two selections are independent: a stage level is a
standalone scene name plus a background image and no longer belongs to a
character (migration `0025`). Everything here is additive: a song without a
selection keeps working and the new JSON fields are simply `null`.

## Storage and serving

| Kind | Directory (volume) | Public URL |
|---|---|---|
| Character spritesheet | `GAMEPLAY_MEDIA_DIR/characters` (`gameplay_media_data`) | `/media/characters/<uuid>.png` |
| Stage background | `GAMEPLAY_MEDIA_DIR/stages` (`gameplay_media_data`) | `/media/stages/<uuid>.png` or `.jpg` |

`GAMEPLAY_MEDIA_DIR` defaults to `/data/gameplay_media`;
`MAX_GAMEPLAY_MEDIA_BYTES` defaults to 16 MiB.

Validation never trusts the client `content_type` or file name:

* characters: PNG signature + IHDR, width must be exactly twice the height
  (eight poses in a 4x2 grid);
* stages: PNG or JPEG signature; PNG needs a valid IHDR;
* files are stored under a generated UUID name; the original name is kept only
  as metadata.

## Admin API (Bearer admin, same `require_admin` as `/admin/songs`)

| Method | Path | Body | Notes |
|---|---|---|---|
| GET | `/api/v1/admin/ui-levels` | — | `{items: [UiCharacter]}` |
| POST | `/api/v1/admin/ui-levels` | multipart `name`, `file` | 201; 409 duplicate name; 415 not PNG; 422 wrong grid |
| POST | `/api/v1/admin/ui-levels/{id}/replace` | multipart `file?`, `name?` | Replace sheet and/or rename |
| DELETE | `/api/v1/admin/ui-levels/{id}` | — | 204; songs and player profiles referencing it get `character_id = NULL` |
| GET | `/api/v1/admin/stage-levels` | — | `{items: [StageLevel]}` |
| POST | `/api/v1/admin/stage-levels` | multipart `name`, `file` | 201; standalone scene, no character; 415 not PNG/JPEG |
| POST | `/api/v1/admin/stage-levels/{id}/replace` | multipart `file?`, `name?` | Replace background and/or rename |
| DELETE | `/api/v1/admin/stage-levels/{id}` | — | 204; songs referencing it get `stage_id = NULL` |

`UiCharacter`: `id`, `name`, `sheet_url`, `content_type`, `size_bytes`,
`original_name`, `created_at`, `updated_at`.

`StageLevel`: `id`, `name`, `background_url`, `content_type`, `size_bytes`,
`original_name`, `created_at`, `updated_at`. There is no `character_id` or
`character_name` field: a stage level is standalone.

The STAGE LEVELS admin form asks only for the scene name and the background
image. The UI keeps the previous JSON contract for songs: `character_id` /
`stage_id` are independent optional selections.

Admin pages: `/admin/` → tabs **UI LEVELS** and **STAGE LEVELS**
(`app/static/admin/admin-gameplay.js`).

## Songs

`POST /api/v1/admin/songs` and `PATCH /api/v1/admin/songs/{id}` accept optional
`character_id` and `stage_id` (UUID or `null`). Omitted fields keep the stored
selection; an explicit `null` clears it. The two fields are independent: any
stage can be paired with any character because a stage is a standalone
background (migration `0025`). Unknown IDs still return 422.

The admin song response (list and detail) adds:
`character_id`, `character_name`, `character_sheet_url`, `stage_id`,
`stage_name`, `stage_background_url`.

## Public catalogue

`GET /api/v1/songs` and `GET /api/v1/songs/{slug}` add four optional fields:

```json
{
  "character_name": "Bard",
  "character_sheet_url": "/media/characters/<uuid>.png",
  "stage_name": "Night Club",
  "stage_background_url": "/media/stages/<uuid>.png"
}
```

All four are `null` when the song has no selection. URLs are origin-relative;
the JUCE client joins them with `ApiClient::serverOrigin`, like other media
paths.

## Player character selection

`GET /api/v1/characters` is the public, unauthenticated catalogue used by the
desktop client. It returns a bare JSON array sorted by `name`:

```json
[
  {
    "id": 1,
    "name": "Bard",
    "sheet_url": "/media/characters/<uuid>.png"
  }
]
```

`id` is the integer `ui_characters.public_id` (migration
`0023_user_character`). The admin API keeps exposing the internal UUID under
its own `id`; player-facing code never sees UUIDs.

`GET /api/v1/auth/me` adds `character_id` (`int | null`), `character_name`
(`str | null`) and `character_sheet_url` (`str | null`).
`PATCH /api/v1/auth/me` accepts `character_id` (`int | null`): `null` clears the
selection, an unknown id returns `422 Unknown character_id`, and any
authenticated user may change their own choice. Omitting the field keeps it.

## Admin ownership endpoints (Bearer admin)

Players own a character when it is a starter (`ui_characters.is_starter`) or has
a `user_characters` row. The panel manages that collection:

| Method | Path | Response |
|---|---|---|
| GET | `/api/v1/admin/users/{user_id}/overview` | Full account card; `characters.owned` / `available` / `selected_character_id` |
| POST | `/api/v1/admin/users/{user_id}/characters/{character_id}` | 201 `{character, created: true}`, 200 `created: false` when already owned |
| DELETE | `/api/v1/admin/users/{user_id}/characters/{character_id}` | 200 `{removed, cleared_selected, selected_character_id}` |

`character_id` is the internal `ui_characters` UUID. Granting a starter is a
200 no-op (starters are always owned); revoking one answers 409
`starter_character`. Revoking the selected character clears `users.character_id`.
Unknown accounts or characters answer 404 (`user_not_found`,
`character_not_found`, or `not_owned` for a character the account does not own).
The same card also exposes the online-duel and Battle aggregates; see the
README section «Activity telemetry and admin statistics».

## Database

Migration `0022_gameplay_visuals` (`down_revision 0021_duels`):

* `ui_characters` — unique `name`;
* `stage_levels` — stage background rows (the character link was removed later);
* `songs.character_id` / `songs.stage_id` → `ON DELETE SET NULL`.

Migration `0023_user_character` (`down_revision 0022_gameplay_visuals`) adds:

* `ui_characters.public_id` — `BIGINT` identity, unique, the integer `id` of
  `GET /characters`;
* `users.character_id` — UUID FK to `ui_characters.id`, `ON DELETE SET NULL`.

Migration `0025_stage_level_no_character` (`down_revision 0024_duel_lobbies`)
drops `stage_levels.character_id` together with its index and
`ON DELETE RESTRICT` foreign key. `ui_characters` and `songs.character_id` are
untouched. The downgrade recreates the column as **nullable** with the old FK
and index; the original per-stage assignments cannot be restored, so existing
stages come back with `character_id = NULL`.

Deletion behaviour: deleting a character clears it from songs and player
profiles (`SET NULL`) and does not affect stages; deleting a stage clears it
from songs. Neither removes a song, a result or a file.

## Deploy

```powershell
cd D:\GuitarBattle\GuitarBattleServer
docker compose up --build -d
docker compose ps
curl.exe -s http://localhost:8000/health/ready
docker compose exec api alembic current   # 0025_stage_level_no_character (head)
```

The `api` entrypoint applies the migrations; the `gameplay_media_data` volume is
created automatically. Existing uploads are not affected.
