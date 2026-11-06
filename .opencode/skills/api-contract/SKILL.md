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
| GET | `/auth/me` | Bearer | профиль, аддитивно `privacy_consent_at` (`str \| null`) и `is_admin` (`bool`, по умолчанию `false`) |
| PATCH | `/auth/me` | `display_name?`, `username?` (хотя бы одно поле; `username` — 3–32 символа `[A-Za-z0-9_.-]`, приводится к нижнему регистру, 409 если занят) | профиль |
| POST | `/auth/change-password` | Bearer, `current_password`, `new_password` | 204, без тела; старые токены инвалидируются |
| GET | `/songs` | `search?`, `genre?`, `difficulty?`, `include_drafts?`, `limit`, `offset` | список опубликованных **без** песен со слотом Daily Challenge; у каждого элемента `readiness`, `preview_url` (`str \| null`, абсолютный URL) и `is_published` (`bool`), а также аддитивные `time_signature_numerator` / `time_signature_denominator` (`int`, по умолчанию `4`/`4`); `include_drafts=1` c admin-JWT добавляет черновики |
| GET | `/songs/{slug}` | Bearer admin (опционально) | детали + `version` + `files` + `assets` + `readiness` + `preview_url` + `is_published` + `time_signature_numerator` / `time_signature_denominator` (размер такта, аддитивно); песня дня тоже резолвится (клиент качает ассеты без токена); черновик — только админу, архив — никому |
| GET | `/songs/{slug}/files/{file_id}/download` | Bearer admin (опционально) | MIDI/MP3; разрешены роли `midi`, `drums`, `bass`, `guitar`, `others`, `preview`; файлы черновика — только админу |
| GET | `/admin/songs` | Bearer admin, `search?`, `published?`, `limit`, `offset` | черновики и опубликованные; у каждой песни `daily_weekday` |
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
| POST | `/games/sessions` | `song_id` (UUID), `speed_percent`, `daily?` (bool, по умолчанию `false`) | сессия; `mode` = `normal`/`daily`/`preview` (черновик, только админ) |
| POST | `/games/sessions/{id}/finish` | `score_points`, `accuracy_percent`, `max_combo`, `perfect_count`, `good_count`, `miss_count`, опционально `completion_percent`, `avg_timing_error_ms`, `notes_total`, `notes_played` | score + блок прогрессии (см. ниже); у `preview` прогрессия нулевая |
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
| GET | `/landing` | — (публичный) | `blocks`, `hero_background`, `download` (Windows installer, для обратной совместимости) и аддитивные `downloads` (`{"windows": DownloadInfo\|null, "macos": DownloadInfo\|null}`) — установщики; `updates` — та же карта для **portable** ZIP автообновления (`url` = `/api/v1/downloads/game/update?platform=...`, `available=false`, если portable-сборки нет). `DownloadInfo` = `available`, `version`, `filename`, `size_bytes`, `url`, `release_notes` |
| GET | `/downloads/game` | — (публичный), `platform=windows\|macos` (по умолчанию `windows`) | активный **installer** платформы (Windows `.exe/.msi`, macOS `.dmg`); 404, если установщика нет; 422 для неизвестного `platform` |
| GET | `/downloads/game/update` | — (публичный), `platform=windows\|macos` (по умолчанию `windows`) | активный **portable** ZIP платформы; 404, если portable-сборки нет; 422 для неизвестного `platform` |
| GET | `/geo` | — (публичный) | `country`, `language` (`ru`/`en`/`null`) по IP из локальной DB-IP Lite |
| GET | `/site/contacts` | — (публичный) | `name` (`ru`/`en`), `email`, `telegram` (`@handle`), `telegram_url`, `socials` (`youtube`/`instagram`/`twitter`/`vk`; `null` — заглушка «Скоро»); без записи в CMS отдаёт дефолты |
| POST | `/activity/heartbeat` | Bearer, `client_session_id` (≤64), `app_version?`, `platform?`, `final=false` | 200, `session_id`, `started_at`, `last_seen_at`; upsert по `(user_id, client_session_id)`, `final=true` ставит `ended_at` |
| POST | `/activity/game-launch` | Bearer, `client_session_id?`, `game_key` (≤120), `song_id?`, `mode?` | 201, `id`; неизвестные `song_id`/сессия сохраняются как `NULL`, а не 4xx |
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

Публичные страницы сайта: `/` (лендинг), `/policy` (политика конфиденциальности),
`/login`, `/register`, `/account`, `/admin/` (только администраторы).
Статистика — вкладка «Статистика» внутри `/admin/`, отдельного `/statistics` нет.
Медиа лендинга отдаётся по `/media/landing/<name>`.

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
