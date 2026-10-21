---
name: GuitarBattle API Contract
description: Keep the JUCE HTTP client and FastAPI server contract in sync
---

# Контракт клиент ↔ сервер

## База

- Клиент: `http://localhost:8000/api/v1` (захардкожен в `MainComponent.cpp`).
- Сервер: `GuitarBattleServer/app/api/v1/`.
- Формат: JSON UTF-8.
- Авторизация: `Authorization: Bearer <access_token>`.

## Эндпоинты

| Метод | Путь | Тело / параметры | Ответ |
|---|---|---|---|
| POST | `/auth/register` | `username`, `email`, `password`, `display_name?` | профиль |
| POST | `/auth/login` | `identifier`, `password` | `access_token`, `token_type` |
| GET | `/auth/me` | Bearer | профиль |
| PATCH | `/auth/me` | `display_name` | профиль |
| GET | `/songs` | `search?`, `genre?`, `difficulty?`, `limit`, `offset` | список опубликованных |
| GET | `/songs/{slug}` | — | детали + файлы |
| GET | `/songs/{slug}/files/{file_id}/download` | — | MIDI/MP3 |
| POST | `/games/sessions` | `song_id` (UUID), `speed_percent` | сессия |
| POST | `/games/sessions/{id}/finish` | `score_points`, `accuracy_percent`, `max_combo`, `perfect_count`, `good_count`, `miss_count` | score |
| GET | `/games/history` | Bearer, `limit`, `offset` | история |
| GET | `/leaderboard` | `song_slug?`, `limit`, `offset` | результаты |
| GET | `/leaderboard/global` | `limit`, `offset` | рейтинг игроков |

Админские маршруты `/admin/songs...` требуют `is_admin`.

## Где искать в клиенте

`Source/MainComponent.cpp`:

- `authenticateOnlineAccount` — register/login/me.
- `startOnlineGameSession` — создание сессии.
- `submitOnlineGameResult` — отправка результата.
- `refreshGlobalLeaderboard` — глобальный рейтинг.
- `loadFirstOnlineCatalogSong` — каталог, скачивание MIDI/MP3.
- Поиск по `juce::URL`, `createInputStream`, `postJson`.

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
- Ошибки FastAPI — `{"detail": ...}`; клиент должен это учитывать.
- Не логировать токены и пароли.
