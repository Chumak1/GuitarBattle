---
name: JUCE Realtime Safety
description: Rules and review checklist for GuitarBattle audio callbacks, pitch thread, and UI-thread handoff
---

# Realtime safety в GuitarBattle

## Потоки

| Поток | Где | Что можно |
|---|---|---|
| Audio callback | `MainComponent::getNextAudioBlock`, `prepareToPlay` | Только заранее выделенные буферы, атомики, lock-free FIFO |
| Pitch worker | `PitchDetectionThread::run` | Анализ, копирование в подготовленные буферы; UI — через `callAsync` |
| Message thread | UI, кнопки, таймеры | Компоненты, диалоги, `juce::URL` (но не блокирующе) |

## Запрещено в audio callback

- `new`/`delete`, `std::vector::resize`, `juce::String`, `juce::Array` с ростом.
- Мьютексы, `CriticalSection`, любые блокировки.
- Файловый и сетевой I/O, `juce::URL`, SQL, логирование.
- `MessageManager::callAsync` (аллокация), `AlertWindow`, `repaint` напрямую.
- Долгие циклы и ожидание.

Правильно: атомарные флаги, `std::atomic`, заранее выделенные `AudioBuffer`, lock-free очередь, `juce::AbstractFifo`.

## UI-thread handoff

- Из audio/pitch thread нельзя трогать `Component`, `Label`, `TextButton`.
- Результат анализа складывай в атомики/структуру и забирай в `timerCallback` либо отправляй `MessageManager::callAsync` из **не-realtime** потока.
- `PitchDetectionThread` — не realtime, ему можно анализ и аллокации, но не прямой доступ к UI.

## Сеть

- Текущие `juce::URL::createInputStream` с таймаутом 4–5 с вызываются из UI-потока и блокируют message thread.
- Новый код: `juce::Thread`/`ThreadPool` → результат в `MessageManager::callAsync`.
- Не выполнять HTTP из `getNextAudioBlock`.
- Не логировать токены и пароли.

## Чек-лист ревью

1. Есть ли в audio callback аллокации/локи/I/O?
2. Все ли общие данные между потоками атомарны или защищены lock-free механизмом?
3. Не трогает ли фоновый поток `Component` напрямую?
4. Не добавлен ли блокирующий HTTP в UI-обработчик?
5. Не возникает ли гонка при загрузке/смене песни во время проигрывания?
6. Корректно ли освобождаются буферы в `releaseResources`?
7. Нет ли `juce::String`/`var` в hot path?

## Известные места

- `MainComponent::getNextAudioBlock`
- `MainComponent::prepareToPlay`
- `MainComponent::releaseResources`
- `PitchDetectionThread::run`
- `MainComponent::timerCallback`
- `MainComponent::loadSongAudio`
- `MainComponent::startOnlineGameSession`
- `MainComponent::submitOnlineGameResult`
- `MainComponent::refreshGlobalLeaderboard`
