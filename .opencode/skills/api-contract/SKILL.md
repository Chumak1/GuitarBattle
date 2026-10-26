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
| GET | `/songs` | `search?`, `genre?`, `difficulty?`, `limit`, `offset` | список опубликованных; у каждого элемента `readiness` |
| GET | `/songs/{slug}` | — | детали + `version` + `files` + `assets` + `readiness` |
| GET | `/songs/{slug}/files/{file_id}/download` | — | MIDI/MP3 |
| GET | `/admin/songs` | Bearer admin, `search?`, `published?`, `limit`, `offset` | черновики и опубликованные |
| POST | `/admin/songs` | Bearer admin, `slug`, `title`, `bpm`, остальные поля опциональны | черновик |
| GET | `/admin/songs/{id}` | Bearer admin | детали + `assets` + `readiness` |
| PATCH | `/admin/songs/{id}` | Bearer admin, поля песни | обновлённая песня |
| POST | `/admin/songs/{id}/publish` | Bearer admin | публикация |
| POST | `/admin/songs/{id}/unpublish` | Bearer admin | снятие с публикации |
| DELETE | `/admin/songs/{id}` | Bearer admin | 204 |
| POST | `/admin/songs/{id}/files/midi` | Bearer admin, multipart `file` | новый `SongFile` (`role=midi`) |
| POST | `/admin/songs/{id}/files/audio/{role}` | Bearer admin, multipart `file`; `role`: `drums`, `bass`, `guitar`, `others` | новый `SongFile` |
| GET | `/admin/songs/{id}/files` | Bearer admin | все версии файлов |
| GET | `/admin/songs/{id}/files/{file_id}/download` | Bearer admin | файл |
| POST | `/admin/songs/{id}/files/{file_id}/analyze` | Bearer admin | сохранённый анализ MIDI |
| POST | `/games/sessions` | `song_id` (UUID), `speed_percent` | сессия |
| POST | `/games/sessions/{id}/finish` | `score_points`, `accuracy_percent`, `max_combo`, `perfect_count`, `good_count`, `miss_count` | score |
| GET | `/games/history` | Bearer, `limit`, `offset` | история |
| GET | `/games/best-scores` | Bearer | лучший счёт по каждой опубликованной песне + сумма рейтинга |
| GET | `/leaderboard` | `song_slug?`, `limit`, `offset` | результаты |
| GET | `/leaderboard/global` | `limit`, `offset` | рейтинг игроков (сумма лучших результатов по песням) |
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
