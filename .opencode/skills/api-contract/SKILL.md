---
name: GuitarBattle API Contract
description: Keep the JUCE HTTP client and FastAPI server contract in sync
---

# Контракт клиент ↔ сервер

## База

- Клиент: `https://fretfight.online/api/v1` — единая константа `ApiClient::baseUrl` в `Source/Network/ApiClient.h`; скачивание ассетов идёт от `ApiClient::serverOrigin`.
- Сервер: `GuitarBattleServer/app/api/v1/`; админ-панель — статическая страница `GuitarBattleServer/app/static/admin/`, отдаётся тем же FastAPI по `/admin/`.
- Формат: JSON UTF-8.
- Авторизация: `Authorization: Bearer <access_token>`. Панель хранит токен в `sessionStorage` и проверяет права запросом к `/admin/songs`.

## Эндпоинты

| Метод | Путь | Тело / параметры | Ответ |
|---|---|---|---|
| POST | `/auth/register` | `username`, `email`, `password`, `display_name?`, `privacy_consent` (**обязательно `true`**, иначе 422 `Privacy policy consent is required`) | профиль, `email_verified=false`, `privacy_consent_at` |
| POST | `/auth/login` | `identifier`, `password` (поле `privacy_consent` игнорируется) | `access_token`, `token_type`, `expires_in`, аддитивно `refresh_token`, `refresh_expires_in`; cookie `ff_refresh`; 403, если почта не подтверждена |
| POST | `/auth/refresh` | cookie `ff_refresh` или `{"refresh_token": "..."}` | новые `access_token` + `refresh_token` (ротация), cookie обновляется; 401 без деталей |
| POST | `/auth/logout` | cookie `ff_refresh` или `{"refresh_token": "..."}` (можно без тела) | 204, cookie очищается, идемпотентно |
| POST | `/auth/verify-email` | `email`, `code` (6 цифр) | `access_token` (+ `refresh_token`, cookie); подтверждает почту |
| POST | `/auth/resend-verification` | `email` | 202, всегда одинаковый ответ |
| POST | `/auth/forgot-password` | `email` | 202, всегда одинаковый ответ |
| POST | `/auth/reset-password` | `email`, `code`, `new_password` | `access_token` (+ `refresh_token`, cookie); старые токены и refresh-сессии инвалидируются |
| GET | `/auth/me` | Bearer | профиль, аддитивно `privacy_consent_at` (`str \| null`), `is_admin` (`bool`, по умолчанию `false`), `avatar_image_url` (`str \| null`, `/media/avatars/...`) и выбор персонажа: `character_id` (`int \| null`), `character_name` (`str \| null`), `character_sheet_url` (`str \| null`) |
| PATCH | `/auth/me` | `display_name?`, `username?`, `character_id?` (хотя бы одно поле в теле; `username` — 3–32 символа `[A-Za-z0-9_.-]`, приводится к нижнему регистру, 409 если занят; `character_id: null` снимает выбор, неизвестный id — 422 `Unknown character_id`, не принадлежащий игроку — 422 `Character is locked`) | профиль |
| POST | `/auth/change-password` | Bearer, `current_password`, `new_password` | 204, без тела; старые токены инвалидируются |
| GET | `/songs` | `search?`, `genre?`, `difficulty?`, `include_drafts?`, `limit`, `offset` | список опубликованных **без** песен со слотом Daily Challenge; у каждого элемента `readiness`, `preview_url` (`str \| null`, абсолютный URL) и `is_published` (`bool`), а также аддитивные `time_signature_numerator` / `time_signature_denominator` (`int`, по умолчанию `4`/`4`); `include_drafts=1` c admin-JWT добавляет черновики |
| GET | `/songs/{slug}` | Bearer admin (опционально) | детали + `version` + `files` + `assets` + `readiness` + `preview_url` + `is_published` + `time_signature_numerator` / `time_signature_denominator` (размер такта, аддитивно); песня дня тоже резолвится (клиент качает ассеты без токена); черновик — только админу, архив — никому |
| GET | `/songs/{slug}/files/{file_id}/download` | Bearer admin (опционально) | MIDI/MP3; разрешены роли `midi`, `drums`, `bass`, `guitar`, `others`, `preview`; файлы черновика — только админу |
| GET | `/characters` | Bearer **опционально** | **голый JSON-массив** `[{id: int, name: str, sheet_url: str, unlocked: bool, unlock_campaign_id: UUID\|null, unlock_campaign_name: str\|null, unlock_condition: str\|null}]`, отсортирован по `name`; пустой список допустим. `id` — целочисленный `ui_characters.public_id`, `sheet_url` — origin-relative `/media/characters/<uuid>.png`. Без токена `unlocked=true` только у стартовых персонажей (`is_starter`); с токеном — у стартовых и выданных (`user_characters`). `unlock_condition` — текст админа или `Complete <campaign name>`; `null`, если башня не выдаёт персонажа. Поля аддитивные: старый сервер их не присылает, клиент считает всех разблокированными |
| GET | `/admin/songs` | Bearer admin, `search?`, `published?`, `limit`, `offset` | черновики и опубликованные; у каждой песни `daily_weekday` |
| GET | `/admin/ui-levels` | Bearer admin | `{items: [{id: UUID, name, sheet_url, content_type, size_bytes, original_name, created_at, updated_at}]}` |
| POST | `/admin/ui-levels` | Bearer admin, multipart `name`, `file` (PNG 4x2) | 201; 409 дубликат имени; 415 не PNG; 422 неверная сетка |
| POST | `/admin/ui-levels/{id}/replace` | Bearer admin, multipart `file?`, `name?` | замена спрайт-листа и/или переименование |
| DELETE | `/admin/ui-levels/{id}` | Bearer admin | 204; у песен и профилей игроков `character_id` становится `NULL` |
| GET | `/admin/stage-levels` | Bearer admin | `{items: [{id: UUID, name, background_url, content_type, size_bytes, original_name, created_at, updated_at}]}`; поля персонажа **нет** |
| POST | `/admin/stage-levels` | Bearer admin, multipart `name`, `file` (PNG/JPEG) | 201; сцена самостоятельная, `character_name` не принимается и не требуется; 415 не PNG/JPEG |
| POST | `/admin/stage-levels/{id}/replace` | Bearer admin, multipart `file?`, `name?` | замена фона и/или переименование |
| DELETE | `/admin/stage-levels/{id}` | Bearer admin | 204; у песен `stage_id` становится `NULL` |
| POST | `/admin/songs` | Bearer admin, `slug`, `title`, `bpm`, остальные поля опциональны, `daily_weekday?` (0–6 или `null`), `time_signature_numerator?` (1–32), `time_signature_denominator?` (1/2/4/8/16/32) | черновик |
| GET | `/admin/songs/{id}` | Bearer admin | детали + `assets` + `files[]` (все версии с `is_active`) + `readiness` + `daily_weekday` + `time_signature_numerator` / `time_signature_denominator` |
| PATCH | `/admin/songs/{id}` | Bearer admin, поля песни; `daily_weekday: null` снимает слот; `time_signature_numerator`/`time_signature_denominator` — NOT NULL, `null` → 422 | обновлённая песня |
| GET | `/admin/daily` | Bearer admin | 7 слотов `{weekday, weekday_name, song \| null, updated_at}` |
| PUT | `/admin/daily/{weekday}` | Bearer admin, `{song_id: UUID \| null}`, weekday 0–6 | слот; назначение переносит песню, `422` вне 0–6, `404` неизвестная песня |
| POST | `/admin/songs/{id}/publish` | Bearer admin | публикация |
| POST | `/admin/songs/{id}/unpublish` | Bearer admin | снятие с публикации |
| DELETE | `/admin/songs/{id}` | Bearer admin | 204 |
| POST | `/admin/songs/{id}/files/midi` | Bearer admin, multipart `file` | новый `SongFile` (`role=midi`), активный; прежние версии роли — архив |
| POST | `/admin/songs/{id}/files/audio/{role}` | Bearer admin, multipart `file`; `role`: `drums`, `bass`, `guitar`, `others`, `preview` | новый `SongFile`, активный; прежние версии роли — архив. `preview` — полный микс, не влияет на `readiness`/публикацию и не меняет `Song.version` |
| DELETE | `/admin/songs/{id}/files/preview` | Bearer admin | 204, идемпотентно; удаляет все версии preview (файлы и строки), `Song.version` не меняется |
| GET | `/admin/songs/{id}/files` | Bearer admin | все версии файлов; у каждой `is_active` |
| GET | `/admin/songs/{id}/files/{file_id}/download` | Bearer admin | файл (активный или архивный) |
| POST | `/admin/songs/{id}/files/{file_id}/activate` | Bearer admin | 200, файл становится активным для своего `(song, role)`, остальные версии роли — архив; 404 если файл не найден/не относится к песне; `version` песни +1 |
| POST | `/admin/songs/{id}/files/{file_id}/analyze` | Bearer admin | сохранённый анализ MIDI (404 `MIDI has not been analyzed yet`, если анализ ещё не запускали) |
| POST | `/games/sessions` | `song_id` (UUID), `speed_percent`, `daily?` (bool, по умолчанию `false`), `practice?` (bool, по умолчанию `false`) | сессия; `mode` = `normal`/`daily`/`preview` (черновик, только админ)/`practice` (Practice-раздел); `daily: true` + `practice: true` → 422 |
| POST | `/games/sessions/{id}/finish` | `score_points`, `accuracy_percent`, `max_combo`, `perfect_count`, `good_count`, `miss_count`, опционально `completion_percent`, `avg_timing_error_ms`, `notes_total`, `notes_played` | score + блок прогрессии (см. ниже); у `preview` и `practice` прогрессия нулевая |
| GET | `/games/history` | Bearer, `limit`, `offset` | история; `practice`-сессии не попадают |
| GET | `/games/best-scores` | Bearer | лучший счёт по каждой опубликованной песне вне Daily Challenge + сумма рейтинга (только `mode == "normal"`) |
| GET | `/leaderboard` | `song_slug?`, `limit`, `offset` | результаты |
| GET | `/leaderboard/global` | `limit`, `offset` | **Guitar Skills** — главный рейтинг. `items[]`: `rank`, `guitar_skills`, `accuracy_score`, `challenge_score`, `score_performance`, `duel_performance`, `progression_score`, `confidence`, `provisional`, `accuracy_percent`, `duel_wins`/`duel_losses`/`duel_draws`/`duel_win_rate`, `duel_matches`, `events_counted`, `position_change` (`null` — нет предыдущего пересчёта), аддитивно `avatar_image_url` (`str \| null`). Legacy-поля `rating_points`, `games_played`, `best_accuracy`, `achievements_unlocked` сохранены, но сортировка теперь по `guitar_skills` desc |
| GET | `/leaderboard/global/{user_id}` | — (публичный) | та же строка + `last_calculated_at`, `calculation_version`, `components.{accuracy,challenge,score,duel,progression}` = `{value, events, confidence}`; 404 `Player not found` |
| GET | `/admin/guitar-skills/config` | Bearer admin | активная конфигурация: 5 весов, `window_size`, `recency_decay`, tier-границы и множители, note-factor, `median_note_count`, `level_normalization_max`, `xp_reference`, duel strength/scale/reliability, anti-farm cap, confidence/provisional, `calculation_version`, `updated_at` |
| PUT | `/admin/guitar-skills/config` | Bearer admin, partial JSON + `recalculate?` (по умолчанию `true`) | сохранённая конфигурация; 422, если пять весов не дают 1.0 (±0.0001) или нарушены диапазоны; пишет историю и запускает фоновый пересчёт |
| GET | `/admin/guitar-skills/config/history` | Bearer admin, `limit` (1–200) | `{items:[{id, config, admin_user_id, created_at}]}` |
| POST | `/admin/guitar-skills/recalculate` | Bearer admin, `{dry_run: bool, reason?}` | `{users, changed, items:[{user_id, username, old_skills, new_skills, old_rank, new_rank, delta}]}`; dry run ничего не пишет; 409, если пересчёт уже идёт |
| GET | `/admin/guitar-skills/status` | Bearer admin | `{running, last_calculated_at, calculation_version, users, provisional, incomplete_history}` |
| GET | `/profile/progression` | Bearer | уровень, XP, мастерство, жанры, статистика |
| GET | `/profile/personal-bests` | Bearer, `limit` (1–100, по умолчанию 10), `offset` | личные рекорды по опубликованным песням |
| GET | `/achievements` | Bearer | каталог достижений + прогресс/разблокировка; у каждого элемента `icon` и `icon_url` |
| GET | `/profile/users/{user_id}` | Bearer | публичная карточка игрока; у выданных достижений `icon` и `icon_url`; аддитивно `avatar_image_url` |

| GET | `/media/achievements/<icon>.svg` | — (публичный) | SVG иконки достижения, `image/svg+xml` |
| GET | `/daily/today` | Bearer | `date`, `weekday` (0=Пн…6=Вс, UTC), `song` (или `null`), официальная попытка, топ-5 |
| GET | `/daily/leaderboard` | Bearer, `date?` (YYYY-MM-DD, по умолчанию сегодня UTC) | официальные результаты дня |
| GET | `/daily/history` | Bearer, `limit` (1–100, по умолчанию 20), `offset` | официальные результаты игрока по дням с `rank` и `participants` |
| GET | `/client-assets/manifest` | — (публичный) | `schema`, `version`, `archive_url`, `archive_size_bytes`, `archive_sha256`, `files[]`; 503, если manifest или bundle отсутствует |
| GET/HEAD | `/client-assets/bundle.zip` | — (публичный) | zip `guitarbattle-sounds.zip`; 503, если архив отсутствует |
| GET | `/landing` | — (публичный) | `blocks`, `hero_background`, `download` (Windows installer, для обратной совместимости) и аддитивные `downloads` (`{"windows": DownloadInfo\|null, "macos": DownloadInfo\|null}`) — установщики; `updates` — та же карта для **portable** ZIP автообновления (`url` = `/api/v1/downloads/game/update?platform=...`, `available=false`, если portable-сборки нет). `DownloadInfo` = `available`, `version`, `filename`, `size_bytes`, `url`, `release_notes`; аддитивно `hero_battle_url`, `hero_duel_url`, `default_avatar_url` — те же значения, что у `/site/config` |
| GET | `/downloads/game` | — (публичный), `platform=windows\|macos` (по умолчанию `windows`) | активный **installer** платформы (Windows `.exe/.msi`, macOS `.dmg`); 404, если установщика нет; 422 для неизвестного `platform` |
| GET | `/downloads/game/update` | — (публичный), `platform=windows\|macos` (по умолчанию `windows`) | активный **portable** ZIP платформы; 404, если portable-сборки нет; 422 для неизвестного `platform` |
| GET | `/geo` | — (публичный) | `country`, `language` (`ru`/`en`/`null`) по IP из локальной DB-IP Lite |
| GET | `/site/contacts` | — (публичный) | `name` (`ru`/`en`), `email`, `telegram` (`@handle`), `telegram_url`, `socials` (`youtube`/`instagram`/`twitter`/`vk`; `null` — заглушка «Скоро»); без записи в CMS отдаёт дефолты |
| POST | `/activity/heartbeat` | Bearer, `client_session_id` (≤64), `app_version?`, `platform?`, `final=false` | 200, `session_id`, `started_at`, `last_seen_at`; upsert по `(user_id, client_session_id)`, `final=true` ставит `ended_at` |
| POST | `/activity/game-launch` | Bearer, `client_session_id?`, `game_key` (≤120), `song_id?`, `mode?` | 201, `id`; неизвестные `song_id`/сессия сохраняются как `NULL`, а не 4xx |
| POST | `/feedback` | — (Bearer опционален), `message` (1–4000, обязателен), `username?`, `app_version?`, `attachment_name?`, `attachment_data?` (base64) | 200 `{"ok": true}`; `503 {"ok": false, "detail": "mail is not configured"}` без SMTP и `503 {"ok": false, "detail": "mail delivery failed"}` при ошибке отправки; 422 на пустое/слишком длинное сообщение, неверный base64 и вложение >5 МиБ после декодирования. Письмо уходит на `FEEDBACK_EMAIL_TO` (по умолчанию `chumakevgeny@gmail.com`), тема `FretFight feedback` (+ `from <username>`), в теле сообщение, username, email, app version; `Reply-To` — email аккаунта из Bearer-токена |
| GET | `/admin/statistics/overview` | Bearer admin, `days=30` (1–365) | `generated_at`, `users_total`, `users_active_24h`, `users_active_7d`, `sessions_total`, `app_seconds_total`, `avg_session_seconds`, `launches_total`, `launches_24h`, `countries[]` (`code`, `name`, `users` — топ-10 стран регистрации, аддитивно) |
| GET | `/admin/statistics/users` | Bearer admin, `search?`, `country?` (ISO 3166-1 alpha-2, регистр не важен), `sort=last_seen\|username\|display_name\|email\|created_at\|app_seconds\|sessions_count\|launches_count\|scores_count`, `order=asc\|desc` (по умолчанию `asc` для текста, `desc` для чисел/дат), `limit=50` (1–200), `offset=0` | `items[]` (`user_id`, `username`, `display_name`, `email`, `is_active`, `created_at`, `app_seconds_total`, `sessions_count`, `last_seen_at`, `launches_count`, `last_launch_at`, `games_distinct`, `scores_count`, `best_score`, `country_code`, `country_name`, `country_flag`, `registration_ip`, `last_login_ip`), `total`, `limit`, `offset` |
| GET | `/admin/statistics/games` | Bearer admin, `days=30`, `limit=100` | `items[]` (`game_key`, `title`, `launches`, `unique_users`, `last_launch_at`), `total` |
| DELETE | `/admin/users/{user_id}` | Bearer admin | 204; 400 при удалении себя (`"You cannot delete your own account"`), 404 если нет, 409 если связанные данные мешают; жёсткое удаление, все FK на `users.id` — `CASCADE` |
| GET | `/admin/site/landing` | Bearer admin | `blocks`, `hero_background` |
| PUT | `/admin/site/landing` | Bearer admin, `blocks`, `hero_background` | сохранённый контент; блоки ≤ 256 КиБ, строки ≤ 5000, глубина ≤ 8 |
| POST | `/admin/site/landing/background` | Bearer admin, multipart `file` | 201, `hero_background`; JPEG/PNG/WebP/GIF/MP4/WebM |
| DELETE | `/admin/site/landing/background` | Bearer admin | 200, `hero_background.type = "none"` |
| GET | `/admin/site/contacts` | Bearer admin | `name_ru`, `name_en`, `email`, `telegram`, `socials` (сохранённые значения или дефолты) |
| PUT | `/admin/site/contacts` | Bearer admin, те же поля | сохранённые нормализованные значения; 422 с именем поля в `detail` для битого email/telegram/URL соцсети; пишет `content["contacts"]`, не трогая `blocks`/`hero_background` |
| GET | `/campaigns` | — (публичный) | `{items: [Campaign]}`; только опубликованные песни; кампании без опубликованных песен опускаются; кампании по `name`, песни по `position`. `Campaign` = `{id, name, bot_level (0=EASY,1=NORMAL,2=HARD — сложность по умолчанию), song_count, songs[{song_id, slug, title, difficulty, is_published, position, bot_level (эффективная сложность этапа, 0..2)}], reward_character_id (публичный int\|null), reward_character_name (str\|null), reward_condition (str\|null), created_at, updated_at}`; `songs[].bot_level` аддитивное — старый сервер его не присылает, клиент берёт `Campaign.bot_level`; поля награды аддитивные |
| GET | `/campaigns/progress` | Bearer | `{items: [{campaign_id, best_stage, stage_count, completed}]}` — серверный прогресс текущего игрока по каждой кампании; `stage_count` считает только опубликованные этапы; `best_stage` — этап **текущего прохода** (`user_campaign_progress.best_stage % stage_count`, `0..stage_count`; `stage_count` = проход завершён), `completed` — история и остаётся `true` после первого прохождения |
| POST | `/campaigns/{campaign_id}/progress` | Bearer, `{best_stage: int >= 0}` (`extra=forbid` — `character_id` и любые другие поля → 422) | `{campaign_id, best_stage, stage_count, completed, unlocked_character: {id,name,sheet_url}\|null, already_owned: bool}`; монотонный upsert `max(старое, новое)` с обрезкой по числу опубликованных этапов; `best_stage` в ответе — этап текущего прохода; при завершении сервер сам выдаёт `campaigns.reward_character_id` идемпотентно. `unlocked_character` заполняется только на переходе к завершению; если награда уже была — `already_owned=true` и `unlocked_character=null`. Клиент не может запросить конкретного персонажа |
| POST | `/campaigns/{campaign_id}/reset` | Bearer, без тела | `{campaign_id, best_stage: 0, stage_count, completed, unlocked_character: null, already_owned: false}`; кнопка **RESET** клиента начинает новый проход на сервере: сырой счётчик `best_stage` → `0`, а `completed_at`/`completion_count` сохраняются (история и повторный XP-фактор), награда и XP не меняются; идемпотентно, строка не создаётся, если игрок не играл башню; неизвестная кампания → 404, без токена → 401 |
| GET | `/admin/campaigns` | Bearer admin | `{items: [Campaign]}` — все этапы, включая черновики; `reward_character_id` — внутренний UUID |
| POST | `/admin/campaigns` | Bearer admin, `{name, bot_level, reward_character_id?: UUID\|null, reward_condition?: str\|null}` | 201 `Campaign` с пустым списком; 409 при совпадении имени без учёта регистра; 422 (в т.ч. неизвестный `reward_character_id`) |
| GET | `/admin/campaigns/{id}` | Bearer admin | `Campaign`; 404 |
| PATCH | `/admin/campaigns/{id}` | Bearer admin, `{name?, bot_level?, reward_character_id?, reward_condition?}` | `Campaign`; 404/409/422; `bot_level` меняет только значение по умолчанию (этапы с `NULL` следуют за ним); `null` в награде очищает её, `null` в `name`/`bot_level` → 422 |
| DELETE | `/admin/campaigns/{id}` | Bearer admin | 204; 404; этапы удаляются каскадом |
| PUT | `/admin/campaigns/{id}/songs` | Bearer admin, ровно одна из форм: `{songs: [{song_id, bot_level?}]}` (порядок = порядок этапов, `bot_level` 0..2, отсутствие → сложность кампании) **или** legacy `{song_ids: [uuid, ...]}` | `Campaign`; замена списка одной транзакцией; legacy сохраняет уровни оставшихся песен по `song_id`, новые получают `campaigns.bot_level`; дубликаты/уровень вне 0..2/обе или ни одна форма → 422; неизвестная песня → 404; при ошибке старый список сохраняется; пустой список очищает кампанию |
| PATCH | `/admin/campaigns/{id}/songs/{song_id}` | Bearer admin, `{bot_level: 0\|1\|2}` | `Campaign`; меняет уровень одного этапа, порядок и остальные этапы не трогает; 404 неизвестная кампания или песня не из этой кампании; 422 уровень вне 0..2 |
| GET | `/admin/site/game-builds` | Bearer admin, `platform?` (`windows`/`macos`), `artifact?` (`installer`/`portable`) | список загруженных сборок; у каждой `platform`, `artifact` и `url` (у portable — `/downloads/game/update?...`); 422 для неизвестного фильтра |
| POST | `/admin/site/game-builds` | Bearer admin, multipart `file`, `platform?` (по умолчанию `windows`), `artifact?` (по умолчанию `installer`), `version?`, `release_notes?` (≤8000) | 201, сборка создаётся неактивной; installer: Windows — `.exe/.msi/.zip`, macOS — `.dmg/.zip`; portable: только `.zip` на обеих платформах; неверная пара платформа/артефакт/расширение → 415, неизвестная `platform`/`artifact` → 422 |
| PATCH | `/admin/site/game-builds/{id}` | Bearer admin, JSON `{release_notes}` (≤8000) | 200, обновлённая сборка; меняет только описание, файл, `artifact` и `is_active` не трогает |
| POST | `/admin/site/game-builds/{id}/activate` | Bearer admin | 200, сборка становится единственной активной в своей паре `(platform, artifact)`; installer и portable друг друга не снимают, другая платформа тоже остаётся активной |
| DELETE | `/admin/site/game-builds/{id}` | Bearer admin | 204 |
| GET | `/app/background-music` | — (публичный) | всегда 200: `{url: str \| null, content_type: str \| null, size_bytes: int}`; без файла — `null`/`null`/`0` |
| GET | `/app/background-music/file` | — (публичный) | текущий MP3 (`audio/mpeg`, `Cache-Control: no-cache`); 404, если трека нет |
| GET | `/admin/app/background-music` | Bearer admin | `url`, `content_type`, `size_bytes`, `uploaded_at`, `original_name`, `sha256` |
| POST | `/admin/app/background-music` | Bearer admin, multipart `file` (только MP3) | 201 при первой загрузке, 200 при замене; тот же shape; 400 не-MP3, 413 больше 100 МиБ |
| DELETE | `/admin/app/background-music` | Bearer admin | 204, идемпотентно |
| GET | `/site/config` | — (публичный) | `{hero_battle_url: str\|null, hero_duel_url: str\|null, default_avatar_url: str\|null}` — картинки из админки, URL origin-relative (`/media/artwork/<slot>-<uuid>.<ext>`); `null` — слот пуст, клиент рисует запасной вариант |
| GET | `/admin/artwork` | Bearer admin | `{items: [{slot, url\|null, updated_at\|null}]}`; слоты всегда все три в порядке `hero-battle`, `hero-duel`, `default-avatar` |
| POST | `/admin/artwork/{slot}/image` | Bearer admin, multipart `file`; `slot` ∈ `hero-battle`/`hero-duel`/`default-avatar` | 201 `{slot, url, updated_at}`; PNG/JPEG/WebP по сигнатуре, hero — ≤ 8 МБ, default-avatar — ≤ 2 МБ; 415 не тот формат, 413 слишком большой, 422 неизвестный слот; замена удаляет прежний файл |
| DELETE | `/admin/artwork/{slot}/image` | Bearer admin | 200 `{slot, url: null, updated_at: null}`, идемпотентно; файл удаляется |
| POST | `/profile/avatar` | Bearer, multipart `file` | 201 `{avatar_image_url}`; `/media/avatars/avatar-<uuid>.<ext>`, PNG/JPEG/WebP ≤ 2 МБ; 415/413; замена удаляет прежний файл |
| DELETE | `/profile/avatar` | Bearer | 200 `{avatar_image_url: null}`, идемпотентно |

Публичные страницы сайта: `/` (лендинг), `/policy` (политика конфиденциальности),
`/login`, `/register`, `/account`, `/admin/` (только администраторы).
Статистика — вкладка «Статистика» внутри `/admin/`, отдельного `/statistics` нет.
Медиа лендинга отдаётся по `/media/landing/<name>`.

## Кампании: сложность этапа (миграция `0028_campaign_song_bot_level`)

- `campaign_songs.bot_level` (`SMALLINT NULL`, CHECK `0..2`) — сложность
  конкретного этапа. `NULL` означает «взять `campaigns.bot_level`».
- `campaigns.bot_level` остаётся значением по умолчанию: применяется к новым
  этапам и служит fallback для `NULL`. Миграция `0028`
  (`down_revision = 0027_duel_lobby_start_confirm`) заполнила все существующие
  строки уровнем их кампании одним `UPDATE ... FROM`, так что старые башни
  играются ровно как раньше.
- В публичном и админском `Campaign` у каждого `songs[]` есть
  `bot_level` — **эффективная** сложность (`link.bot_level`, иначе
  `campaigns.bot_level`), всегда `0..2`. Поле аддитивное: клиент 1.5+
  использует его, старый клиент/сервер — `Campaign.bot_level`.
- `PUT /admin/campaigns/{id}/songs` принимает новую форму
  `{"songs": [{"song_id": "...", "bot_level": 0}, ...]}`; legacy
  `{"song_ids": [...]}` продолжает работать и сохраняет уровни оставшихся
  песен (по `song_id`), новые получают `campaigns.bot_level`. Ровно одна форма
  за запрос, иначе 422; уровень вне `0..2` — 422.
- `PATCH /admin/campaigns/{id}/songs/{song_id}` с `{"bot_level": 0|1|2}` —
  точечная правка одного этапа (404, если песня не входит в кампанию);
  порядок и остальные этапы не меняются.
- Админка (`admin-campaigns.js`): «Сложность по умолчанию» в шапке редактора,
  отдельный select EASY/NORMAL/HARD у каждого этапа; `SAVE` шлёт обе формы
  состояния в одном `PUT`.
- Клиент (`CampaignStageView.botLevel`) стартует этап с уровнем песни
  (`startCampaignStage` → `requestDuelWithBot(stage.botLevel)`), а не с
  уровнем кампании.

## Персонажи и разблокировки (миграция `0029_character_unlocks`)

Модель: персонаж открыт, если он **стартовый** (`ui_characters.is_starter`)
или у игрока есть строка в `user_characters`. Награду выдаёт только сервер.

- `ui_characters.is_starter` (`BOOLEAN NOT NULL DEFAULT false`) — при миграции
  выставлен `Ray`/`Luna` (case-insensitive; отсутствие имени не ошибка).
  `user_characters` — `(user_id, character_id)` PK, `source_campaign_id`
  (`NULL` для стартовых и для персонажа, выбранного профилем до миграции).
  Бэкфилл выдаёт каждому существующему пользователю стартовых и его текущего
  `users.character_id`, поэтому старые профили ничего не теряют.
- Регистрация добавляет строки для всех стартовых персонажей; при удалении
  персонажа строки каскадятся, при удалении кампании `source_campaign_id`
  становится `NULL`.
- `PATCH /auth/me` с `character_id`: неизвестный id — 422
  `Unknown character_id`; известный, но не принадлежащий игроку — 422
  `Character is locked`. `character_id: null` по-прежнему снимает выбор.
- `GET /characters` с необязательным Bearer отдаёт `unlocked`,
  `unlock_campaign_id`, `unlock_campaign_name`, `unlock_condition`. Анонимно
  открыты только стартовые. Поля аддитивные: старая связка (клиент/сервер)
  продолжает работать без блокировок.
- `campaigns.reward_character_id` (`UUID NULL`, `ON DELETE SET NULL`) и
  `campaigns.reward_condition` (`VARCHAR(200) NULL`) — награда башни. Текст
  обрезается, пустой → `NULL`; публичный `Campaign` отдаёт
  `reward_character_id` как **публичный int**, админский — как внутренний UUID.
  Публичный `reward_condition` — кастомный текст или `Complete <name>`.
- `user_campaign_progress` — `(user_id, campaign_id)` PK, `best_stage >= 0`,
  `completed_at`, `updated_at`. `POST /campaigns/{id}/progress` принимает
  только `best_stage`, обрезает его по числу **опубликованных** этапов,
  хранит максимум, при завершении ставит `completed_at` и выдаёт награду
  идемпотентно (`ON CONFLICT DO NOTHING`). Никакого поля с id персонажа нет:
  клиент не может попросить конкретную разблокировку.
- `best_stage` в ответах всех эндпоинтов (кроме legacy-хранилища
  `/progress`) — этап **текущего прохода**: `best_stage % stage_count`
  (`displayed_stage` в `app/services/campaigns.py`). Сырой счётчик
  кумулятивный и не обрезается `min`: иначе после 3-этапной башни и победы на
  этапе 0 сервер вернул бы `min(4,3)=3`, и клиент снова показал бы
  «BATTLE COMPLETE». `POST /campaigns/{id}/reset` (Bearer, без тела) сбрасывает
  сырой счётчик в 0, сохраняя `completed_at`/`completion_count`; идемпотентно.
- `GET /campaigns/progress` — серверный прогресс по всем кампаниям. Клиент
  после входа отправляет локальные `CampaignBest:<id>` (> 0, обрезанные по
  `song_count`), затем берёт `max(локальный, серверный)`; при победе над
  финальным этапом отправляет новый счёт. Ответ с `unlocked_character`
  показывает «New character unlocked: <Name>» и обновляет PROFILE.
- Админка (`admin-campaigns.js`): в редакторе «Персонаж-награда» (select из
  `GET /admin/ui-levels`, значение — внутренний UUID, плюс «— без награды —»)
  и «Условие разблокировки» (пусто → `Complete <name>`, placeholder показывает
  вычисленный текст). Список кампаний показывает имя награды.

## Сайт, лендинг и язык

- Лендинг — шаблон `app/static/landing/index.html` с плейсхолдерами `{{ key }}`;
  `app/services/landing_page.py` рендерит его на сервере: `/` — русский документ,
  `/en` — английский (`<html lang>`, title/description/OG/JSON-LD и тексты без JS).
  Тексты — `app/services/landing_content.py`, браузерный дубль для переключателя
  языка — `landing-i18n.js` (тест `tests/test_landing_seo.py` ловит расхождение).
  `app/static/app/landing.js` запрашивает `/landing` и накладывает CMS-переопределения
  поверх отрендеренного текста.
- `GET /robots.txt` и `GET /sitemap.xml` генерируются тем же модулем: sitemap
  содержит `/`, `/en`, `/policy`, `/support`, `/login`, `/register`, у языковой пары
  — `xhtml:link` alternates; `lastmod` берётся из mtime шаблона.
- Соцпревью: `app/static/app/og-image.png` (ru) и `og-image-en.png` (en), 1200×630,
  пересобираются `scripts/build_og_image.ps1`.
- `blocks` — объект `{hero, facts, about, how, features, requirements, cta}`; каждый
  блок: `{visible: bool, ru: {...}, en: {...}}`. Админ-редактор всегда сохраняет
  оба языка целиком, поэтому публичный ответ содержит полный снимок.
- `hero_background`: `{type: "none" | "image" | "video", url, poster_url}`. Файлы
  лежат в `LANDING_MEDIA_DIR` (`/data/landing_media`), лимит 50 МиБ, сигнатуры
  проверяются на сервере.
- Контакты — `content["contacts"]` в той же строке `landing_settings` (миграции
  не требуют): `{name_ru, name_en, email, telegram, socials}`. Публичный
  `GET /site/contacts` нормализует telegram (`@handle`, `handle`, `t.me/handle`,
  `https://t.me/handle` → `@handle` + `telegram_url`) и пропускает только
  абсолютные `http(s)`-ссылки разрешённых доменов; пустая соцсеть — `null`.
  Блок статически вставлен на `/`, `/login`, `/register`, `/account`, `/policy`;
  `app/static/app/contacts.js` подменяет значения из API, без JS остаются
  дефолты и 4 заглушки соцсетей.
- Сборки игры лежат в `GAME_BUILDS_DIR` (`/data/game_builds`), лимит 500 МиБ.
  У сборки две координаты: `platform` (`windows`/`macos`, миграция
  `0017_game_build_platform`; macOS — одна универсальная сборка для Apple
  Silicon и Intel) и `artifact` (`installer`/`portable`, миграция
  `0018_game_build_artifact`, `server_default='installer'`, CHECK, индекс).
  `installer` — то, что ставит новый пользователь и что отдаёт кнопка на сайте;
  `portable` — ZIP, который установленный клиент распаковывает сам.
  Правила расширений — `app/services/game_builds.py`
  (`GAME_BUILD_TYPES[platform][artifact]`): installer — Windows
  `.exe/.msi/.zip` (zip legacy), macOS `.dmg/.zip` (zip legacy); portable —
  только `.zip` на обеих платформах. Неверная пара → 415 с текстом
  `Windows portable ZIP must be .zip`, неизвестные значения → 422.
  Активна максимум одна сборка на пару `(platform, artifact)`; активация
  portable не снимает `is_active` у installer той же платформы и наоборот.
  `GET /downloads/game` отдаёт активный installer (без `?platform=` — Windows),
  `GET /downloads/game/update?platform=...` — активный portable ZIP.
  На лендинге рядом с кнопкой Windows стоит ссылка
  `/api/v1/downloads/game?platform=macos`; пока macOS-установщика нет, она
  отключена и подписана «Скоро»/«Coming soon».
- `game_builds.release_notes` (Text, NOT NULL, default `''`, миграция
  `0016_game_build_release_notes`) — текст «что нового», который администратор
  пишет в админке. Отдаётся в `GET /landing` как `download.release_notes`
  (пустая строка, если описания нет) и в админском `GameBuildItem`.
  `PATCH /admin/site/game-builds/{id}` меняет описание без повторной загрузки
  установщика; `POST` принимает `release_notes` в multipart-форме.
- Клиент (`MainComponent::checkForUpdatesOnLaunch`) читает `updates` из
  `/landing` (`updates[windows|macos]`), а `downloads` использует только как
  fallback для старых серверов: установщик с сайта и канал автообновления
  разведены. `updates.*.url` — `/api/v1/downloads/game/update?platform=...`,
  `available=false` — portable-сборки нет. `release_notes` показывается в
  диалоге обновления, если он непустой.
- Админка (`app/static/admin/index.html`, `admin-site.js`): у формы загрузки
  два селектора — `Платформа` и `Артефакт` (`Установщик` /
  `Portable ZIP для автообновления`); `accept` и подсказка считаются из пары
  `platform+artifact` (`BUILD_ARTIFACTS` в JS). В таблице колонка `Артефакт`
  с бейджем `Установщик`/`Portable`, фильтр по артефакту рядом с фильтром
  платформ, а сводка активных сборок выводится отдельно для installer и
  portable по каждой видимой платформе.
- Язык публичного сайта — cookie `ff_lang` (`ru`/`en`). Middleware на первом HTML-
  запросе определяет страну по локальной базе DB-IP Lite (`GEOIP_DB_PATH`,
  обновляется `scripts/fetch_geoip.py`) и ставит cookie для России/СНГ → `ru`,
  остальные → `en`. Если IP неизвестен, `i18n.js` запрашивает `/geo`, затем
  использует язык браузера. Админ-панель всегда русская (`data-i18n-off`).
- Политика `/policy` содержит русский и английский тексты в одном документе;
  CSS показывает нужный по `html[lang]`. Формы входа и регистрации требуют
  согласия со ссылкой на `/policy`.

Админские маршруты `/admin/songs...` требуют `is_admin`.

## Активность и статистика

- Клиент шлёт `POST /activity/heartbeat` при старте и периодически (например,
  раз в 2 минуты), `client_session_id` — свой UUID на запуск процесса.
  `final=true` при выходе ставит `ended_at`. Повторные вызовы не создают
  дубликат: уникальность `(user_id, client_session_id)`.
- `POST /activity/game-launch` — один вызов на вход в игровой режим.
  `game_key`: `song` (с `song_id` опубликованной песни), `daily_challenge`,
  `practice`, `local:<slug>`. Сервер не отвечает 4xx на неизвестный `song_id`
  или отсутствующую сессию: такие поля сохраняются как `NULL`, телеметрия
  не должна ломать игру.
- Время считает только сервер: `coalesce(ended_at, last_seen_at) - started_at`
  по каждой `app_sessions`; отрицательные значения обрезаются до 0.
  Клиент не отправляет длительность.
- Статистика (только `is_admin`): `/admin/statistics/overview`,
  `/admin/statistics/users`, `/admin/statistics/games`. Период `days`
  ограничивает сессии и запуски; `users_total` всегда по всем аккаунтам.
  `users_active_24h/7d` — уникальные пользователи с heartbeat или запуском
  в окне. `last_seen` сортирует по максимуму из `last_seen_at` и
  `last_launch_at`, пустые значения в конце.
- `/admin/statistics/users`: `sort` — `last_seen`, `username`, `display_name`,
  `email`, `created_at`, `app_seconds`, `sessions_count`, `launches_count`,
  `scores_count`; `order` — `asc`/`desc`, по умолчанию `asc` для текстовых
  столбцов и `desc` для чисел/дат. Текст сравнивается через `lower()`,
  `NULLS LAST` в обе стороны, tiebreaker — `username`, затем `id`.
  `scores_count`/`best_score` считаются подзапросом `scores` через
  `game_sessions` (без N+1); `best_score = null`, если результатов нет.
- `/admin/statistics/users?country=RU` — фильтр по стране регистрации
  (ISO 3166-1 alpha-2; принимается в любом регистре, в БД хранится верхним).
  В строке — аддитивные `country_code`, `country_name`, `country_flag`
  (эмодзи флага), `registration_ip`, `last_login_ip`; у аккаунтов до миграции
  `0019` все пять полей `null`, админка показывает «—».
- `overview.countries` — топ-10 стран регистрации: `code`, `name`, `users`;
  одна группирующая выборка по `country_code` (имя — `max(country_name)`,
  чтобы переименование у провайдера не раздваивало страну), сортировка
  `users desc, code asc`. Поле аддитивное, пустой список — стран ещё нет.
- Страна определяется сервером в фоне по IP регистрации/входа через
  бесплатный `https://ipwho.is` (без ключа, таймаут 2 с, без ретраев,
  успешный ответ кэшируется на 24 ч; неудача не кэшируется и повторяется
  при следующем входе). Регистрация/вход не ждут резолв и не падают из-за
  него. IP пишутся в `users.registration_ip`/`last_login_ip` (`INET`,
  только публичные адреса), полный IP не логируется. `GEOIP_ENABLED=false`
  отключает только резолв страны, IP по-прежнему сохраняются; `GEOIP_URL`
  и `GEOIP_TIMEOUT_SECONDS` переопределяют провайдера и таймаут.
- `DELETE /admin/users/{user_id}` — жёсткое удаление аккаунта (204). Себя
  удалить нельзя (400 `"You cannot delete your own account"`), неизвестный id —
  404, блокирующие связи — 409. Все FK на `users.id` (`game_sessions`,
  `scores` через них, `app_sessions`, `game_launches`, `email_auth_codes`,
  `competitions`) — `ON DELETE CASCADE`, отдельная миграция не нужна.
  В лог пишутся только id администратора и пользователя, без email/логина.
- `title` в `/admin/statistics/games` — название песни из самого свежего
  запуска, у которого есть `song_id`; иначе сам `game_key`.
- UI: вкладка «Статистика» в `/admin/` (`app/static/admin/admin-statistics.js`,
  разметка `#sectionStatistics` в `index.html`). Данные грузятся лениво при
  первом открытии вкладки и обновляются при повторном открытии/`Обновить`;
  заголовки таблицы сортируют по клику (▲/▼, серверные `sort`/`order`,
  `offset=0`), столбец Scores показывает число результатов и лучший счёт,
  столбец «Страна» — флаг и название страны регистрации (или «—»), рядом с
  поиском — фильтр по стране из `overview.countries`; в `title` ячейки видны
  `registration_ip`/`last_login_ip`. Кнопка-корзина удаляет игрока через
  `DELETE /admin/users/{id}` после
  `confirm()` с логином и почтой; своя строка — disabled.
  Токен — общий админский из `sessionStorage` (`guitarbattle.admin.token`),
  при 401/403 вызывается `window.FFAdmin.handleUnauthorized()` (общий
  логин-экран SPA). Отдельного `/statistics` и `statistics.html` больше нет.

## Регистрация и восстановление пароля

1. `POST /auth/register` создаёт пользователя с `email_verified=false`, генерирует
   6-значный код и отправляет письмо. Если SMTP не настроен в production,
   возвращается 503 и пользователь не создаётся.
2. `POST /auth/verify-email` проверяет код, ставит `email_verified=true` и сразу
   возвращает `access_token`. Повторная проверка уже подтверждённой почты — 400.
3. `POST /auth/login` для неподтверждённой почты возвращает 403 с
   `detail="Email address is not verified"`; клиент предлагает `VERIFY EMAIL`.
4. `POST /auth/resend-verification` и `POST /auth/forgot-password` всегда
   возвращают 202, чтобы не раскрывать наличие аккаунта. Повторная отправка
   ограничена cooldown (по умолчанию 60 секунд).
5. `POST /auth/reset-password` проверяет код, меняет пароль, подтверждает почту,
   возвращает новый `access_token` и отзывает все refresh-сессии. Старые JWT
   инвалидируются через `users.token_version`.

### Согласие с политикой и постоянная сессия (миграция `0014_privacy_refresh`)

- `privacy_consent_at` (`timestamptz`, nullable) заполняется **только** при
  регистрации; существующие аккаунты остаются `NULL`. Логин согласие не требует и
  не меняет. Сайт передаёт `privacy_consent: true` из обязательного чекбокса формы
  регистрации; чекбокса в форме входа нет.
- `POST /auth/login` (а также `verify-email`/`reset-password`) создаёт
  refresh-сессию на 30 дней и возвращает `refresh_token` + `refresh_expires_in`
  для настольного клиента и ставит HttpOnly cookie `ff_refresh`
  (`path=/api/v1/auth`, `samesite=lax`, `secure` при HTTPS/`X-Forwarded-Proto`).
- `POST /auth/refresh` ротирует сессию: старый токен отзывается
  (`revoked_at`, `replaced_by_id`), выдаётся новый. Повторное использование
  отозванного токена — 401 и отзыв всей цепочки (защита от кражи); окно 15 с
  после ротации считается гонкой двух вкладок и цепочку не убивает.
- В БД хранится только `sha256` токена. Токены и коды не логировать.

Коды хранятся только как HMAC-SHA256, живут 15 минут, одноразовые, максимум
5 неверных попыток. Существующие аккаунты миграция `0005` помечает
подтверждёнными, чтобы не заблокировать игроков при выкатке.

## Версии контента и кэш ассетов

- `Song.version` — целочисленная **версия контента/кэша**, а не версия файла. Она
  возвращается в `GET /songs/{slug}` (и в админских ответах песни).
- `version` увеличивается на 1 при `PATCH /admin/songs/{id}`, при каждой успешной
  загрузке MIDI (`/files/midi`) или MP3-стема (`/files/audio/{role}`) — в той же
  транзакции, что и создание `SongFile`, а также при
  `POST /admin/songs/{id}/files/{file_id}/activate`.
- **Preview Song не участвует в версии игровых ассетов.** Загрузка, замена,
  активация архивной версии и удаление `role=preview` не меняют `Song.version`:
  иначе клиент перекачал бы неизменившиеся MIDI и стемы. Это единственное
  исключение из правила «новая загрузка роли увеличивает version».
- **Background Music** — глобальный MP3 приложения, не ассет песни: таблица
  `app_audio_assets` (миграция `0013_app_audio`, одна строка на `role`,
  сейчас `background_music`), каталог `APP_AUDIO_DIR` (`/data/app_audio`,
  volume `app_audio_data`). Публичный `url` — стабильный маршрут
  `/api/v1/app/background-music/file`, поэтому замена файла не оставляет
  битых ссылок; старый файл удаляется после коммита новой строки.
- **Preview Song** хранится в `song_files` как `role=preview`,
  `content_type=audio/mpeg`; правила загрузки — как у стемов (только MP3,
  лимит 100 МиБ, сигнатура `ID3`/MPEG sync, замена = новый `is_active`,
  прежние версии в архиве). В публичных `GET /songs` и `GET /songs/{slug}`
  есть аддитивное поле `preview_url` — абсолютный URL
  (`http://host/api/v1/songs/{slug}/files/{file_id}/download`) или `null`.
  Preview отсутствует в `files[]`/`assets` публичного детального ответа и не
  влияет на `readiness`, `publish_ready` и публикацию.
- `song_files.is_active` (миграция `0011_asset_versions`) — признак опубликованной
  версии файла. У каждой пары `(song_id, role)` активен **ровно один** файл;
  остальные загрузки хранятся как архив, доступны для скачивания по id и могут
  быть возвращены в публикацию эндпоинтом `activate`.
- Новая загрузка автоматически становится активной, прежняя версия этой роли
  архивируется в той же транзакции (правило «новый файл побеждает» без
  множественных активных версий).
- `GET /songs/{slug}` отдаёт только активный файл каждой роли; у файлов есть
  `role`, `size_bytes`, `sha256`, `download_url`.
- Миграция `0011` выполнила разовый откат: все файлы, загруженные 27.10.2026
  начиная с 19:00 UTC (ошибочные короткие сеты «Придумай светлый мир» и
  «Metal song»), переведены в архив, активными снова стали самые ранние версии
  каждой роли (First song: midi 7223 байта от 21.10 09:46, стемы 960722 байта
  от 21.10 14:43; Metal: midi 6912, стемы 11480293 от 22.10 08:05), а
  `songs.version` обеих песен увеличен на 1, чтобы клиенты перекачали ассеты.
- Клиент (`loadFirstOnlineCatalogSong`) хранит `version.txt` в
  `%APPDATA%/GuitarBattle/OnlineSongs/<slug>/`. Если версия на сервере > 0 и совпадает
  с `version.txt`, а MIDI (с заголовком `MThd`) и все опубликованные стемы существуют с
  ожидаемым `size_bytes`, скачивание пропускается. Иначе скачиваются MIDI и все стемы,
  а `version.txt` пишется только после полного успеха. При `version <= 0` клиент всегда
  скачивает файлы и версию не пишет.

### Размер такта (миграция `0020_song_time_signature`)

- `songs.time_signature_numerator` (1–32) и `songs.time_signature_denominator`
  (1, 2, 4, 8, 16, 32) — `SMALLINT NOT NULL DEFAULT 4`; существующие песни
  становятся `4/4` без отдельного UPDATE. CHECK-ограничения
  `ck_songs_time_signature_numerator` / `ck_songs_time_signature_denominator`
  дублируют правила Pydantic.
- Клиент использует размер для метронома и тактовых линий: `7/8` — семь
  восьмых кликов в такте, `3/4` — три четвертных. Поля аддитивные: записи и
  ответы без них читаются как `4/4` (дефолт в `SongSummaryResponse`,
  `AdminSongResponse`).
- Админка (`app/static/admin/index.html`, `admin.js`): в форме песни после BPM
  — «Доли в такте» (`songTimeSignatureNumerator`, number 1–32, по умолчанию 4)
  и «Размер (знаменатель)» (`songTimeSignatureDenominator`, select
  1/2/4/8/16/32, по умолчанию 4 — четверть). В таблице песен колонка «Размер»
  (`7/8`, скрыта до `lg`), в карточке файлов `7/8` выводится рядом с BPM/LVL.
- Ошибки: numerator вне 1–32 и denominator вне набора — 422
  (`Time signature denominator must be one of 1, 2, 4, 8, 16, 32`);
  `PATCH` с `null` для любого из полей — 422, как у `bpm` (NOT NULL).

## Обратная связь (feedback)

- `POST /feedback` — публичный, `Authorization` не требуется. Если токен
  передан, из него берутся `username` (fallback, если клиент не прислал) и
  email аккаунта для `Reply-To`; битый или истёкший токен трактуется как
  анонимный запрос (`get_optional_current_user`), а не как 401.
- Поля: `message` (строка, 1–4000 символов, обязательна; строка из одних
  пробелов — 422), `username` (≤64), `app_version` (≤64), `attachment_name`
  (имя файла, ≤120), `attachment_data` (base64 **сырых байтов** файла).
  Поля `email` больше нет: старый клиент, который его пришлёт, получит ignore,
  и адрес из тела никогда не станет `Reply-To`. Неизвестные поля игнорируются.
- Вложение опционально: запрос валиден без `attachment_data` и/или
  `attachment_name`. Если данные есть, а имени нет — используется
  `attachment.bin`. Имя очищается от каталогов (`../../x.txt` → `x.txt`) и
  непечатаемых/запрещённых символов, обрезается до 120 символов.
- Лимит вложения — **5 МиБ после декодирования** (`5 * 1024 * 1024` байт).
  Превышение и неверный base64 — **422** (не 413), чтобы контракт ошибок был
  единым: клиент показывает текст `detail` и не повторяет запрос.
- Ответ: `200 {"ok": true}` — письмо принято транспортом. `503
  {"ok": false, "detail": "mail is not configured"}` — `SMTP_HOST` пуст
  (сервис не вызывается, чтобы тело не попало в лог development-режима).
  `503 {"ok": false, "detail": "mail delivery failed"}` — SMTP отклонил
  письмо. `422` — пустое или слишком длинное сообщение, неверный base64 или
  вложение больше 5 МиБ. Остальные метаданные не отклоняют запрос: они
  обрезаются до разумной длины.
- Получатель — `FEEDBACK_EMAIL_TO` (по умолчанию `chumakevgeny@gmail.com`,
  задаётся в `docker-compose.yml` и `.env`). Тема — `FretFight feedback` или
  `FretFight feedback from <username>`. Тело: текст сообщения, затем
  `Username`, `Email`, `App version` (`(not provided)` для пустых).
  `Reply-To` ставится только для авторизованного запроса — email пользователя
  из БД; у анонимного письма reply-адреса нет.
- Вложение добавляется как MIME-часть (`EmailMessage.add_attachment`,
  MIME-тип по расширению через `mimetypes`, иначе
  `application/octet-stream`). Поддержка — в `send_email(...,
  reply_to=..., attachments=[(filename, bytes)])`.
- Отправка идёт через `app/services/email_service.py` (`send_feedback_email`
  → `send_email`), те же `SMTP_*`, что и письма подтверждения. Тело письма,
  имя вложения, username и адрес игрока не логируются; в лог попадает только
  факт «принято к отправке» / «не доставлено». В БД ничего не сохраняется.
- Тесты: `tests/test_feedback.py` (SMTP-транспорт замокан, реальная почта не
  отправляется; проверены вложение, санитизация имени, fallback имени,
  неверный base64, граница 5 МиБ, Reply-To по токену). Без `SMTP_*` в
  production эндпоинт отвечает 503 — проверяйте `log_email_configuration()` в
  `docker compose logs api`.

## Ассеты и готовность песни

- `GET /songs/{slug}` отдаёт единый объект:
  - `assets.midi` — `SongFileResponse | null`;
  - `assets.stems` — объект с ключами `drums`, `bass`, `guitar`, `others`, каждый
    `SongFileResponse | null`;
  - `files[]` сохранён для обратной совместимости и содержит те же самые новые файлы;
  - `readiness` — `state` (`empty`, `midi_only`, `partial`, `complete`),
    `available_roles`, `missing_roles`, `has_midi`, `has_all_stems`,
    `is_playable`, `is_complete`, `publish_ready`.
- `GET /songs` отдаёт `readiness` в каждом элементе списка, но без `assets` —
  ассеты запрашиваются в деталях.
- `is_playable` требует только MIDI: MIDI-only песня остаётся рабочей.
  `is_complete` и `publish_ready` требуют MIDI + все четыре MP3-стема.
- Админские `GET /admin/songs` и `GET /admin/songs/{id}` содержат тот же `readiness`;
  детальный ответ дополнительно содержит `assets` (только активные файлы, с
  админскими `download_url`) и `files[]` — все версии всех ролей с `is_active`,
  `size_bytes` и `created_at`, новые сверху. `GET /admin/songs/{id}/files` отдаёт
  тот же список отдельным запросом. Публикация не блокируется при неполном
  наборе стемов, чтобы не ломать MIDI-only песни; администратор видит
  `publish_ready` и `missing_roles` до публикации.
- Админка (`admin.js`, модалка «Файлы песни»): в каждом слоте перечислены все
  версии; активная помечена бейджем «Активен», архивные — «Архив» с размером и
  датой; у архивных есть кнопка «Сделать активной»
  (`POST /admin/songs/{id}/files/{file_id}/activate`), у всех версий — скачивание.
  `404` от `/files/{file_id}/analysis` — нормальное состояние «анализ ещё не
  сохранён»: панель показывает «Анализ не найден» и кнопку «Разобрать»
  (`POST .../analyze`), без красного тоста при открытии песни.

## Клиентские ассеты (метроном и сэмплы)

Клиент больше не читает `GuitarBattle/Sounds` с диска: метрономные клики и
MIDI-банки гитары/баса/фортепиано/барабанов скачиваются с сервера одним
версионированным zip-архивом. Оба эндпоинта публичные, без `Authorization`.

- `GET /client-assets/manifest` отдаёт JSON:
  - `schema` — целое, сейчас `1`;
  - `version` — SHA-256 архива в lowercase hex (он же `archive_sha256`);
  - `archive_url` — путь от `serverOrigin`, сейчас
    `/api/v1/client-assets/bundle.zip`;
  - `archive_size_bytes`, `archive_sha256`;
  - `files[]` — `{path, size_bytes, sha256}` для каждого файла в архиве.
- `GET /client-assets/bundle.zip` отдаёт `application/zip` с именем
  `guitarbattle-sounds.zip`; `HEAD` поддерживается и возвращает `Content-Length`.
- Оба ответа идут с `Cache-Control: no-cache`. Если `manifest.json` или
  `bundle.zip` нет в `CLIENT_ASSETS_DIR`, сервер возвращает 503 с `detail`
  (`Client asset manifest is not available` / `Client asset bundle is not available`).
  Manifest-эндпоинт также проверяет наличие архива, чтобы клиент не начинал
  скачивание заведомо отсутствующего файла.
- Архив и манифест генерирует `GuitarBattleServer/scripts/build_client_assets.py`
  (не импортируется приложением). В Docker Compose каталог
  `/data/client_assets` — named volume `client_assets_data`; переменная
  окружения — `CLIENT_ASSETS_DIR`.
- Раскладка архива: `metronome/*.wav`, `guitar/` (SFZ + `samples/`),
  `drums/*.wav`, `bass/` (SFZ `FingerBassYR 20190930.sfz` + `samples/`),
  `piano/` (SFZ `UprightPianoKW-small-20190703.sfz` + `samples/`).
  `jlearman.SteelDrum-main` не включается.
- Zip детерминированный: пути отсортированы, timestamps фиксированы, поэтому
  SHA-256/`version` не меняется при неизменных исходниках.
- Клиент хранит `manifest.json` и `version.txt` в
  `%APPDATA%/GuitarBattle/ClientAssets/`. Если `version` совпадает с
  `version.txt` и все файлы на месте, архив не скачивается. `archive_url`
  резолвится от `serverOrigin` (`ApiClient::downloadToFile`), а не от `baseUrl`.

## Прогрессия, достижения и испытание дня

Реализация: `GuitarBattleServer/app/services/progression.py` (формулы и правила),
`app/services/achievement_catalog.py` (каталог), `app/api/v1/progression.py`
(эндпоинты), миграция `0008_progression`.

### XP и уровни

- `XP_REQ(L) = round(50 * L ** 1.5)` — стоимость шага `L -> L+1`; округление
  half-up. `level_for_xp(xp)` — максимальный `L`, для которого сумма
  `XP_REQ(1..L-1)` не превышает `xp`. `level_progress(xp)` даёт
  `level`, `xp_total`, `xp_into_level`, `xp_for_next`, `xp_remaining`.
- XP за попытку: `round(100 * score * difficulty * tempo * content * repeat)`,
  clamp `[3, 600]`, `0` при completion < 50 % или исчерпанном слоте повтора.
  - `score` = accuracy/100 (максимальный счёт песни серверу неизвестен, поэтому
    нормализация только по точности; `score_points` для XP не используется);
  - `difficulty`: 1→0.8, 2→1.0, 3→1.2, 4→1.35, 5→1.5, >5→min(1.8, 1.5+0.05·(d−5)), <1→0.6;
  - `tempo` = clamp(0.90, 1.15, 0.90 + (bpm−60)/120·0.25) — не более +15 %;
  - `content` = clamp(0.6, 1.4, sqrt(duration/90)) · completion/100;
  - `repeat` по числу уже завершённых **обычных** попыток этой песни за текущие
    сутки UTC: 1.0, 0.5, 0.25, 0.0 (4-я и далее — 0 XP);
  - достижения дают фиксированный бонус и под repeat никогда не умножаются.
- Мастерство песни: `clamp(0, 100, best_accuracy + (difficulty−1)·2)`;
  ступени `<60 Novice`, `<75 Learner`, `<87 Guitarist`, `<95 Pro`, `≥95 Master`.
  Жанровое мастерство — среднее по личным рекордам песен жанра.

### Валидация попытки

- `completion_percent` принимается 0–100, но урезается до
  `100 * notes_played / notes_total`, если переданы оба счётчика; `notes_played >
  notes_total` → 422. Completion никогда не повышается счётчиками.
- Повторный `finish` той же сессии → 409, XP не начисляется (`xp_awards.session_id`
  уникален, сессия блокируется `SELECT ... FOR UPDATE`).
- `personal_best_updated` = создан или улучшен рекорд (больше score, при равном
  score — выше accuracy). `attempts` растёт всегда.

### Ответ `POST /games/sessions/{id}/finish`

Поля score (`id`, `session_id`, `score_points`, `accuracy_percent`, `max_combo`,
`perfect_count`, `good_count`, `miss_count`, `completion_percent`,
`avg_timing_error_ms`, `notes_total`, `notes_played`, `created_at`) плюс плоско:
`xp_earned`, `xp_total`, `level`, `level_up`, `xp_to_next`, `xp_remaining`,
`personal_best_updated`, `mastery {tier, value, next_threshold}`,
`achievements_awarded [{code, title, xp_bonus}]`.
`xp_to_next` и `xp_remaining` — одно и то же значение (остаток до следующего
уровня), оставлено обоими именами для совместимости.

### Достижения

- Каталог в БД (`achievements`, 27 записей, сидируется миграцией `0008` и
  синхронизируется на старте приложения). Код условия — по `code`.
- Выдаются один раз (`user_achievements` unique `(user_id, code)`), бонус
  прибавляется к `users.xp_total` и пересчитывает `level`.
- Достижения-исполнения (`first_notes`, `first_personal_best`, `accuracy_*`,
  `clean_run`, `timing_40ms`, `improve_*`, `hard_80`, `songs_*`, `genres_*`,
  `mastery_*`, `genre_master`) проверяются только на попытках с completion ≥ 50 %.
- `timing_40ms` без `avg_timing_error_ms` остаётся заблокированным.
- `genres_3/5/8`: эффективный порог = `min(goal, число жанров в опубликованных
  песнях)`; при совпадении порогов выдаётся только первое достижение, остальные
  остаются заблокированными, а в `description` добавляется пометка
  `Unavailable: catalogue has N genres of M required.` (в БД — с ведущим
  пробелом, т.к. это суффикс). Жанры не выдумываются.
- Отображаемые `title`/`description` — только английские (ASCII), без кириллицы:
  API нейтрален к языку, клиенты показывают строку как есть. `code`, `goal`,
  `xp_bonus`, `category` и `icon` при смене текста не меняются; переименование
  подхватывается на старте через `sync_achievement_catalog`, миграция не нужна.
  `category` (`progress`/`songs`/`accuracy`/`rhythm`/`genres`/`mastery`/`daily`) —
  идентификатор для группировки в UI (First Steps, Accuracy and Timing, Library
  Explorer, Mastery, Daily Challenge), а не отображаемое имя.
- `daily_first_place` не может быть решён в момент попытки: он выдаётся при
  следующем обращении к `/profile/progression`, `/achievements` или
  `/daily/leaderboard`, когда день уже закончился (фонового планировщика нет).

### Иконки достижений

Миграция `0010_achievement_icons`: `achievements.icon` (`String(128)`, NOT NULL) —
имя файла из `app/static/media/achievements/`. Те же значения лежат в
`ACHIEVEMENT_CATALOG` и пересинхронизируются на старте, поэтому код — источник
истины, а миграция только заполняет существующие БД.

- SVG отдаются тем же FastAPI: `GET /media/achievements/<icon>` (публично, без
  токена, `image/svg+xml`, файлы в образе, без volume). Префикс — `/media`,
  как у `/media/landing`; наружу его должен проксировать тот же Caddy.
- `GET /achievements` и `GET /profile/users/{id}` отдают в каждом элементе
  достижения два новых поля: `icon` (имя файла, `str | None`) и `icon_url`
  (`/media/achievements/<icon>`, `str | None`). Остальные поля не менялись,
  `icon_url` относительный — склеивать с origin сайта.
- `achievements_awarded` в ответе `POST /games/sessions/{id}/finish` **не**
  менялся: `[{code, title, xp_bonus}]`, без иконок.
- 14 файлов: `first_notes.svg`, `notes.svg`, `metronome.svg`, `accuracy.svg`,
  `target.svg`, `star.svg`, `mastery.svg`, `daily.svg`, `medal.svg`,
  `diploma.svg`, `genres.svg`, `calendar.svg`, `clock.svg`, `hard.svg`.
- Лицензия иконок — Tilda Icons (free), обязательна атрибуция
  «Icons are provided by Tilda Publishing» со ссылкой на https://tilda.cc;
  она стоит в футере `/account`. Новые иконки качать только из наборов Tilda и
  не удалять атрибуцию.
- Веб-кабинет (`app/static/app/account.html` + `account.js`) рисует сетку
  «Достижения» из `GET /achievements` с тем же токеном: у открытых — иконка,
  название, описание, дата, XP; у закрытых — серая иконка, `progress/goal` и
  без даты. Без сети/без достижений блок показывает текстовое состояние и не
  ломает остальную страницу.

### Испытание дня (расписание по дням недели)

Миграция `0009_daily_slots`: `songs.daily_weekday` (smallint `NULL` или 0..6,
CHECK + индекс) и таблица `daily_slots` (`weekday` PK 0..6, `song_id`
`ON DELETE CASCADE`, `updated_at`). `0` — понедельник, `6` — воскресенье (UTC);
значение совпадает с `datetime.date.weekday()`.

- Песня дня берётся из слота текущего дня недели и при первом запросе за дату
  фиксируется в `daily_challenges` (`rules_version=1`). Уже созданная запись не
  перезаписывается: смена или очистка слота меняет только будущие дни. Слот,
  указывающий на черновик, для игроков выглядит как пустой.
- `/daily/today` без слота отвечает `200` с
  `{date, weekday, song: null, rules_version: 1, official_attempt_used: false,
  official_result: null, leaderboard_top: []}` — это не ошибка.
- `POST /games/sessions` c `daily: true` требует песню дня (иначе 409) и
  неиспользованную официальную попытку (иначе 409); если слот не задан или
  занят черновиком — тоже 409. Сессия получает `mode=daily`. Неопубликованная
  песня вообще не стартует (404 «Published song not found»).
- `finish` такой сессии вставляет `daily_results` (unique `date+user`) в той же
  транзакции; вторая официальная попытка → 409. Тренировки (`mode=normal`) в
  рейтинг дня не попадают и остаются доступными даже для песни дня.
- Рейтинг дня сортируется по `score_points` desc, затем `accuracy_percent` desc.
- Песни со слотом исключены из `GET /songs`, `GET /leaderboard/global` и
  `GET /games/best-scores`. `GET /songs/{slug}` и скачивание файлов продолжают
  работать: клиент запрашивает детали песни дня **без** токена, чтобы скачать
  MIDI и стемы. `GET /leaderboard` (по песне) и `song_best_scores` не менялись.
- Админ: `GET /admin/daily` (7 слотов) и `PUT /admin/daily/{weekday}`
  (`{song_id}` / `{song_id: null}`), только `is_admin`. Конфликты решаются
  переносом: песня уходит со старого дня, прежняя песня слота теряет
  `daily_weekday` и возвращается в обычный каталог (это не 409).
- `daily_weekday` есть в схемах `/admin/songs` (create/patch/list/detail) и
  синхронизируется с `daily_slots` в той же транзакции: установка поля
  назначает слот, `null` — снимает.
- Админка: вкладка «Daily Challenge» (`app/static/admin/admin-daily.js`,
  панель `#sectionDaily`), 7 строк с «Назначить»/«Очистить»; в форме песни —
  поле «Daily weekday»; в списке песен — бейдж «Daily · Пн».

### Админ-превью черновиков

Администратор (`users.is_admin`) может играть неопубликованную песню до
публикации. Контракт аддитивный: старый клиент не отправляет ни новых полей, ни
токена и видит ровно тот же публичный каталог; новый клиент против старого
сервера просто не получает `is_admin`/`is_published` и работает как раньше.

- `UserResponse.is_admin` (`bool`, default `false`) — отдаётся `/auth/register`,
  `/auth/me`, `PATCH /auth/me`. Клиент показывает тумблер `ADMIN` только при
  `true`; сервер всё равно перепроверяет флаг в БД на каждом эндпоинте.
- `GET /songs?include_drafts=1` — учитывается только вместе с валидным
  admin-JWT. Тогда снимается **только** фильтр `is_published`; `archived_at IS
  NULL` и `daily_weekday IS NULL` остаются. Без токена, с токеном не-админа или
  с битым токеном ответ прежний (публичный каталог, не 401).
- `SongSummaryResponse.is_published` (`bool`, default `true`) — у каждого
  элемента `/songs` и в деталях. Клиент рисует бейдж `DRAFT`.
- `GET /songs/{slug}` и `GET /songs/{slug}/files/{file_id}/download` —
  неопубликованную песню отдают только при валидном admin-JWT, иначе `404 Song
  not found`. **Архивная песня недоступна и админу** (у админки свои
  `/admin/songs/{id}` маршруты): `archived_at IS NOT NULL` → 404.
- `POST /games/sessions` для черновика: админ получает `201` с `mode="preview"`;
  не-админ — прежний `404 Published song not found`. `daily: true` для черновика
  — `404` (песня дня обязана быть опубликованной).
- `POST /games/sessions/{id}/finish` для `mode="preview"`: `Score` сохраняется
  (история), но `apply_attempt_progression` не вызывается — нет XP, ачивок,
  daily-результата, `song_best_scores` и записи в рейтинге. Ответ: `xp_earned=0`,
  `level_up=false`, `personal_best_updated=false`, `mastery=null`,
  `achievements_awarded=[]`, `xp_to_next=0`, `xp_remaining=0`, `xp_total`/`level`
  — текущие значения игрока.
- Preview-сессии исключены из рейтинга (`_best_score_per_song_subquery` и
  `/games/best-scores` фильтруют `mode == "normal"`), из `/leaderboard`
  (требует `Song.is_published`), из `/profile/personal-bests` (нет строки
  `song_best_scores`) и из статистики `player_stats` (`mode != "preview"`).
- Тесты: `tests/test_admin_preview.py`.

## Practice-раздел: `mode = "practice"` (без наград)

- `GameSessionCreate.practice` (`bool`, default `false`) — аддитивное поле.
  Приоритет режима в `POST /games/sessions`: `daily: true` + `practice: true`
  → **422** (`daily and practice cannot be combined`, проверка в Pydantic);
  неопубликованный черновик → `preview` при любом `practice`; опубликованная
  песня + `practice: true` → `practice`; `daily: true` → `daily`; иначе
  `normal`. Архивная/чужая песня — прежние 404.
- `finish` для `practice` обрабатывается ровно как `preview`: строка `scores`
  сохраняется (аудит), `apply_attempt_progression` не вызывается — нет XP,
  достижений, daily-результата, `song_best_scores` и рейтинга. Ответ:
  `xp_earned=0`, `xp_total`/`level` — текущие значения игрока, `level_up=false`,
  `xp_to_next=0`, `xp_remaining=0`, `personal_best_updated=false`,
  `mastery=null`, `achievements_awarded=[]`. Строка `users` не меняется.
- `practice` исключён из пользовательской статистики: `/games/history`
  (`mode != "practice"`), `/leaderboard` (`mode != "practice"`),
  `progression.player_stats` (не учитывает `preview` и `practice`),
  `/games/best-scores` и `/leaderboard/global` (только `normal`).
  Строки остаются в БД для аудита.
- `normal`, `daily` и `preview` ведут себя как раньше; `attempts_today_for_song`
  по-прежнему считает только `normal`, поэтому practice не съедает repeat-слоты.
- Тесты: `tests/test_practice_mode.py`.

## Где искать в клиенте

`Source/Network/ApiClient.h` — единственное место HTTP-запросов (`get`, `post`, `getJson`,
`postJson`, `download`, `downloadToFile`, `runAsync`). Запросы выполняются на
`juce::ThreadPool` внутри `ApiClient`, результат приходит в UI через
`juce::MessageManager::callAsync`; синхронных вызовов из обработчиков кнопок нет.
`get(path, timeout)`, `download(path, timeout)` и `downloadToFile(path, file,
timeout)` — анонимные перегрузки; вариант с `bearerToken` добавляет заголовок
`Authorization: Bearer <token>`. Пустой токен = прежнее поведение без заголовка.

`Source/Network/ClientAssets.h` — манифест, проверка кэша, скачивание и
распаковка `bundle.zip`; `MainComponent::refreshClientAssets` /
`reloadClientAssets` вызывают его с worker-потока и применяют результат в UI.

`Source/MainComponent.cpp`:

- `authenticateOnlineAccount` — вход и регистрация.
- `showEmailVerificationDialog` — ввод/повторная отправка кода подтверждения.
- `recoverOnlineAccountPassword` / `showPasswordResetDialog` — восстановление пароля.
- `finishOnlineSignIn` — загрузка `/auth/me` и переход в приложение.
- `startOnlineGameSession` — создание сессии.
- `submitOnlineGameResult` — отправка результата.
- `refreshGlobalLeaderboard` — глобальный рейтинг.
- `refreshOnlineCatalog` — список опубликованных песен на главном экране.
- `refreshOnlineBestScores` — лучшие счета пользователя по каждой песне (`/games/best-scores`).
- `refreshProgression` / `refreshAchievements` / `loadDailyChallenge` — прогрессия
  (`/profile/progression`, `/achievements`, `/daily/today`). Ответы приходят на
  worker-потоке, применяются в UI через `callAsync`.
- `startOnlineGameSession` с `daily=true` — официальная попытка дня; при 409
  показывать «официальная попытка уже использована», а не ошибку сети.
- `loadDailyChallenge` (`/daily/today`) — `song: null` значит «на этот день
  песня не назначена»; это нормальный ответ, кнопку официальной попытки нужно
  скрыть. Поле `weekday` — 0=Пн…6=Вс (UTC).
- `startSelectedSong` / `downloadOnlineSong` — выбор строки каталога и загрузка ассетов по требованию.
  В admin-превью запросы идут с `Authorization` (детали + каждый файл), иначе —
  анонимно, как раньше.
- Админ-превью: `onlineIsAdmin` (из `is_admin` в `/auth/me`), `adminPreviewMode`
  (тумблер `ADMIN` в шапке, ключ `AdminPreviewMode` в `appProperties`),
  `currentSessionIsPreview` (из `mode` ответа `/games/sessions`). Каталог в этом
  режиме запрашивается как `/songs?limit=100&offset=0&include_drafts=1` с
  токеном; строки с `is_published=false` помечаются бейджем `DRAFT`, а в игре и
  на экране результатов рисуется «PREVIEW - NOT RANKED».
- Practice-раздел: `currentSessionIsPractice` выставляется в
  `startSelectedSong()` для одиночного запуска (`!campaignFightActive &&
  !duelPending && !duelOnlinePlaybackPending`) и отправляется в
  `startOnlineGameSession()` как `practice`. Ответ `mode` уточняет флаг
  (`mode == "practice"`); старый сервер вернёт `normal`. После `finish`
  прогрессия не применяется, статус «Practice attempt saved. This result is not
  ranked.», на карточке — пилюля «PRACTICE - NOT RANKED», блок прогрессии
  скрыт, локальный лучший счёт (`saveBestScoreForSelectedSong`) не пишется.
  Study-режим (`practiceMode`) сессию не открывает вовсе.
- Блокировка песен по уровню удалена: `requiredPlayerLevelForSong`,
  `isSongUnlocked` и `refreshSongUnlockState` больше не существуют; START
  включается по факту выбора строки (`updateStartButtonState`). Не возвращайте
  проверку уровня в `startSelectedSong`/`startOnlineDuel`.
- Study (`practiceMode`, внутреннее имя): кнопка `STUDY: ON/OFF`, тултип
  «Study the song note by note. No score, no rating, no XP.», HUD
  «STUDY - PLAY THE HIGHLIGHTED NOTE», пилюля «STUDY - NO SCORE».
- Телеметрия активности (`/activity/heartbeat`, `/activity/game-launch`) — из
  worker-потока, без блокировки UI-потока.
- Поиск по `apiClient.`, `ApiClient::get`, `ApiClient::post`.

## Чек-лист изменения API

1. Обновить Pydantic-схему в `app/schemas/`.
2. Обновить/добавить роутер в `app/api/v1/`.
3. Зарегистрировать роутер в `app/api/v1/router.py`.
4. Добавить миграцию, если меняется БД.
5. Обновить путь, JSON-поля и заголовки в `MainComponent.cpp`.
6. Обновить обработку ошибок: 400/401/403/404/409/422.
7. Обновить эту таблицу.
8. Проверить `curl.exe` и собрать клиент.

## Ловушки

- `good_count` на клиенте — сумма great + good; не переименовывать без клиента.
- `speed_percent` ограничен 50–120.
- `accuracy_percent` — 0–100, не 0–1.
- `song_id` — UUID, `slug` — строка.
- `client_session_id` в телеметрии — UUID запуска клиента (≤64 символа),
  не путать с `app_session_id`/`session_id` на сервере.
- Время в приложении считает сервер; клиент не отправляет длительность сессии.
- `access_token` живёт только в памяти; при 401 клиент один раз пробует
  `/auth/refresh` и повторяет запрос. Refresh-токен — в HttpOnly cookie, в JS и
  localStorage его нет.
- `privacy_consent` обязателен только при регистрации; `422` с
  `Privacy policy consent is required` — отсутствующее согласие, а не ошибка полей.
- Cookie `ff_refresh` ограничена `path=/api/v1/auth`; браузер не отправляет её в
  остальные запросы, поэтому access token по-прежнему передаётся в `Authorization`.
- При 403 на `/auth/login` почта не подтверждена; это не неверный пароль.
- После смены/сброса пароля старый JWT перестаёт работать (401) из-за `token_version`.
- Ошибки FastAPI — `{"detail": ...}`; клиент должен это учитывать.
- SMTP-переменные и коды никогда не логировать в production.
- Не логировать токены и пароли.
- `completion_percent` — проценты 0–100, не доля 0–1; `notes_played`/`notes_total`
  только понижают completion, поэтому клиент не может «дописать» прогресс.
- `daily: true` в `POST /games/sessions` — только для песни дня; 409 значит
  «слот не задан / занят черновиком», «не та песня» или «официальная попытка уже
  использована». Клиент должен различать 409 (нормальный отказ) и сетевую ошибку.
- Песня дня есть в `/songs/{slug}`, но отсутствует в `/songs`, `/leaderboard/global`,
  `/games/best-scores`. Не «исправляйте» это, скрывая детали по slug: клиент
  качает ассеты песни дня этим запросом без `Authorization`.
- `weekday` в `/daily/today` и `/admin/daily` — 0=Пн…6=Вс (UTC), а не ISO 1–7.
- `song` в `/daily/today` может быть `null`; это не ошибка и не 404.
- `is_admin` в `/auth/me` — только подсказка для UI. Нельзя полагаться на него
  как на разрешение: сервер проверяет `users.is_admin` в БД на каждом запросе, а
  `include_drafts`/детали/скачивание черновика без admin-JWT дают 404 или
  публичный ответ.
- Черновик (`mode="preview"`) не даёт XP, ачивок, личного рекорда и рейтинга.
  Не показывайте блок прогрессии по `finish` preview-сессии и не ждите строку в
  `/games/best-scores`.
- `practice: true` — только для Practice-раздела; сервер сам решает `mode` и
  никогда не начисляет награды. Нельзя подменять им `daily` (422) и нельзя
  рассчитывать на XP даже при прямом вызове API. Study-режим клиента сессию не
  создаёт.
- Уровень игрока больше не ограничивает доступ к песням: сервер не проверяет
  уровень при создании сессии, а клиент не рисует «REQUIRES LVL».
- Повторный `finish` (в том числе параллельный) даёт 409 и не начисляет XP —
  это нормальный ответ, а не сбой.
- `mastery` в ответе `finish` — объект, а `achievements_awarded` — массив объектов,
  а не строк.
- XP за повторные попытки падает (100/50/25/0 %), даже если рекорд растёт.
- Публичные `/songs` и `/songs/{slug}` отдают только файлы с `is_active = TRUE`.
  Не меняйте выборку обратно на «самый новый по `created_at`»: активную версию
  задаёт админ (`activate` или новая загрузка), архивные файлы намеренно скрыты
  из публичной выдачи, но остаются скачиваемыми по id.
- `404` от `GET /admin/songs/{id}/files/{file_id}/analysis` — это «MIDI ещё не
  разобран», а не сбой; клиент/админка должны предлагать `POST .../analyze`.
- `preview_url` — **абсолютный** URL (в отличие от относительных `download_url`
  и `icon_url`); `null`, если preview не загружен. Preview не входит в
  `files[]` публичного ответа, не считается в `readiness` и не требуется для
  публикации — не добавляйте его в `ASSET_ROLES`.
- `GET /app/background-music` всегда 200; отсутствие трека — `url=null`,
  `size_bytes=0`, это не 404. Сам MP3 — `GET /app/background-music/file`;
  URL стабилен при замене, поэтому не кэшируйте его содержимое без revalidate.

## Дуэли (`/duels`)

Полный контракт — `GuitarBattleServer/docs/DUELS_API.md`; схемы —
`app/schemas/duels.py`. Клиент: `Source/Network/DuelClient.{h,cpp}`,
флоу и HUD — `Source/Gameplay/MainComponentDuel.cpp`.

| Метод | Путь | Тело / параметры | Ответ |
|---|---|---|---|
| POST | `/duels` | `song_id` (UUID), `mode=bot|online`, `bot_level` только для `bot` | 201 `DuelMatchState` |
| POST | `/duels/{id}/join` | — | `DuelMatchState`; идемпотентно для участника |
| POST | `/duels/{id}/ready` | — | `DuelMatchState`; `409 no_opponent` после 30 с |
| GET | `/duels/{id}` | Bearer | `DuelMatchState` (REST всегда содержит `your_side`) |
| POST | `/duels/{id}/events` | `{"events":[{"note_index":int,"quality":"perfect|great|good|miss","client_time_ms?":int}]}`, 1–100 | `match`, `accepted[]`, `duplicates[]`, `rejected[]`, `exchanges[]`, `finished` |
| POST | `/duels/{id}/finish` | — | `match`, `already_finished`, `waiting_for_opponent` |
| POST | `/duels/{id}/leave` | — | `match`; идемпотентно |
| GET | `/duels/{id}/result` | Bearer | `DuelMatchState`; `409 match_not_finished` |
| GET | `/duels/history` | `limit`, `offset`, `mode?`, `result?` | `items[]`, `total`, `limit`, `offset` |
| GET | `/duels/leaderboard` | `period=week|month|all`, `limit`, `offset` | `items[]`, `total` |
| GET | `/duels/me` | Bearer | профиль + `active_match` |
| GET | `/duels/players/{user_id}` | Bearer | публичный профиль |
| WS | `/duels/{id}/ws?token=` | — | `connected/join/ready/countdown/start/note/block/hp/state/finish/cancelled/ping` |
| POST | `/duels/lobbies` | `name` (1–40, trim), `password?` (≤128; пусто = нет пароля) | 201 `DuelRoomState`; песни ещё нет |
| GET | `/duels/lobbies` | `include_full=true` (по умолчанию `false`) | `items[]`, `total`, `include_full`; только `waiting`/`playing`, максимум 50 |
| GET | `/duels/lobbies/{lobby_id}` | Bearer, только участники | `DuelRoomState`; `403 not_participant` |
| POST | `/duels/lobbies/{lobby_id}/join` | `password?` (тело можно не слать) | `DuelRoomState`; `403 password_required/invalid_password`, `409 lobby_full/lobby_closed/lobby_not_waiting`; повтор участника идемпотентен |
| POST | `/duels/lobbies/{lobby_id}/leave` | — | `DuelRoomState`; гость освобождает место, хост закрывает комнату |
| POST | `/duels/lobbies/{lobby_id}/song` | `song_id` (UUID опубликованной песни) | `DuelRoomState`; только хост (`403 not_host`), только `waiting`; `422 song_not_published` |
| POST | `/duels/lobbies/{lobby_id}/start` | — | `{match_id, lobby}`; только хост; `409 opponent_missing/song_not_selected/lobby_not_waiting`; **legacy** (клиенты ≤ 1.3.x): матч сразу `playing`, отсчёт идёт |
| POST | `/duels/lobbies/{lobby_id}/start/request` | — | `DuelRoomState`; только хост; `409 opponent_missing/song_not_selected/lobby_not_waiting`; появляется `start` в комнате, статус остаётся `waiting` |
| POST | `/duels/lobbies/{lobby_id}/start/confirm` | — | `{match_id, lobby}`; участник; один подтвердил — `match_id: null`; оба — матч `waiting` создан, комната `playing`; `409 start_not_requested` |
| POST | `/duels/lobbies/{lobby_id}/start/decline` | — | `DuelRoomState`; участник; `start` очищается, комната `waiting`; идемпотентно |
| GET | `/duels/lobbies/{lobby_id}/chat` | `after_id` (≥0), `limit` (1–200) | `{items, latest_id}`; `after_id=0` — последние 200 |
| POST | `/duels/lobbies/{lobby_id}/chat` | `message` (1–500) | 201 `DuelChatMessage`; `422 empty_message`, `429 rate_limited` |
| POST | `/voice/lobbies/{lobby_id}/chunks` | `{"data": "<base64 PCM>"}`, ≤6400 байт после декодирования | 200 `{"seq": int}`; `422` на битый base64/пусто/перебор, `429` **строкой** `{"detail": "voice rate limit"}` |
| GET | `/voice/lobbies/{lobby_id}/chunks` | `since` (≥0, по умолчанию 0) | `{"chunks":[{"seq","user_id","username","data"}], "next_since": int}`, только чужие куски, по возрастанию `seq`, максимум 20 |
| DELETE | `/voice/lobbies/{lobby_id}/chunks` | — | 204 без тела; очищает только свои буферы; идемпотентно |
| POST | `/voice/lobbies/{lobby_id}/endpoint` | `{"port": 49152}` — **целый** 1..65535 (`strict`; `"49152"`/`49152.0` → 422); `ip`/`public_ip` в теле игнорируются | 200 `{ip, port, peer}` — `ip`/`port` вызывающего (IP выводит сервер из X-Forwarded-For/сокета), `peer` — эндпоинт **другого** участника `{ip, port}` или `null` (не публиковал / истёк / нет гостя); TTL 30 с; `400 client_ip_unavailable`, если публичный IP не определить |
| POST | `/duels/{id}/rematch` | Bearer, только участники | `match_id`, `previous_match_id`, `lobby_id`, `song`, `already_created`; песня берётся из **текущей** `song_id` комнаты (хост мог выбрать другую после `return-to-lobby`), иначе из матча |
| POST | `/duels/{id}/return-to-lobby` | Bearer, только участники; матч `finished`/`cancelled` | `DuelRoomState`; лобби → `waiting`, песня и чат сохранены |

- Сервер **не принимает** HP, урон, счёт и победителя. Клиент отправляет только
  `(note_index, quality)`; HP/счётчики/XP/рейтинг читаются из `DuelMatchState`.
- `quality` — строка `perfect`/`great`/`good`/`miss` (не enum и не числа).
- `note_index` — индекс в **замороженном** чарте матча (`note_count` из создания).
  Клиент сверяет `note_count` с `gameSession.getMidiNotes().size()` и при
  расхождении отменяет онлайн-матч и предлагает бота.
- Повтор `(match, user, note_index)` не применяется второй раз и попадает в
  `duplicates`; обмены разрешаются строго по возрастанию `note_index`.
- Онлайн-матч играется на 100% скорости: времена нот заморожены в исходном темпе.
- `battle rating` (Elo) меняется только в `mode=online` человек-человек;
  бот-матч и ничья не дают рейтинга, XP начисляется.
- `finish`/`leave` без единого отправленного события — техническое поражение и 0 XP.
- 401 — access token истёк: клиент просит войти заново (WS закрывается кодом 4401).
- `juce::WebSocket` в JUCE 9.0.2 отсутствует, поэтому клиент опрашивает
  `GET /duels/{id}` примерно раз в 900 мс; при обрыве polling продолжается
  (сервер не форсирует поражение для REST-клиентов, в отличие от WS-таймаута 30 с).
- `DuelMatchState` аддитивно содержит выбранных персонажей:
  `you_character_name`, `you_character_sheet_url`, `opponent_character_name`,
  `opponent_character_sheet_url` (`str | null`). Значения читаются из текущего
  `users.character_id` в момент ответа (не снимок матча); `you_*` соответствуют
  `your_side`. В широковещательном WS-`state` (`your_side = null`) все четыре
  поля — `null`; персональный снимок приходит в `connected` и в REST.
- У бота и у игрока без выбора поля персонажа — `null`. Имена и URL — те же,
  что в `GET /characters`; URL origin-relative, клиент склеивает с `serverOrigin`.
- `match_id: null` в ответе `/duels/lobbies/{id}/start/confirm` — нормальный
  ответ «подтвердил только один», а не ошибка; матч появится в следующем
  опросе комнаты. `409 start_not_requested` — подтверждение без запроса хоста.

### Общие лобби (комнаты с чатом)

Отдельная сущность `duel_lobbies` (миграция `0024_duel_lobbies`), **не** статус
матча. Поток: создать комнату без песни → гость входит по паролю → чат → хост
выбирает песню → хост стартует матч → после матча `rematch` или
`return-to-lobby`.

- `DuelRoomState`: `lobby_id`, `name`, `has_password`, `status`
  (`waiting`/`playing`/`closed`), `host_user_id`, `guest_user_id` (`str | null`),
  `song` (`DuelSongInfo | null`), `match_id` (последний матч комнаты, `str | null`),
  `players[]` (`user_id`, `username`, `display_name`, `is_host`),
  `start` (`{requested_by, host_confirmed, guest_confirmed} | null`, аддитивно),
  `created_at`, `updated_at`. Пароль и хеш в ответ никогда не попадают — только
  `has_password`.
- Старт с подтверждением (клиенты 1.4.0+): `start/request` (хост) открывает
  `start`; оба игрока `start/confirm`; матч создаётся только когда оба флага
  `true` — при этом `status="waiting"`, `started_at=null`, `ready_at=null`
  у обоих. Дальше клиенты скачивают песню и шлют `/ready`, общий отсчёт 3 с
  стартует на сервере. `start/decline` любого сбрасывает `start`. Отмена
  матча из комнаты (`/leave` при `waiting`) возвращает комнату в `waiting`
  и очищает `start`. `POST .../start` без `/request` — прежний немедленный
  старт для старых клиентов.
- `waiting_expired` у матча из комнаты всегда `false`: 30-секундный таймаут
  очереди не должен предлагать бота во время скачивания.
- `DuelChatMessage`: `id` (`int64`, монотонный), `user_id`, `username`,
  `display_name`, `text`, `created_at`. `after_id` — это предыдущий `latest_id`;
  `latest_id` в ответе — максимальный id выдачи.
- В комнате ровно два человека (`host_user_id` + `guest_user_id`, CHECK
  «гость ≠ хост»); третий получает `409 lobby_full`.
- Пароль хранится Argon2-хешем (`app/services/passwords.py`). Неверный или
  отсутствующий пароль — `403`; полная комната — `409` (проверяется раньше
  пароля); повторный вход участника — 200 с тем же состоянием.
- Читать комнату и чат могут только участники (`403 not_participant`);
  выбирать песню и стартовать — только хост (`403 not_host`).
- Чат очищается на сервере: HTML-теги, `<`/`>`, управляющие символы удаляются,
  пробелы схлопываются, лимит 500; пустой после очистки текст — `422`.
- Лимиты: создание комнаты burst 6 / 0.5 в секунду; чат burst 12 / 2 в секунду
  на пару `(lobby, user)`; `429 rate_limited`.
- `start` создаёт обычный `duel_matches` (`mode=online`, `status=playing`,
  стандартный отсчёт) и связывает его `duel_matches.lobby_id`. `DuelMatchState`
  аддитивно содержит `lobby_id` (`null` для бота и быстрой очереди).
- `rematch` не трогает прошлый матч (результат, XP, Elo, события сохраняются) и
  создаёт новый для тех же двух игроков; песня — **текущая** `song_id` комнаты
  (хост мог выбрать другую после `return-to-lobby`), только если комнате песня
  ещё не назначена — `song_id` самого матча. Повторный вызов возвращает уже
  созданный матч с `already_created=true`.
- `return-to-lobby` требует `finished`/`cancelled`, переводит комнату в
  `waiting`, песня и чат сохраняются; после этого `start` работает сразу.
  `select_song` меняет песню только в `waiting` (после возврата это как раз так)
  и сбрасывает незавершённый `start`, поэтому подтверждение всегда относится
  к той песне, которую оба видели: `start/confirm` создаёт матч из
  `lobby.song_id` на момент подтверждения (`_create_lobby_match`).
- Роутер `/duels/lobbies` подключается **до** `/duels`, иначе
  `GET /duels/lobbies` перехватывается `GET /duels/{match_id}`.
- Клиент: `DuelRoomSnapshot`, `DuelRoomStartState` (поле `start`), `DuelRoomPlayer`,
  `DuelRoomListReply`, `DuelChatMessage`, `DuelChatHistoryReply`,
  `DuelRoomStartReply` (`matchId` пустой, пока второй не подтвердил),
  `DuelRematchReply`; методы `createRoom`, `rooms`, `room`, `joinRoom`,
  `leaveRoom`, `selectRoomSong`, `startRoomMatch` (legacy), `requestLobbyStart`,
  `confirmLobbyStart`, `declineLobbyStart`, `roomChat`, `sendRoomChat`,
  `rematch`, `returnToLobby`. `room()`/`roomChat()` — идемпотентные GET с
  ретраем, годятся для `juce::Timer`; все ответы приходят в message thread.
  `rematch` в клиенте 1.8.1+ **не вызывается**: на экране результата нет рематча
  (метод оставлен для совместимости серверного контракта).
- UI комнаты (`Source/UI/OnlineDuelRoom.*`): START GAME → `start/request`,
  модальный диалог «Start the duel?» у обоих, CONFIRM → `start/confirm`,
  DECLINE → `start/decline`; при исчезновении `start` диалог закрывается с
  сообщением. Клиент 1.8.1: опрос комнаты нумеруется, ответы, отправленные до
  более нового запроса или до собственного подтверждения, отбрасываются
  (`roomRequestSeq`/`appliedRoomSeq`), а подтверждение запоминается по
  идентичности раунда `requested_by|song_id`. Поэтому ровно одно подтверждение
  на раунд: диалог не переоткрывается ни устаревшим опросом, ни после
  `start/confirm`.
- Матч из комнаты больше не показывает второй диалог после скачивания: клиент
  сам шлёт `/ready`. Признак комнаты — `duelOnlineLobbyId` **или** флаг
  `duelOnlineConfirmedInRoom` (ставится комнатой и любым `lobby_id` в снапшоте):
  очередь подтверждается диалогом, комната — нет, даже если id комнаты потерялся
  при передаче состояния. `promptOnlineDuelReady()` для такого матча сразу
  отправляет `ready`.
- Экран результата онлайн-матча из комнаты: ровно две кнопки — **GO TO LOBBY**
  (`return-to-lobby`, та же комната с чатом и песней) и **LEAVE LOBBY**
  (`/duels/lobbies/{lobby_id}/leave`, затем очистка состояния и переход в раздел
  Online со списком комнат). 404/409 при выходе не мешают навигации.
  Кампания/Battle сохраняют свои кнопки, подсчёт очков и рейтинг не менялись.
- Завершённый матч нельзя переиспользовать: клиент помнит
  `duelOnlineFinishedMatchId` и отказывается заходить в него снова (устаревший
  снапшот комнаты после прошлого раунда), возвращаясь в комнату через
  `return-to-lobby`. Чарт для воспроизведения привязан к слагу матча
  (`duelOnlinePreloadedSlug`), поэтому песня прошлого матча не может достаться
  новому.
- Тесты: `tests/test_duel_lobbies.py` (создание с паролем и без, список,
  неверный/отсутствующий пароль, полная комната, идемпотентный вход,
  санитизация и лимит чата, `after_id`, выбор песни только хостом, старт только
  хостом, `rematch` с сохранением результата, `return-to-lobby` с сохранением
  чата и состава, подтверждение старта: request/confirm/decline, матч
  `waiting` до `/ready`, откат комнаты при отмене, второй раунд после
  `return-to-lobby` играет новую песню, смена песни сбрасывает раунд
  подтверждения, `rematch` берёт песню, выбранную после матча).

### Голосовой чат лобби (`/voice`, без WebSocket)

Клиент JUCE 9.0.2 не умеет открывать WebSocket/TLS-сокет, поэтому голос в
комнате — **REST-поллинг-реле**: каждый игрок шлёт куски PCM (~100 мс)
POST-ом и опрашивает куски соперника GET-ом. Код: `app/api/v1/voice.py`,
`app/schemas/voice.py`, `app/services/voice_relay.py`, тесты
`tests/test_voice_relay.py`, контракт `docs/VOICE_API.md`.

- Формат: 16 кГц, моно, signed 16-bit little-endian PCM, base64 в JSON.
  Ничего не сохраняется: кусок живёт в памяти процесса 3 секунды.
- Все маршруты (включая `/endpoint`) требуют Bearer и членства в комнате:
  `duel_lobby_service.lobby_member` (не дублирует правила). Неизвестная или
  `closed` комната — `404 lobby_not_found` (даже участнику), не участник —
  `403 not_participant`. `playing` разрешён.
- `POST .../chunks` → `{"seq": int}`; `seq` монотонный **для комнаты**
  (глобальный счётчик процесса — поэтому он не переиспользуется после
  истечения пустого буфера). Лимиты: ≤6400 байт после декодирования
  (`422`), ≤60 кусков на комнату, ≤20 новейших на игрока, ~10 в секунду на
  игрока → `429` с **строковым** `detail`: `{"detail": "voice rate limit"}`
  (не конверт `{code, message}`, клиент проверяет только код).
- `GET .../chunks?since=N` → `{"chunks": [...], "next_since": int}` — только
  куски **других** участников с `seq > since`, от старых к новым, максимум
  20 (равно лимиту на игрока, поэтому страница никогда не обрезается).
  `next_since` — максимальный `seq`, хранящийся для комнаты **включая свои**
  куски, иначе `since`; назад не уменьшается. Клиент хранит его и присылает
  в следующий раз; при пустом буфере `{"chunks": [], "next_since": since}`.
- `DELETE .../chunks` → 204, чистит только свои куски; вызывать при
  выключении микрофона и при выходе. Хост, закрывая комнату
  (`POST /duels/lobbies/{id}/leave`), освобождает буфер всей комнаты; выход
  гостя — только его куски (`duel_lobby_service.leave_room`).
- `POST .../endpoint` — прямой UDP-голос (клиент ↔ клиент). Тело —
  **только** `{"port": 1..65535}` (`strict`: `"49152"` и `49152.0` — 422);
  публичный IP сервер выводит сам через `app/services/client_ip.py`
  (`X-Forwarded-For`, первый адрес цепочки; иначе адрес сокета; при прямом
  соединении loopback разрешён), поэтому `ip` в теле игнорируется. Ответ
  `{ip, port, peer}`: `ip`/`port` — **свои**, `peer` — **другого** участника
  или `null` (не публиковал, истёк, в комнате нет гостя). Запись обновляется
  каждым POST, TTL 30 с (`ENDPOINT_TTL_SECONDS`), просрочка чистится и на
  чтении, и на записи; `clear_user`/`clear_lobby` удаляют эндпоинты вместе с
  кусками. `400 client_ip_unavailable`, если адрес вывести нельзя (заголовка
  нет и сокет не IP, либо в XFF приватный/loopback адрес — за Caddy это был бы
  сам прокси). `peer` не кэшируется на диск и не переживает рестарт процесса.
- Один процесс: реестр в памяти рассчитан на один uvicorn worker
  (`docker/entrypoint.sh` без `--workers`); несколько воркеров потребуют
  Redis. Логи содержат только ошибки, payload/токены не логируются.

## Персонажи игрока

- `ui_characters` — админский каталог (миграция `0022_gameplay_visuals`).
  Внутренний PK — UUID; админские `/admin/ui-levels` по-прежнему отдают его в
  поле `id`. Публичный `/characters` отдаёт **целое** `public_id` как `id`
  (миграция `0023_user_character`), чтобы клиент не зависел от UUID.
- `users.character_id` — UUID FK на `ui_characters.id`, `ON DELETE SET NULL`
  (`0023_user_character`): удаление персонажа в админке только сбрасывает выбор.
- `PATCH /auth/me` принимает целый `character_id`; сервер резолвит его в UUID.
  `null` очищает выбор, неизвестный id — 422. Поле можно менять любому
  авторизованному пользователю, админ не нужен.
- Спрайтшит отдаётся с `/media/characters/<uuid>.png` (том
  `gameplay_media_data`, `GAMEPLAY_MEDIA_DIR`), монтирование — в `app/main.py`.
  Все URL в JSON origin-relative, как у ассетов песен.
- Тесты: `tests/test_character_selection.py` (публичный список, `/auth/me`,
  set/clear/unknown, поля дуэли, отдача файла с media mount).

## Сцены (STAGE LEVELS)

- Начиная с миграции `0025_stage_level_no_character` сцена — это только
  `name` и загруженный фон; `stage_levels.character_id` удалён вместе с FK и
  индексом. `ui_characters` и `songs.character_id` не затронуты.
- Админские `POST /admin/stage-levels` и `/replace` принимают только
  multipart `name` и `file`; параметра `character_name` нет и 422 про
  персонажа не бывает. Старый закэшированный JS может прислать
  `character_name` — FastAPI игнорирует лишнее поле.
- `StageLevelResponse`: `id`, `name`, `background_url`, `content_type`,
  `size_bytes`, `original_name`, `created_at`, `updated_at`. Полей
  `character_id` / `character_name` нет.
- Выбор в песне остаётся независимым: `songs.character_id` и `songs.stage_id`
  можно задавать в любой комбинации. Публичный контракт не изменился:
  `character_name` / `character_sheet_url` берутся из персонажа песни,
  `stage_name` / `stage_background_url` — из её сцены.
- Тесты: `tests/test_gameplay_visuals.py` (создание сцены без персонажа,
  точный shape ответа, валидация PNG/JPEG, замена, удаление, публичные поля
  песни).

## Нормализация и анти-фарм (миграция `0030_scoring_normalization`)

Клиент 1.8.0+. Сервер считает всё сам; клиент присылает только факты.

| Метод | Путь | Тело / параметры | Ответ |
|---|---|---|---|
| POST | `/campaigns/{campaign_id}/stages/{position}/result` | Bearer, `{run_id: UUID, won: bool, score_points: int>=0, accuracy_percent: 0..100, perfect_count, great_count, good_count, miss_count, completion_percent: 0..100, notes_total?, notes_played?}` (`extra=forbid`) | `{run_id, already_processed, won, stage_count, best_stage, completed, score_points, normalized_score, completion_percent, xp_earned, xp_breakdown, xp_total, level, level_up, pb_improved, repeat_factor, unlocked_character\|null}`. 404 неизвестная кампания, 409 `stage_not_current` / `no_stages`, 422 на битое тело |
| GET | `/gameplay/rules` | — (публичный) | `min_completion_for_xp`, `tower_repeat_decay`, `tower_repeat_floor`, `online_repeat_factors[]`, `online_repeat_floor`, `pb_bonus_factor`, `normalization_min_completion`, `normalization_max_multiplier` |
| GET | `/campaigns/progress` | Bearer | как раньше; `best_stage` по-прежнему обрезан по числу этапов |

Поля-аддитивные изменения существующих ответов:

- `POST /games/sessions/{id}/finish` (и `ScoreResponse`) — `normalized_score: int|null`;
  заполняется для `normal`/`daily`, `null` для `preview`/`practice` и слишком
  ранних попыток.
- `duel_participants` в снапшоте/результате дуэли — `score_points: int|null`
  (`perfect*100 + great*85 + good*70`) и `normalized_score: int|null`.

Правила:

- `normalized_score = actual / effective_completion`, только если эффективное
  прохождение ≥ `NORMALIZATION_MIN_COMPLETION` (по умолчанию 10%) и
  `notes_total > 0`; множитель ≤ 10. Иначе `null`. Фактический счёт не меняется.
- Battle: `POST .../stages/{position}/result` — единственный путь XP.
  `position == best_stage % stage_count`, иначе 409 `stage_not_current`;
  завершённая башня начинает новый заход со сцены 0, счётчик `best_stage`
  монотонно растёт, отображается обрезанным. `won=false` — 0 XP, прогресс не
  меняется. XP = `progression.xp_for_attempt(..., repeat_multiplier=tower_factor)`,
  где `tower_factor = max(floor, decay ** completion_count_before)` (1-й заход
  1.0, 2-й 0.5, 3-й 0.25, floor 0.05). Бонус PB = `pb_bonus_factor` (0.5) × база,
  только при строго лучшей точности на этой кампании+сцене и один раз за run_id.
  `(user_id, run_id)` уникален: повтор возвращает сохранённую строку с
  `already_processed=true`, XP/прогресс не меняются (`xp_earned` — сохранённая
  сумма этого запуска). `user_campaign_progress.completion_count` — число полных
  прохождений, переживает релогин.
- Online: Elo не изменён. XP = `xp_for_duel(...) × difficulty_factor(song) ×
  online_repeat_factor(завершённые дуэли этой песни за UTC-день)`; факторы
  `(1.0, 0.5, 0.25, 0.1)`, затем floor 0.05. PB-бонус — только при строгом
  улучшении `duel_song_bests` (таблица `(user_id, song_id)`), один раз; аудит в
  `duel_participants.xp_breakdown` (JSONB). `xp_applied_at` защищает от
  повторного начисления.
- XP/рейтинг по-прежнему требуют `completion >= MIN_COMPLETION_FOR_XP` (50%) и
  остальных старых условий. Practice/Study не ранжируются.
- Настройки — `app/services/reward_config.py`, переопределяются переменными
  `TOWER_REPEAT_DECAY`, `TOWER_REPEAT_FLOOR`, `ONLINE_REPEAT_FACTORS`,
  `ONLINE_REPEAT_FLOOR`, `PB_BONUS_FACTOR`, `NORMALIZATION_MIN_COMPLETION`,
  `NORMALIZATION_MAX_MULTIPLIER`. Клиент их не присылает.
- Тесты: `tests/test_scoring_normalization.py` (нормализация, порог 50%, затухание
  башни и онлайн, идемпотентный run_id, PB, Practice без XP, `/gameplay/rules`).


## Личные сообщения (/messages, миграция 0033_direct_messages)

Приватная переписка игроков; клиент — раздел **MESSAGES** (Source/UI/MessagesPanel.*).

- GET /messages/conversations?limit=50&offset=0 — {items:[{user:CompactPlayer,
  last_message:{id, body, created_at, mine}, unread}], total, unread_total}.
  	otal — число диалогов, unread_total — глобальный счётчик непрочитанных
  (не зависит от пагинации). Сортировка: последнее сообщение сверху; диалоги
  с деактивированными аккаунтами не показываются.
- GET /messages/{user_id}?limit=50&before=<ISO8601> — {peer:CompactPlayer,
  items:[{id, body, created_at, mine, read}], has_more}; элементы всегда
  oldest→newest, efore — эксклюзивный курсор по created_at. Открытие треда
  помечает все входящие сообщения прочитанными в той же транзакции, поэтому
  входящие в ответе всегда ead: true.
- POST /messages/{user_id} {body} → 201 {id, body, created_at, mine, read:false}.
  ody обрезается по краям, 1..2000 символов (иначе 422); себе — 400
  cannot_message_self; блокировка в любую сторону — 403 locked; нет игрока
  или он деактивирован — 404 user_not_found. Писать не-друзьям разрешено.
- POST /messages/{user_id}/read → {unread_total} (пометить тред прочитанным).
- GET /messages/unread → {unread_total} — для бейджа в сайдбаре (опрос 20 с
  вместе с /social/notifications).

Правила:

- created_at — RFC3339 UTC с микросекундами и Z.
- mine — отправил я; ead — получатель открыл тред.
- Блокировка не скрывает историю и не мешает чтению — запрещена только отправка.
- Модель: pp/models/messages.py (DirectMessage, ограничения
  sender_id <> recipient_id и длина body 1..2000); логика —
  pp/services/message_service.py; тесты — 	ests/test_messages.py.
## Guitar Skills (миграция `0035_guitar_skills`)

`Guitar Skills` заменяет старую сумму лучших результатов как главный рейтинг.
Формула: `0.35*A + 0.20*C + 0.10*S + 0.25*D + 0.10*P`, все компоненты 0..100,
веса хранятся в `guitar_skills_config` и валидируются суммой 1.0.

### Источники событий

- **Solo (Online)**: `game_sessions.mode IN ('normal','daily')`,
  `status='finished'`, есть `scores`, `completion_percent >= 80`.
  `practice` и `preview` исключены; Study не создаёт серверную сессию.
- **Duels**: `duel_matches.mode='online'`, `status='finished'`, два реальных
  (не бот) `duel_participants` с `result` и `rating_before`/`rating_after`.
  Бот-дуэли и незавершённые матчи исключены.
- События идемпотентны по `event_key`: `solo:<session_id>` /
  `duel:<match_id>:<user_id>`. Повторная обработка — no-op.

### Компоненты

- `A` — точность последних `window_size` (30) событий; вес
  `2^(-i/decay) * difficultyFactor * noteFactor`, per-song cap 2 попытки.
- `C` — `100 * Σ((acc*difficulty*note/1.25) * weight) / Σ(weight)`.
- `S` — `actual_score / (notes_total*100)`, clamp 0..1; события без
  `notes_total` пропускаются.
- `D` — только online-дуэли: `Actual = 0.70*Outcome + 0.30*Margin`,
  `Expected` — Elo от `0.70*GuitarSkills + 0.30*NormalizedLevel`,
  `Delta = Actual - Expected`, `Reliability = n/(n+10)`,
  `D = clamp(50 + 80*weighted(Delta)*Reliability, 0, 100)`.
  Для новых дуэлей `duel_participants.guitar_skills_before` фиксирует
  предматчевый рейтинг; исторические матчи используют rating-proxy и
  пониженную confidence.
- `P` — `0.70*LevelScore + 0.30*XPScore`, `xp_reference` фиксирован
  (XP до уровня 50), не максимум по пользователям.

Нет истории — нейтральные `50` и `confidence=0`, не нули.
`provisional=true`, пока `confidence < 0.35` или событий меньше 5.
`position_change = previous_rank - rank` (плюс — рост), `null` при первом
расчёте. В публичную таблицу попадают только игроки с ≥1 ranked-событием;
остальные получают нейтральный снимок, но не занимают место.

### Клиент

- Home (`MainComponentNetwork.cpp`, `MainComponent.h`): таблица
  `# / PLAYER / LEVEL / SKILLS / ACC / DUELS / STATUS`; подиум показывает
  `SKILLS`, `LVL`, `ACC` и стрелку `position_change`.
- AccountWindow (`Source/UI/MainComponentUI.cpp`): OVERVIEW =
  `LEVEL / GUITAR SKILLS / RATING / TOTAL XP`; в GAME STATISTICS добавлен
  `PEAK RATING`; detail грузится через `/leaderboard/global/{id}`.
- PublicProfilePanel: шапка `LEVEL / SKILLS / RANK`, OVERVIEW-плитки и
  stacked bar из пяти компонентов с весами и confidence; `ACCURACY`
  берётся из `accuracy_percent`, если социальная карточка не даёт
  `best_accuracy`.
- Старый сервер без `guitar_skills`: Home падает обратно на
  `rating_points`, STATUS показывает `-`; профиль показывает `-`.
- `user_id` своего профиля берётся из `/duels/me` (отдельного
  `onlineUserId` в клиенте нет).

### Админ-панель

`/admin/` → вкладка **Guitar Skills**: все параметры конфига, live-проверка
суммы весов 100%, история, статус, **Recalculate Global Leaderboard** с
подтверждением и Dry Run (таблица old/new skills и old/new rank).
Кэш-бастинг: `admin.css`, `admin.js`, `admin-site.js`,
`admin-guitar-skills.js` c `?v=20261115-1`.

### Пересчёт

- После ranked solo — `record_solo_event` + `recalculate_user` в той же
  транзакции.
- После online-дуэли — `on_duel_finalized` (фиксирует pre-match skills,
  пишет два события, пересчитывает обоих).
- При изменении конфига — bump `calculation_version`, история и фоновый
  `recalculate_all` с process-lock.
- Backfill идемпотентен, не трогает XP/Level/награды и не удаляет старые
  данные. Dry run перед переключением обязателен.
## Lobby voice chat (`/voice`, без миграции)

JUCE 9.0.2 не имеет WebSocket/TLS-сокета, поэтому голос идёт через HTTPS-обмен
PCM-чанками. Клиент — `Source/Audio/VoiceChat.*`, кнопка **VOICE: ON/OFF** в
окне Online-лобби. Сервер хранит чанки только в памяти (TTL 3 с), не пишет
аудио на диск и не логирует payload.

| Метод | Путь | Тело / параметры | Ответ |
|---|---|---|---|
| POST | `/voice/lobbies/{lobby_id}/chunks` | Bearer, `{"data":"<base64 PCM>"}`; PCM = signed 16-bit LE mono 16 kHz, ≤ 6400 байт (200 мс) | `{"seq": 123}`; `seq` — глобальный для лобби, монотонный |
| GET | `/voice/lobbies/{lobby_id}/chunks?since=0` | Bearer, `since` ≥ 0 | `{"chunks":[{"seq","user_id","username","data"}],"next_since":N}`; только чужие чанки, `seq > since`, не больше 20 |
| DELETE | `/voice/lobbies/{lobby_id}/chunks` | Bearer | 204; очищает свои чанки, идемпотентно |
| POST | `/voice/lobbies/{lobby_id}/endpoint` | Bearer, `{"port": 49152}` — целый 1..65535 | `{"ip":"203.0.113.5","port":49152,"peer":{"ip":"198.51.100.7","port":50001} \| null}` |

Правила:
- Только участники лобби (host/guest); неизвестное/закрытое лобби — 404,
  не участник — 403, без токена — 401.
- Лимиты: 6400 байт после base64-декодирования, 20 последних чанков на
  пользователя, 60 на лобби, TTL 3 с; превышение частоты — 429.
- `next_since` включает и свои чанки, чтобы клиент не перечитывал их.
- Клиент: захват с микрофона из Settings (`MIC INPUT`), 16 кГц mono,
  100 мс чанки; воспроизведение микшируется в основной output.
- Если микрофон совпадает с гитарным входом, захват идёт из основного
  audio callback (все активные каналы); иначе открывается отдельный
  `AudioDeviceManager` для микрофона. При ошибке открытия — fallback на
  гитарный вход.
- Кнопка **TUNER** в лобби открывает standalone-тюнер, не закрывая комнату;
  **BACK TO LOBBY** возвращает в комнату.

### Прямой UDP-голос: обмен эндпоинтами (клиентская сторона, для `juce-dev`)

Серверная часть готова (`POST /voice/lobbies/{lobby_id}/endpoint`), клиент
ещё не реализован. Что должен сделать клиент, без изменения серверного
контракта:

1. На включение **VOICE: ON** в лобби открыть `juce::DatagramSocket` на
   эфемерном локальном порту (bind на `0`), получить `getBoundPort()`.
   Сокет живёт, пока голос включён или пока комната открыта; закрывать при
   выходе из лобби и при выключении микрофона.
2. Отправлять `POST .../endpoint` с `{"port": <bound port>}`:
   сразу после bind, затем периодически (~каждые 10 с, TTL сервера 30 с) и
   дополнительно при входе в комнату/старте матча. Тело содержит **только**
   `port`; IP не отправлять — его определяет сервер.
3. Из ответа брать `peer`. `peer == null` — UDP-аудио ещё некуда слать:
   продолжать REST-релей (`/chunks`) или молчать, но не считать это ошибкой.
   `peer` меняется (соперник переподключился) — просто обновлять адрес
   назначения; никакого handshake нет.
4. Свой IP/порт из ответа (`ip`, `port`) можно использовать для диагностики,
   но полагаться на них в логике не нужно. `peer.ip` может быть IPv6
   (клиент пришёл по IPv6): если `juce::DatagramSocket` создан под IPv4,
   `peer` с IPv6-адресом использовать нельзя — оставаться на REST-рееле.
5. `400 client_ip_unavailable` — сервер не смог определить публичный адрес
   (нет `X-Forwarded-For` за прокси или адрес приватный). Это не 401:
   токен в порядке, повторять логин не нужно. Fallback — REST-релей.
   `401` — просить логин заново (токен только в памяти клиента).
6. Сетевой вызов синхронный: выполнять его в `juce::Thread`/`ThreadPool`,
   результат отдавать в message thread через `MessageManager::callAsync`.
   В `getNextAudioBlock`/`prepareToPlay` — только атомики и заранее
   выделенные буферы; адрес соперника хранить в атомарной структуре
   (`std::atomic<uint32_t>` для IPv4 + `std::atomic<uint16_t>` для порта,
   либо `SpinLock`-защищённая копия, обновляемая из message thread).
7. UDP-приёмник — отдельный поток: `read()` с таймаутом/`waitUntilReady`,
   декод 16 кГц mono -> буфер воспроизведения. Никакого I/O в audio callback.
8. Порт менять нельзя без повторной публикации: сервер хранит последний
   присланный порт (обновление перезаписывает запись целиком).
9. Формат полезной нагрузки UDP сервером **не** задаётся — это должны
   согласовать клиенты между собой (тот же PCM 16 кГц mono, что и в чанках,
   плюс собственный порядковый номер для борьбы с переупорядочиванием).
   Сервер в этом трафике не участвует и ничего о нём не знает.
10. Ничего не персистится: после рестарта API/клиента обмен нужно повторить
    (публикация на старте сессии это уже покрывает).

## Артворк и аватары (миграция `0036_artwork_avatars`)

Аддитивно на базе `0035_guitar_skills`; существующие поля не меняются.

- `artwork_assets` (`id`, `slot` UNIQUE, `url`, `updated_at`) — три
  админских слота: `hero-battle`, `hero-duel`, `default-avatar`. Одна строка
  на слот: загрузка заменяет URL, прежний файл удаляется после коммита.
- `users.avatar_image_url` (`VARCHAR(500) NULL`) — аватар игрока. `NULL` у
  всех существующих аккаунтов: клиент рисует первую букву имени.
- Файлы: `/data/landing_media/artwork/<slot>-<uuid>.<ext>` (слоты админа) и
  `/data/landing_media/avatars/avatar-<uuid>.<ext>` (игроки). Обе папки
  вложены в том `landing_media_data`, отдельного тома нет. Наружу —
  `GET /media/artwork/<name>` и `GET /media/avatars/<name>`.
- Проверка загрузок: расширение из `.png/.jpg/.jpeg/.webp` **и** магические
  байты; `content_type` клиента игнорируется. Лимиты:
  `MAX_ARTWORK_BYTES` = 8 МБ (hero), `MAX_AVATAR_BYTES` = 2 МБ
  (`default-avatar` и `/profile/avatar`). 415 — формат, 413 — размер,
  422 — неизвестный слот.
- `GET /site/config` — то, что читает клиент:
  `{"hero_battle_url": "/media/artwork/hero-battle-<uuid>.png" | null,
  "hero_duel_url": ... | null, "default_avatar_url": ... | null}`.
  Те же три ключа аддитивно добавлены в `GET /landing`.
- `avatar_image_url` (аддитивно, `str | null`) отдаётся в `/auth/me`,
  строках `/leaderboard/global`, компактной карточке игрока (друзья,
  заявки, поиск, участник диалога), `/social/players/{id}` и
  `/profile/users/{id}`.
- Админка: вкладка **Artwork** (`app/static/admin/admin-artwork.js`,
  разметка `#sectionArtwork` в `index.html`), три карточки с превью,
  выбором файла, кнопками Upload/Remove и строкой статуса. Токен — общий
  админский из `sessionStorage`.
- Клиент: `Source/Network/MainComponentArtwork.cpp` (в `.jucer`).
  `refreshSiteArtwork()` вызывается после входа и при каждом переходе в HOME;
  JSON читается на пуле `ApiClient`, картинки качаются
  `ApiClient::download` + `runAsync` и кэшируются по URL. Результат:
  `HomeView::setHeroBackgrounds(battle, duel)` и
  `HubHeader::setAvatarImage(...)`; невалидная картинка оставляет
  процедурный фон и первую букву. `MainComponent::avatarFor(userId)` отдаёт
  аватар игрока (или `default_avatar_url`) для будущих строк/подиума.
  Профиль: `UPLOAD AVATAR`/`REMOVE AVATAR` в карточке ACCOUNT
  (`AccountWindow`), `juce::FileChooser::launchAsync` и
  `ApiClient::postFileAsync("/profile/avatar", ...)` — без блокировки
  message thread; после ответа обновляется чип игрока.
