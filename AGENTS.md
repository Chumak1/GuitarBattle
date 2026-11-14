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

### macOS (локально)

macOS-сборка выполняется на Mac, а не в GitHub Actions:

```bash
./GuitarBattle/scripts/build-macos.sh
```

Скрипт проверяет Xcode, клонирует JUCE 9.0.2 в `~/JUCE`, собирает Projucer,
перегенерирует Xcode-проект, делает universal Release (`arm64 x86_64`),
ad-hoc подпись и кладёт `FretFight-<version>.dmg` / `.zip` в `docs/Output`.
Требования, опции и типовые ошибки: `docs/BUILD_MACOS.md`.
Workflow `.github/workflows/macos.yml` оставлен как справка и не используется.

### Патч JUCE

После установки или обновления JUCE 9.0.2 выполните
`GuitarBattle\scripts\patch-juce-vst3-restart-assert.ps1`. Он убирает ложный
`JUCE_ASSERT_MESSAGE_THREAD` в `VST3HostContextHeadless::restartComponent`:
Neural DSP и подобные плагины вызывают `restartComponent` не из message
thread, а JUCE штатно переносит работу через `ComponentRestarter`. Скрипт
идемпотентен, `-Revert` возвращает исходный файл. После применения или
отката пересоберите проект. Подробности: `docs/JUCE_PATCHES.md`.

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
- Тесты: `tests/` (pytest, отдельная PostgreSQL-БД на `127.0.0.1:55432`); полный прогон: `& "D:\GuitarBattle\GuitarBattleServer\.venv-test\Scripts\python.exe" -m pytest -q`.
- SMTP: `SMTP_*` в `.env`; в development без SMTP код пишется в лог `api`, в production регистрация возвращает 503. Секреты и коды не логировать.

## Жёсткие правила

1. **Аудио-поток.** В `getNextAudioBlock` и `prepareToPlay` нельзя: выделять память, блокировки, файловый/сетевой I/O, логирование, `juce::String`, `callAsync`, `AlertWindow`. Только атомики, заранее выделенные буферы и lock-free FIFO.
2. **UI-поток.** Сетевые запросы нельзя выполнять синхронно из обработчиков кнопок: текущие вызовы `juce::URL::createInputStream` с таймаутом 4–5 с блокируют message thread. Новый сетевой код — в `juce::Thread`/`ThreadPool`, результат — через `MessageManager::callAsync`.
3. **Проектные файлы.** Источник истины — `GuitarBattle/GuitarBattle.jucer`. После добавления/удаления файлов обновляйте `.jucer` и пересобирайте VS-проект. Не правьте `.vcxproj` вручную без синхронизации с `.jucer`.
4. **Include-cpp.** `Generation/MidiMelodyGenerator.cpp` и `Generation/SongArrangement.cpp` включаются только из `Source/Gameplay/MainComponentGameplay.cpp`, потому что их нет в `.jucer`. Не добавляйте эти файлы в проект и не включайте их в другие `.cpp` — иначе duplicate symbols.
5. **`PitchDetectionThread.h`** — заголовок с реализацией. Включается из `MainComponent.cpp` (создание/остановка потока) и `MainComponentAudio.cpp` (`initialiseAudio()`); не добавляйте его в `.jucer` как отдельный compile unit.
6. **API-контракт.** Production-базовый URL `https://fretfight.online/api/v1` захардкожен в `Source/Network/ApiClient.h` (`ApiClient::baseUrl`), ассеты скачиваются от `ApiClient::serverOrigin`. Наружу открыты только 80/443 через Caddy; FastAPI слушает `127.0.0.1:8000`. При изменении API обновляйте обе стороны и skill `api-contract`.
7. **Секреты.** Не коммитить `.env`, токены, пароли, `Backup Server/`, `GuitarBattle/Backup/`, `Builds/**/x64`, `Source.zip`.
8. **Не трогать без задачи:** `competitions.py` и миграцию `0004` (не подключены к роутеру), архивы в `Backup*`, сгенерированные файлы в `Builds`.

## Архитектура клиента

`MainComponent` — один класс, но его реализация разделена по ответственности.
`MainComponent.cpp` — только composition root: конструктор, `timerCallback()`,
деструктор. Конструктор вызывает `initialiseUi()` и `initialiseAudio()`.

| Файл | Ответственность |
|---|---|
| `Source/MainComponent.h` | Объявления класса, состояние игры/аудио/сети. В конце private-секции включает `UI/MainComponentUi.inc`. |
| `Source/MainComponent.cpp` | Composition root: конструктор, `timerCallback()`, деструктор. |
| `Source/UI/MainComponentUI.cpp` | `initialiseUi()`, `resized()`, `paint()`, `paintOverlay()`, `paintModalOverlay()`, диалоги, `AudioSettingsPanel`, UI-хелперы. |
| `Source/UI/MainComponentUi.inc` | Точка расширения frontend: новые виджеты, frontend-состояние, объявления UI-хелперов. Включается внутрь класса, без include guard. |
| `Source/UI/GameStyle.*` | Тема, цвета, шрифты, общие UI-хелперы. |
| `Source/Audio/MainComponentAudio.cpp` | Аудио-устройство, `prepareToPlay()`, `getNextAudioBlock()`, банки сэмплов, выбор входа, `initialiseAudio()`. |
| `Source/Network/MainComponentNetwork.cpp` | Авторизация, каталог, лидерборд, скачивание песен, онлайн-сессии. |
| `Source/Network/MainComponentAssets.cpp` | Жизненный цикл серверного sound-pack. |
| `Source/Gameplay/MainComponentGameplay.cpp` | Сессия, тюнинг, MIDI-цели, scoring. Здесь же компилируются `Generation/MidiMelodyGenerator.cpp` и `SongArrangement.cpp`. |
| `Source/Audio/PitchDetectionThread.h` | Worker-поток анализа питча (заголовок с реализацией, не compile unit). |
| `Source/Audio/PitchDetector.*` | Детектор высоты тона. |
| `Source/Midi/MidiParser.*`, `TabMapper.h`, `TablatureComponent.*` | MIDI → табы. |
| `Source/Gameplay/` | Сессия, метроном, scoring, game engine. |
| `Source/Generation/` | Генератор MIDI и аранжировок. |
| `Source/Audio/AudioEngine.*`, `Source/Gameplay/GameController.*`, `Source/UI/SelectComponent.*`, `Source/UI/TunerComponent.*` | Пустые заглушки; не считать их рабочей логикой. |

### Разделение frontend/backend

- **Frontend-агент** владеет `Source/UI/**` и `Source/UI/MainComponentUi.inc`.
  Новые экраны, виджеты, layout, paint и диалоги — только туда.
  Подробные инструкции: `docs/FRONTEND_AGENT.md`.
- **Backend-агент (`juce-dev`)** владеет `MainComponent.cpp`, `MainComponent.h`,
  `Audio/**`, `Network/**`, `Gameplay/**`, `Generation/**`, `Midi/**`,
  `Tuner/**`.
- Frontend не редактирует `MainComponent.cpp`, `MainComponent.h`,
  `Audio/**`, `Network/**`, `Gameplay/**`. Если нужен новый backend-хук,
  backend-агент добавляет объявление в `MainComponent.h` и реализацию в своём
  модуле, после чего frontend вызывает метод.
- `MainComponentUi.inc` включается только из `MainComponent.h` и только внутри
  класса. В нём нет `#pragma once`; не включайте его в другие файлы.

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
