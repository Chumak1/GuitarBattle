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
| POST | `/auth/register` | `username`, `email`, `password`, `display_name?` | профиль, `email_verified=false` |
| POST | `/auth/login` | `identifier`, `password` | `access_token`, `token_type`; 403, если почта не подтверждена |
| POST | `/auth/verify-email` | `email`, `code` (6 цифр) | `access_token`; подтверждает почту |
| POST | `/auth/resend-verification` | `email` | 202, всегда одинаковый ответ |
| POST | `/auth/forgot-password` | `email` | 202, всегда одинаковый ответ |
| POST | `/auth/reset-password` | `email`, `code`, `new_password` | `access_token`; старые токены инвалидируются |
| GET | `/auth/me` | Bearer | профиль |
| PATCH | `/auth/me` | `display_name?`, `username?` (хотя бы одно поле; `username` — 3–32 символа `[A-Za-z0-9_.-]`, приводится к нижнему регистру, 409 если занят) | профиль |
| POST | `/auth/change-password` | Bearer, `current_password`, `new_password` | 204, без тела; старые токены инвалидируются |
| GET | `/songs` | `search?`, `genre?`, `difficulty?`, `limit`, `offset` | список опубликованных **без** песен со слотом Daily Challenge; у каждого элемента `readiness` |
| GET | `/songs/{slug}` | — | детали + `version` + `files` + `assets` + `readiness`; песня дня тоже резолвится (клиент качает ассеты без токена) |
| GET | `/songs/{slug}/files/{file_id}/download` | — | MIDI/MP3 |
| GET | `/admin/songs` | Bearer admin, `search?`, `published?`, `limit`, `offset` | черновики и опубликованные; у каждой песни `daily_weekday` |
| POST | `/admin/songs` | Bearer admin, `slug`, `title`, `bpm`, остальные поля опциональны, `daily_weekday?` (0–6 или `null`) | черновик |
| GET | `/admin/songs/{id}` | Bearer admin | детали + `assets` + `readiness` + `daily_weekday` |
| PATCH | `/admin/songs/{id}` | Bearer admin, поля песни; `daily_weekday: null` снимает слот | обновлённая песня |
| GET | `/admin/daily` | Bearer admin | 7 слотов `{weekday, weekday_name, song \| null, updated_at}` |
| PUT | `/admin/daily/{weekday}` | Bearer admin, `{song_id: UUID \| null}`, weekday 0–6 | слот; назначение переносит песню, `422` вне 0–6, `404` неизвестная песня |
| POST | `/admin/songs/{id}/publish` | Bearer admin | публикация |
| POST | `/admin/songs/{id}/unpublish` | Bearer admin | снятие с публикации |
| DELETE | `/admin/songs/{id}` | Bearer admin | 204 |
| POST | `/admin/songs/{id}/files/midi` | Bearer admin, multipart `file` | новый `SongFile` (`role=midi`) |
| POST | `/admin/songs/{id}/files/audio/{role}` | Bearer admin, multipart `file`; `role`: `drums`, `bass`, `guitar`, `others` | новый `SongFile` |
| GET | `/admin/songs/{id}/files` | Bearer admin | все версии файлов |
| GET | `/admin/songs/{id}/files/{file_id}/download` | Bearer admin | файл |
| POST | `/admin/songs/{id}/files/{file_id}/analyze` | Bearer admin | сохранённый анализ MIDI |
| POST | `/games/sessions` | `song_id` (UUID), `speed_percent`, `daily?` (bool, по умолчанию `false`) | сессия; `mode` = `normal`/`daily` |
| POST | `/games/sessions/{id}/finish` | `score_points`, `accuracy_percent`, `max_combo`, `perfect_count`, `good_count`, `miss_count`, опционально `completion_percent`, `avg_timing_error_ms`, `notes_total`, `notes_played` | score + блок прогрессии (см. ниже) |
| GET | `/games/history` | Bearer, `limit`, `offset` | история |
| GET | `/games/best-scores` | Bearer | лучший счёт по каждой опубликованной песне вне Daily Challenge + сумма рейтинга |
| GET | `/leaderboard` | `song_slug?`, `limit`, `offset` | результаты |
| GET | `/leaderboard/global` | `limit`, `offset` | рейтинг игроков (сумма лучших результатов по песням вне Daily Challenge) |
| GET | `/profile/progression` | Bearer | уровень, XP, мастерство, жанры, статистика |
| GET | `/profile/personal-bests` | Bearer, `limit` (1–100, по умолчанию 10), `offset` | личные рекорды по опубликованным песням |
| GET | `/achievements` | Bearer | каталог достижений + прогресс/разблокировка; у каждого элемента `icon` и `icon_url` |
| GET | `/profile/users/{user_id}` | Bearer | публичная карточка игрока; у выданных достижений `icon` и `icon_url` |
| GET | `/media/achievements/<icon>.svg` | — (публичный) | SVG иконки достижения, `image/svg+xml` |
| GET | `/daily/today` | Bearer | `date`, `weekday` (0=Пн…6=Вс, UTC), `song` (или `null`), официальная попытка, топ-5 |
| GET | `/daily/leaderboard` | Bearer, `date?` (YYYY-MM-DD, по умолчанию сегодня UTC) | официальные результаты дня |
| GET | `/daily/history` | Bearer, `limit` (1–100, по умолчанию 20), `offset` | официальные результаты игрока по дням с `rank` и `participants` |
| GET | `/client-assets/manifest` | — (публичный) | `schema`, `version`, `archive_url`, `archive_size_bytes`, `archive_sha256`, `files[]`; 503, если manifest или bundle отсутствует |
| GET/HEAD | `/client-assets/bundle.zip` | — (публичный) | zip `guitarbattle-sounds.zip`; 503, если архив отсутствует |
| GET | `/landing` | — (публичный) | `blocks`, `hero_background`, `download` для лендинга |
| GET | `/downloads/game` | — (публичный) | активная сборка игры; 404, если активной сборки нет |
| GET | `/geo` | — (публичный) | `country`, `language` (`ru`/`en`/`null`) по IP из локальной DB-IP Lite |
| POST | `/activity/heartbeat` | Bearer, `client_session_id` (≤64), `app_version?`, `platform?`, `final=false` | 200, `session_id`, `started_at`, `last_seen_at`; upsert по `(user_id, client_session_id)`, `final=true` ставит `ended_at` |
| POST | `/activity/game-launch` | Bearer, `client_session_id?`, `game_key` (≤120), `song_id?`, `mode?` | 201, `id`; неизвестные `song_id`/сессия сохраняются как `NULL`, а не 4xx |
| GET | `/admin/statistics/overview` | Bearer admin, `days=30` (1–365) | `generated_at`, `users_total`, `users_active_24h`, `users_active_7d`, `sessions_total`, `app_seconds_total`, `avg_session_seconds`, `launches_total`, `launches_24h` |
| GET | `/admin/statistics/users` | Bearer admin, `search?`, `sort=last_seen\|username\|display_name\|email\|created_at\|app_seconds\|sessions_count\|launches_count\|scores_count`, `order=asc\|desc` (по умолчанию `asc` для текста, `desc` для чисел/дат), `limit=50` (1–200), `offset=0` | `items[]` (`user_id`, `username`, `display_name`, `email`, `is_active`, `created_at`, `app_seconds_total`, `sessions_count`, `last_seen_at`, `launches_count`, `last_launch_at`, `games_distinct`, `scores_count`, `best_score`), `total`, `limit`, `offset` |
| GET | `/admin/statistics/games` | Bearer admin, `days=30`, `limit=100` | `items[]` (`game_key`, `title`, `launches`, `unique_users`, `last_launch_at`), `total` |
| DELETE | `/admin/users/{user_id}` | Bearer admin | 204; 400 при удалении себя (`"You cannot delete your own account"`), 404 если нет, 409 если связанные данные мешают; жёсткое удаление, все FK на `users.id` — `CASCADE` |
| GET | `/admin/site/landing` | Bearer admin | `blocks`, `hero_background` |
| PUT | `/admin/site/landing` | Bearer admin, `blocks`, `hero_background` | сохранённый контент; блоки ≤ 256 КиБ, строки ≤ 5000, глубина ≤ 8 |
| POST | `/admin/site/landing/background` | Bearer admin, multipart `file` | 201, `hero_background`; JPEG/PNG/WebP/GIF/MP4/WebM |
| DELETE | `/admin/site/landing/background` | Bearer admin | 200, `hero_background.type = "none"` |
| GET | `/admin/site/game-builds` | Bearer admin | список загруженных сборок |
| POST | `/admin/site/game-builds` | Bearer admin, multipart `file`, `version?` | 201, сборка создаётся неактивной; `.exe/.zip/.msi` |
| POST | `/admin/site/game-builds/{id}/activate` | Bearer admin | 200, сборка становится единственной активной |
| DELETE | `/admin/site/game-builds/{id}` | Bearer admin | 204 |

Публичные страницы сайта: `/` (лендинг), `/policy` (политика конфиденциальности),
`/login`, `/register`, `/account`, `/admin/` (только администраторы).
Статистика — вкладка «Статистика» внутри `/admin/`, отдельного `/statistics` нет.
Медиа лендинга отдаётся по `/media/landing/<name>`.

## Сайт, лендинг и язык

- Лендинг — статический `app/static/landing/index.html`; `app/static/app/landing.js`
  запрашивает `/landing` и накладывает CMS-переопределения. Если переопределения
  для языка нет, остаётся статический перевод из `landing-i18n.js`.
- `blocks` — объект `{hero, facts, about, how, features, requirements, cta}`; каждый
  блок: `{visible: bool, ru: {...}, en: {...}}`. Админ-редактор всегда сохраняет
  оба языка целиком, поэтому публичный ответ содержит полный снимок.
- `hero_background`: `{type: "none" | "image" | "video", url, poster_url}`. Файлы
  лежат в `LANDING_MEDIA_DIR` (`/data/landing_media`), лимит 50 МиБ, сигнатуры
  проверяются на сервере.
- Сборки игры лежат в `GAME_BUILDS_DIR` (`/data/game_builds`), лимит 500 МиБ.
  Активна максимум одна сборка; `/downloads/game` отдаёт её с
  `Content-Disposition: attachment`.
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
  кнопка-корзина удаляет игрока через `DELETE /admin/users/{id}` после
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
5. `POST /auth/reset-password` проверяет код, меняет пароль, подтверждает почту и
   возвращает новый `access_token`. Старые JWT инвалидируются через
   `users.token_version`.

Коды хранятся только как HMAC-SHA256, живут 15 минут, одноразовые, максимум
5 неверных попыток. Существующие аккаунты миграция `0005` помечает
подтверждёнными, чтобы не заблокировать игроков при выкатке.

## Версии контента и кэш ассетов

- `Song.version` — целочисленная **версия контента/кэша**, а не версия файла. Она
  возвращается в `GET /songs/{slug}` (и в админских ответах песни).
- `version` увеличивается на 1 при `PATCH /admin/songs/{id}` и при каждой успешной
  загрузке MIDI (`/files/midi`) или MP3-стема (`/files/audio/{role}`) — в той же
  транзакции, что и создание `SongFile`.
- `GET /songs/{slug}` отдаёт только самый новый файл для каждой роли; у файлов есть
  `role`, `size_bytes`, `sha256`, `download_url`.
- Клиент (`loadFirstOnlineCatalogSong`) хранит `version.txt` в
  `%APPDATA%/GuitarBattle/OnlineSongs/<slug>/`. Если версия на сервере > 0 и совпадает
  с `version.txt`, а MIDI (с заголовком `MThd`) и все опубликованные стемы существуют с
  ожидаемым `size_bytes`, скачивание пропускается. Иначе скачиваются MIDI и все стемы,
  а `version.txt` пишется только после полного успеха. При `version <= 0` клиент всегда
  скачивает файлы и версию не пишет.

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
  детальный ответ дополнительно содержит `assets` с админскими `download_url`.
  Публикация не блокируется при неполном наборе стемов, чтобы не ломать MIDI-only
  песни; администратор видит `publish_ready` и `missing_roles` до публикации.

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
  «Недоступно: в каталоге N жанр(а/ов) из M». Жанры не выдумываются.
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

## Где искать в клиенте

`Source/Network/ApiClient.h` — единственное место HTTP-запросов (`get`, `post`, `getJson`,
`postJson`, `download`, `downloadToFile`, `runAsync`). Запросы выполняются на
`juce::ThreadPool` внутри `ApiClient`, результат приходит в UI через
`juce::MessageManager::callAsync`; синхронных вызовов из обработчиков кнопок нет.

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
- `access_token` живёт только в памяти; при 401 нужен повторный логин.
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
- Повторный `finish` (в том числе параллельный) даёт 409 и не начисляет XP —
  это нормальный ответ, а не сбой.
- `mastery` в ответе `finish` — объект, а `achievements_awarded` — массив объектов,
  а не строк.
- XP за повторные попытки падает (100/50/25/0 %), даже если рекорд растёт.
