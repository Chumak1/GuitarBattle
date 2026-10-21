---
name: GuitarBattle Server Docker
description: Run, inspect, migrate, and troubleshoot the FastAPI + PostgreSQL GuitarBattle server in Docker Compose
---

# GuitarBattle Server в Docker

## Когда использовать

При запуске сервера, проверке API, работе с БД, миграциями Alembic и ошибками контейнеров.

## Запуск

```powershell
cd D:\GuitarBattle\GuitarBattleServer
docker compose up --build -d
docker compose ps
curl.exe -s http://localhost:8000/health/ready
```

Ожидаемый ответ: `{"status":"ready","database":"ok"}`.

## Диагностика

```powershell
docker compose ps
docker compose logs --tail=100 api
docker compose logs --tail=100 db
curl.exe -s http://localhost:8000/api/v1/songs?limit=2
```

Документация: `http://localhost:8000/docs` (кроме `APP_ENV=production`).

## База данных

```powershell
# Интерактивный psql; переменные окружения уже есть внутри контейнера
docker compose exec db sh -lc 'psql -U "$POSTGRES_USER" -d "$POSTGRES_DB"'

# Проверить текущую миграцию
docker compose exec api alembic current

# Применить миграции (обычно это делает entrypoint)
docker compose exec api alembic upgrade head
```

Выдать администратора (подставьте email; имя БД и пользователь берутся из `.env`):

```powershell
docker compose exec db psql -U <POSTGRES_USER> -d <POSTGRES_DB> -c "UPDATE users SET is_admin = TRUE WHERE lower(email) = lower('admin@example.com');"
```

После изменения роли нужно залогиниться заново.

## Миграции

- Файлы: `migrations/versions/`.
- Не генерируй миграцию только внутри контейнера: файл исчезнет. Используй venv на хосте или контейнер с volume.
- Всегда просматривай `alembic upgrade head` и autogenerate diff перед применением.
- Не удаляй миграции из истории; для отката используй новую миграцию.

## Запрещено

- `docker compose down -v` — удаляет `postgres_data` и `song_files_data`.
- Читать/печатать `.env`, токены и пароли.
- Менять пароль БД без обновления `.env` и перезапуска.
- Коммитить `.env`, `__pycache__`, `.venv`.

## Тесты

Сейчас `tests/` содержит только README. При добавлении:

```powershell
docker compose exec api python -m compileall app
# после появления pytest:
docker compose exec api pytest -q
```

Для тестов нужен отдельный test-database или транзакции с откатом; не гоняй destructive-тесты по рабочей БД.
