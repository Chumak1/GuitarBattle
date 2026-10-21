# GuitarBattle — инструкции для агентов

## Проект

`D:\GuitarBattle` — два независимых приложения:

- `GuitarBattle/` — нативный клиент на C++/JUCE 9.0.2: UI, аудио, pitch detection, MIDI и табы. Собирается в Visual Studio 2026 / MSBuild.
- `GuitarBattleServer/` — FastAPI + PostgreSQL + SQLAlchemy + Alembic, запускается через Docker Compose. API: `http://localhost:8000/api/v1`.
- `Backup Server/` и `GuitarBattle/Backup/` — архивы. Не редактировать и не добавлять в Git.

## Маршрутизация задач

| Задача | Агент |
|---|---|
| `GuitarBattle/` — C++/JUCE, UI, аудио, MIDI, табы | `juce-dev` |
| `GuitarBattleServer/` — FastAPI, PostgreSQL, Alembic, Docker | `server-dev` |
| Изменение API-контракта, полей JSON, URL | `api-sync` |
| Ревью diff: realtime safety, безопасность, миграции | `reviewer` |

Если задача затрагивает `GuitarBattleServer/`, не решайте её в сессии `juce-dev`: переключитесь на `server-dev` или делегируйте ему. Если меняется и клиент, и сервер — используйте `api-sync`.

## Ассеты

- `GuitarBattle/Sounds/` и `GuitarBattle/Songs/` хранятся в Git LFS. После клонирования выполните `git lfs install` и `git lfs pull`.
- Не добавляйте аудиофайлы (`.wav`, `.flac`, `.mp3`, `.zip`) из этих каталогов в обычный Git: для них настроены LFS-фильтры в корневом `.gitattributes`.

## Сборка клиента

```powershell
# Пересобрать проект Projucer (только если меняли .jucer)
& "D:\juce-9.0.2-windows\JUCE\Projucer.exe" --resave "D:\GuitarBattle\GuitarBattle\GuitarBattle.jucer"

# Debug x64
& "D:\Visual Studio\Main\MSBuild\Current\Bin\MSBuild.exe" `
  "D:\GuitarBattle\GuitarBattle\Builds\VisualStudio2026\GuitarBattle.sln" `
  /p:Configuration=Debug /p:Platform=x64 /m
```

Результат: `GuitarBattle/Builds/VisualStudio2026/x64/Debug/App/GuitarBattle.exe`.

## Сервер

```powershell
cd D:\GuitarBattle\GuitarBattleServer
docker compose up --build -d
docker compose ps
curl.exe -s http://localhost:8000/health/ready
```

- `.env` содержит секреты. Не читать, не печатать, не коммитить.
- Никогда не выполнять `docker compose down -v` — это удаляет БД и загруженные файлы.
- Документация API: `http://localhost:8000/docs` (при `APP_ENV=production` отключена).
- Тестов пока нет: `tests/` содержит только README.

## Жёсткие правила

1. **Аудио-поток.** В `getNextAudioBlock` и `prepareToPlay` нельзя: выделять память, блокировки, файловый/сетевой I/O, логирование, `juce::String`, `callAsync`, `AlertWindow`. Только атомики, заранее выделенные буферы и lock-free FIFO.
2. **UI-поток.** Сетевые запросы нельзя выполнять синхронно из обработчиков кнопок: текущие вызовы `juce::URL::createInputStream` с таймаутом 4–5 с блокируют message thread. Новый сетевой код — в `juce::Thread`/`ThreadPool`, результат — через `MessageManager::callAsync`.
3. **Проектные файлы.** Источник истины — `GuitarBattle/GuitarBattle.jucer`. После добавления/удаления файлов обновляйте `.jucer` и пересобирайте VS-проект. Не правьте `.vcxproj` вручную без синхронизации с `.jucer`.
4. **Include-cpp.** `MainComponent.cpp` включает `Generation/MidiMelodyGenerator.cpp` и `Generation/SongArrangement.cpp`, потому что их нет в `.jucer`. Не добавляйте эти файлы в проект, пока не удалили соответствующие `#include`.
5. **`PitchDetectionThread.h`** — заголовок с реализацией, включён в `MainComponent.cpp`; не добавляйте его в `.jucer` как отдельный compile unit.
6. **API-контракт.** Базовый URL `http://localhost:8000/api/v1` сейчас захардкожен в `MainComponent.cpp`. При изменении API обновляйте обе стороны и skill `api-contract`.
7. **Секреты.** Не коммитить `.env`, токены, пароли, `Backup Server/`, `GuitarBattle/Backup/`, `Builds/**/x64`, `Source.zip`.
8. **Не трогать без задачи:** `competitions.py` и миграцию `0004` (не подключены к роутеру), архивы в `Backup*`, сгенерированные файлы в `Builds`.

## Архитектура клиента

- `Source/MainComponent.*` — основной экран, состояние игры, аудио, сеть. Большой монолит; новые функции по возможности выносите в отдельные классы.
- `Source/Audio/PitchDetectionThread.h` — worker-поток анализа питча.
- `Source/Audio/PitchDetector.*` — детектор высоты тона.
- `Source/Midi/MidiParser.*`, `TabMapper.h`, `TablatureComponent.*` — MIDI → табы.
- `Source/Gameplay/` — сессия, метроном, scoring, game engine.
- `Source/Generation/` — генератор MIDI и аранжировок.
- `Source/UI/` — стиль и вспомогательные компоненты.
- `Source/Audio/AudioEngine.*`, `Source/Gameplay/GameController.*`, `Source/UI/SelectComponent.*`, `Source/UI/TunerComponent.*` — пустые заглушки; не считать их рабочей логикой.

## Архитектура сервера

- `app/api/v1/router.py` — единственное место регистрации роутеров.
- `app/api/v1/*.py` — эндпоинты.
- `app/models/` — SQLAlchemy-модели.
- `app/schemas/` — Pydantic-схемы.
- `app/services/` — MIDI-парсер и сервисы.
- `migrations/versions/` — Alembic.
- `docker-compose.yml` — `db` + `api`; порт API `127.0.0.1:8000`; у `db` порт наружу не проброшен.

## Проверка перед завершением

- C++: собрать Debug x64, убедиться, что нет новых warning/error, проверить, что изменения не выполняются в аудио-потоке.
- Python: `python -m compileall app`, проверить `/health/ready`, затронутые эндпоинты через `curl.exe`.
- API: при изменении схемы/роутера — миграция, регистрация роутера, обновление клиента и skill `api-contract`.
- Git: `git status` не должен показывать `.env`, `Backup*`, `Builds/**/x64`.
