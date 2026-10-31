# Патчи JUCE

Клиент собирается против внешней установки JUCE 9.0.2
(`D:\juce-9.0.2-windows\JUCE`). Некоторые исправления нельзя сделать в
`GuitarBattle/Source`, поэтому они применяются к самой установке JUCE
скриптом. Скрипты идемпотентны и не входят в сборку.

## VST3: ложный assert в restartComponent

**Файл:** `modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h`
**Функция:** `VST3HostContextHeadless::restartComponent`
**Скрипт:** `GuitarBattle/scripts/patch-juce-vst3-restart-assert.ps1`

### Симптом

В Debug-сборке под отладчиком при загрузке Neural DSP Archetype Cory Wong /
Gojira появляется:

```
JUCE Assertion failure in juce_VST3PluginFormatImpl.h:3523
```

и отладчик останавливается на `__debugbreak()`.

### Причина

Плагин вызывает `IComponentHandler::restartComponent` не из message thread
(обычно из аудио- или worker-потока). JUCE это поддерживает:
`ComponentRestarter` накапливает флаги и через `AsyncUpdater` переносит
`restartComponentOnMessageThread()` в message thread. Строка 3523 —
`JUCE_ASSERT_MESSAGE_THREAD` — диагностика для разработчиков плагинов, а не
требование хоста. Сам JUCE в комментарии пишет: «JUCE should be able to cope,
but you should consider filing a bug report against the plugin».

Release-сборки этот assert не затрагивает.

### Что делает патч

Убирает `JUCE_ASSERT_MESSAGE_THREAD` из `restartComponent`, оставляя
`componentRestarter.restart (flags)`. Поведение не меняется: работа по-прежнему
выполняется в message thread.

### Применение

```powershell
powershell -ExecutionPolicy Bypass -File `
  "D:\GuitarBattle\GuitarBattle\scripts\patch-juce-vst3-restart-assert.ps1"
```

Скрипт создаёт рядом с файлом резервную копию `.orig` и при повторном запуске
ничего не делает. Если JUCE переустановлен или обновлён — запустите скрипт
снова. Если JUCE лежит в другом каталоге, передайте его через `-JucePath`.
После применения или отката пересоберите проект: MSBuild перекомпилирует
`include_juce_audio_processors_headless.cpp`.

### Откат

```powershell
powershell -ExecutionPolicy Bypass -File `
  "D:\GuitarBattle\GuitarBattle\scripts\patch-juce-vst3-restart-assert.ps1" -Revert
```

### Проверка

```powershell
Select-String -Path `
  "D:\juce-9.0.2-windows\JUCE\modules\juce_audio_processors_headless\format_types\juce_VST3PluginFormatImpl.h" `
  -Pattern "JUCE_ASSERT_MESSAGE_THREAD"
```

В `restartComponent` не должно остаться строки с макросом (упоминание в
комментарии допустимо).
