# Frontend-агент GuitarBattle — правила работы

Документ для второго AI-агента, который работает **только с frontend** клиента
`GuitarBattle/` (C++/JUCE 9.0.2). Backend-часть клиента (аудио, сеть, геймплей)
ведёт основной агент `juce-dev`. Сервер (`GuitarBattleServer/`) — зона
`server-dev` / `api-sync`.

Главное правило: **не редактируй файлы вне своей зоны**. Если задача требует
изменений в аудио, сети или геймплее — опиши контракт и передай её
backend-агенту. Не «дописывай по пути» в чужие файлы.

---

## 1. Зона ответственности

### Твои файлы (frontend)

| Файл | Что внутри |
|---|---|
| `GuitarBattle/Source/UI/MainComponentUI.cpp` | Вся UI-часть `MainComponent`: `initialiseUi()`, `resized()`, `paint()`, `paintOverlay()`, `paintModalOverlay()`, диалоги, `AudioSettingsPanel`, UI-хелперы. |
| `GuitarBattle/Source/UI/MainComponentUi.inc` | Точка расширения. Новые виджеты, frontend-состояние и объявления UI-хелперов. Включается **внутрь класса** `MainComponent`. |
| `GuitarBattle/Source/UI/GameStyle.h/.cpp` | Тема, цвета, шрифты, общие UI-хелперы. |
| `GuitarBattle/Source/UI/` (остальное) | Новые UI-компоненты, панели, оверлеи. |

### Файлы backend-агента (не редактировать)

| Файл | Что внутри |
|---|---|
| `GuitarBattle/Source/MainComponent.cpp` | Composition root: конструктор, `timerCallback()`, деструктор. |
| `GuitarBattle/Source/MainComponent.h` | Объявления класса, состояние игры, аудио и сети. |
| `GuitarBattle/Source/Audio/MainComponentAudio.cpp` | Аудио-устройство, `getNextAudioBlock()`, банки сэмплов, выбор входа. |
| `GuitarBattle/Source/Network/MainComponentNetwork.cpp` | Авторизация, каталог, лидерборд, скачивание песен, онлайн-сессии. |
| `GuitarBattle/Source/Network/MainComponentAssets.cpp` | Жизненный цикл серверного sound-pack. |
| `GuitarBattle/Source/Gameplay/MainComponentGameplay.cpp` | Сессия, тюнинг, MIDI-цели, scoring. |
| `GuitarBattle/Source/Audio/PitchDetectionThread.h` | Worker-поток анализа питча. |
| `GuitarBattle/Source/Generation/`, `Midi/`, `Tuner/` | Генерация, MIDI-парсер, тюнер. |

### Не трогать никогда

- `GuitarBattleServer/`, `migrations/`, `docker-compose.yml`, `.env`.
- `Backup Server/`, `GuitarBattle/Backup/`, `Builds/**/x64`, `Source.zip`.
- Сгенерированные бинарники и аудио-ассеты.

---

## 2. Как устроен `MainComponent` после разделения

`MainComponent` остаётся одним классом, но его реализация разложена по файлам:

```
Source/
  MainComponent.h              объявления класса + #include "UI/MainComponentUi.inc"
  MainComponent.cpp            конструктор, timerCallback, деструктор
  UI/
    MainComponentUI.cpp        initialiseUi(), layout, paint, dialogs, UI helpers
    MainComponentUi.inc        ← твоя точка расширения (внутри класса)
    GameStyle.h/.cpp           тема
  Audio/
    MainComponentAudio.cpp     audio device, getNextAudioBlock, sample banks
  Network/
    MainComponentNetwork.cpp   auth, catalogue, leaderboard, downloads
    MainComponentAssets.cpp    server sound-pack lifecycle
  Gameplay/
    MainComponentGameplay.cpp  session, tuning, MIDI targets, scoring
```

Конструктор `MainComponent` вызывает:

```cpp
initialiseUi();    // определено в UI/MainComponentUI.cpp
initialiseAudio(); // определено в Audio/MainComponentAudio.cpp
```

`initialiseUi()` содержит весь код настройки UI, который раньше был в
конструкторе. Добавляй новые экраны и контролы **туда**, а не в
`MainComponent.cpp`.

`MainComponentUi.inc` включается в private-секцию класса перед
`JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`. Поэтому:

- в `.inc` пишутся только объявления членов класса;
- **нельзя** добавлять `#pragma once` или include guard;
- **нельзя** включать `.inc` в другие файлы;
- `.inc` не является отдельной единицей компиляции.

---

## 3. Как добавить новый виджет (пошагово)

### Шаг 1. Объявить в `UI/MainComponentUi.inc`

```cpp
// Новый экран/контрол
juce::Label newFeatureLabel;
juce::TextButton newFeatureButton { "NEW FEATURE" };
bool newFeatureOpen = false;

void initialiseNewFeature();
void layoutNewFeature();
```

### Шаг 2. Реализовать в `UI/MainComponentUI.cpp`

```cpp
void MainComponent::initialiseNewFeature()
{
    newFeatureLabel.setText("Hello", juce::dontSendNotification);
    newFeatureLabel.setFont(GameUi::heading(18.0f));
    addAndMakeVisible(newFeatureLabel);

    newFeatureButton.onClick = [this]
    {
        newFeatureOpen = ! newFeatureOpen;
        resized();
        repaint();
    };
    addAndMakeVisible(newFeatureButton);
}
```

### Шаг 3. Вызвать из `initialiseUi()`

В конце `MainComponent::initialiseUi()` добавь:

```cpp
initialiseNewFeature();
```

### Шаг 4. Разложить в `MainComponent::resized()`

Добавь блок layout в `MainComponentUI.cpp`:

```cpp
if (newFeatureOpen)
{
    newFeatureLabel.setBounds(...);
    newFeatureButton.setBounds(...);
}
```

### Шаг 5. При необходимости — отрисовка

Для декоративной отрисовки используй `paintOverlay()` (слой над фоном) или
`paint()` (фон). Не дублируй отрисовку в `paintOverlay()` и `paint()`.

### Шаг 6. Собрать

```powershell
& "D:\Visual Studio\Main\MSBuild\Current\Bin\MSBuild.exe" `
  "D:\GuitarBattle\GuitarBattle\Builds\VisualStudio2026\GuitarBattle.sln" `
  /p:Configuration=Debug /p:Platform=x64 /m
```

Если добавлял новый `.cpp`/`.h` — сначала обнови `.jucer` (см. §6).

---

## 4. Как frontend общается с backend

`MainComponentUI.cpp` определяет методы `MainComponent::...`, поэтому имеет
доступ ко всем private-членам класса. Это позволяет вызывать существующие
backend-методы напрямую:

```cpp
pauseGame();
resumeGame();
returnToSongSelect();
startSelectedSong();
setPreferredInputChannel(channel);
```

Но помни:

- **Не вызывай блокирующие сетевые операции из UI-потока.** Существующий
  сетевой код уже вынесен в фоновые потоки и возвращает результат через
  `MessageManager::callAsync`. Новый сетевой код в UI-файлах не пиши.
- **Не трогай аудио-буферы и `getNextAudioBlock()`.** UI может менять только
  атомарные параметры (громкость, скорость), которые уже предусмотрены.
- **Если нужен новый backend-хук** (например, «начать новую сессию с
  параметром X») — не добавляй его сам в `MainComponent.h`. Опиши задачу
  backend-агенту: он добавит объявление в `MainComponent.h` и реализацию в
  нужном модуле. После этого frontend просто вызывает метод.

Контракт «frontend → backend»: существующие методы `MainComponent` и
атомарные поля. Контракт «backend → frontend»: backend обновляет UI-поля
(лейблы, флаги) на message thread; frontend читает их в `resized()`/`paint()`.

---

## 5. Чего нельзя делать

1. **Не редактируй `MainComponent.cpp`.** Это composition root. Если нужно
   добавить setup UI — используй `initialiseUi()`.
2. **Не редактируй `MainComponent.h` без крайней необходимости.** Новые
   виджеты и UI-состояние — в `.inc`. Если без правки заголовка не обойтись
   (например, новый `override`), согласуй с backend-агентом.
3. **Не редактируй `Audio/`, `Network/`, `Gameplay/`, `Generation/`, `Midi/`,
   `Tuner/`.** Даже «одну строчку».
4. **Не добавляй `#include "UI/MainComponentUi.inc"` нигде, кроме
   `MainComponent.h`.**
5. **Не добавляй `MidiMelodyGenerator.cpp` и `SongArrangement.cpp` в
   `.jucer`.** Они компилируются только через
   `Gameplay/MainComponentGameplay.cpp`.
6. **Не добавляй `PitchDetectionThread.h` в `.jucer` как compile unit.**
7. **Не блокируй message thread** длительными операциями, `Thread::sleep`,
   синхронным сетевым I/O, `AlertWindow::showMessageBox` в цикле.
8. **Не коммить** `.env`, `Builds/**/x64`, `Backup*`, `Source.zip`.

---

## 6. Проектные файлы и сборка

Источник истины — `GuitarBattle/GuitarBattle.jucer`. `.vcxproj` генерируется
Projucer.

### Если менял только существующие `.cpp`/`.h`/`.inc`

```powershell
& "D:\Visual Studio\Main\MSBuild\Current\Bin\MSBuild.exe" `
  "D:\GuitarBattle\GuitarBattle\Builds\VisualStudio2026\GuitarBattle.sln" `
  /p:Configuration=Debug /p:Platform=x64 /m
```

### Если добавил/удалил файл

1. Добавь файл в `.jucer` в группу `UI` (только UI-файлы!).
   - `.cpp` → `compile="1"`;
   - `.h`/`.inc` → `compile="0"`.
2. Пересобери проект:

```powershell
& "D:\juce-9.0.2-windows\JUCE\Projucer.exe" --resave `
  "D:\GuitarBattle\GuitarBattle\GuitarBattle.jucer"
```

3. Собери Debug x64.
4. Закоммить `.jucer`, `.vcxproj`, `.vcxproj.filters` и новые файлы.

> `.vcxproj` и `.vcxproj.filters` — сгенерированные файлы, но они в Git.
> Не правь их вручную; меняй `.jucer` и делай `--resave`.

---

## 7. Git-процесс

- Ветка: `frontend/<короткое-имя>`.
- Перед началом: `git fetch origin && git rebase origin/main`.
- Коммить только свои файлы + изменения `.jucer`/`.vcxproj`, вызванные
  добавлением UI-файлов.
- Перед коммитом обязательно собери Debug x64.
- Не делай force-push в `main`.
- Если конфликт в `MainComponent.h` — не разрешай его сам, согласуй с
  backend-агентом.

---

## 8. Чек-лист перед завершением задачи

- [ ] Сборка Debug x64 без новых ошибок/warnings.
- [ ] Приложение запускается, открывается экран авторизации.
- [ ] Новый UI не блокирует message thread.
- [ ] Аудио-поток не изменён (нет новых аллокаций/локов/I/O в
      `getNextAudioBlock`).
- [ ] Новые виджеты объявлены в `.inc`, а не в `MainComponent.h`.
- [ ] Если добавлял файлы — `.jucer` обновлён и `--resave` выполнен.
- [ ] `git status` не содержит `.env`, `Builds/**/x64`, `Backup*`.

---

## 9. Полезные ориентиры

- `MainComponent::initialiseUi()` — единственная точка входа для UI-setup.
- `MainComponent::resized()` — layout.
- `MainComponent::paintOverlay()` — декоративный слой над фоном.
- `MainComponent::paintModalOverlay()` — модальные оверлеи.
- `GameUi::` / `UiColors::` в `UI/GameStyle.h` — шрифты, отступы, цвета.
- `MainComponent::showHitFeedback()` / `updateHitFeedbackAnimation()` —
  анимация попаданий.
- `MainComponent::updateSongBestScoreLabels()` — обновление рекордов в UI.

Если сомневаешься, к какому слою относится изменение, — спроси backend-агента
до правки, а не после.
