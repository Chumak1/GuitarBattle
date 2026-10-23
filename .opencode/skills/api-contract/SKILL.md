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
| PATCH | `/auth/me` | `display_name` | профиль |
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

Админские маршруты `/admin/songs...` требуют `is_admin`.

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
- `access_token` живёт только в памяти; при 401 нужен повторный логин.
- При 403 на `/auth/login` почта не подтверждена; это не неверный пароль.
- После смены/сброса пароля старый JWT перестаёт работать (401) из-за `token_version`.
- Ошибки FastAPI — `{"detail": ...}`; клиент должен это учитывать.
- SMTP-переменные и коды никогда не логировать в production.
- Не логировать токены и пароли.
