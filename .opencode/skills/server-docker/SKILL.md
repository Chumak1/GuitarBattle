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

## Production (fretfight.online)

- DNS: публичная зона `fretfight-online` в Yandex Cloud; NS `ns1.yandexcloud.net`, `ns2.yandexcloud.net`.
- Edge: Caddy на хосте (не в Docker), `/etc/caddy/Caddyfile`; TLS Let's Encrypt; reverse proxy на `127.0.0.1:8000`.
- Конфиг Caddy версионируется: `GuitarBattleServer/deploy/Caddyfile`, применяется
  `pwsh -File scripts/deploy-prod.ps1 -Caddy`. Сертификаты и ACME-аккаунт лежат
  в `/var/lib/caddy/.local/share/caddy` — **не удалять** (иначе перевыпуск и
  риск rate limit Let's Encrypt). Подробности: `GuitarBattleServer/docs/HTTPS_TLS.md`.
- Заголовки `Strict-Transport-Security`, `X-Content-Type-Options`,
  `Referrer-Policy` ставит приложение (`security_headers` в `app/main.py`), не Caddy.
- API-контейнер слушает только `127.0.0.1:8000` (`API_BIND_HOST=127.0.0.1`). В security group открыты только 22 (админский IP), 80 и 443 TCP; UDP/443 для HTTP/3 пока не открыт.
- Проверка: `curl.exe -s https://fretfight.online/health/ready`, `https://fretfight.online/admin/`.
- Caddy: `sudo systemctl status caddy`; после правки конфига — `sudo caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile && sudo systemctl reload caddy`.

## Диагностика

```powershell
docker compose ps
docker compose logs --tail=100 api
docker compose logs --tail=100 db
curl.exe -s http://localhost:8000/api/v1/songs?limit=2
```

Документация: `http://localhost:8000/docs` (кроме `APP_ENV=production`).

## Email (SMTP)

- Переменные: `SMTP_HOST`, `SMTP_PORT`, `SMTP_USERNAME`, `SMTP_PASSWORD`,
  `SMTP_SECURITY` (`starttls`/`ssl`/`none`), `SMTP_FROM_EMAIL`, `SMTP_FROM_NAME`.
- Порт 587 — обычно `starttls`, порт 465 — `ssl`.
- Если `SMTP_HOST` пуст в development, код подтверждения пишется в
  `docker compose logs api`. В production при пустом `SMTP_HOST` регистрация
  возвращает 503: смотреть логи с кодами там нельзя.
- Проверка: зарегистрировать тестовый аккаунт, найти письмо, ввести код через
  `POST /api/v1/auth/verify-email`, затем проверить вход.
- Не печатать SMTP-пароль и коды в чат/лог.

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

```powershell
# Python-код и миграции
python -m compileall -q app tests migrations

# Полный набор; conftest сам поднимает одноразовый PostgreSQL на 127.0.0.1:55432
& "D:\GuitarBattle\GuitarBattleServer\.venv-test\Scripts\python.exe" -m pytest -q
```

Тесты используют `TEST_DATABASE_URL` и отдельную БД с `test` в имени; рабочую БД
они не трогают. Письма в тестах складываются во внутренний outbox, SMTP не нужен.
Не гоняй destructive-тесты по рабочей БД.
