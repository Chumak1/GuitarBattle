---
name: GuitarBattle Build
description: Build and troubleshoot the JUCE GuitarBattle client with Projucer and Visual Studio 2026 / MSBuild
---

# Сборка GuitarBattle

## Когда использовать

При сборке клиента, ошибках MSVC/MSBuild, добавлении или удалении `.cpp`/`.h`, проблемах с JUCE-модулями.

## Пути

- Проект: `D:\GuitarBattle\GuitarBattle`
- Projucer: `D:\juce-9.0.2-windows\JUCE\Projucer.exe`
- JUCE-модули: `D:\juce-9.0.2-windows\JUCE\modules`
- Solution: `D:\GuitarBattle\GuitarBattle\Builds\VisualStudio2026\GuitarBattle.sln`
- MSBuild: `D:\Visual Studio\Main\MSBuild\Current\Bin\MSBuild.exe`
- Выход: `Builds\VisualStudio2026\x64\Debug\App\GuitarBattle.exe`

## Команды

```powershell
# Перегенерировать VS-проект из .jucer
& "D:\juce-9.0.2-windows\JUCE\Projucer.exe" --resave "D:\GuitarBattle\GuitarBattle\GuitarBattle.jucer"

# Debug x64
& "D:\Visual Studio\Main\MSBuild\Current\Bin\MSBuild.exe" `
  "D:\GuitarBattle\GuitarBattle\Builds\VisualStudio2026\GuitarBattle.sln" `
  /p:Configuration=Debug /p:Platform=x64 /m

# Release x64
& "D:\Visual Studio\Main\MSBuild\Current\Bin\MSBuild.exe" `
  "D:\GuitarBattle\GuitarBattle\Builds\VisualStudio2026\GuitarBattle.sln" `
  /p:Configuration=Release /p:Platform=x64 /m
```

Перед сборкой закрой запущенный `GuitarBattle.exe`, иначе MSBuild не сможет перезаписать файл.

## Патчи JUCE

Установка JUCE пропатчена скриптом
`GuitarBattle\scripts\patch-juce-vst3-restart-assert.ps1`: удалён ложный
`JUCE_ASSERT_MESSAGE_THREAD` в `VST3HostContextHeadless::restartComponent`.
Neural DSP и подобные плагины вызывают `restartComponent` не из message
thread, а JUCE штатно переносит работу через `ComponentRestarter`. После
переустановки или обновления JUCE запусти скрипт снова; откат — `-Revert`.
Подробности: `docs/JUCE_PATCHES.md`.

## Иконка приложения

Иконка задаётся в `.jucer`, а не правкой `Builds/VisualStudio2026/resources.rc`
(файл генерируется и будет перезаписан):

1. Картинка лежит в `Source/UI/Assets/AppIcon.png` (600×600) и описана в группе
   `Assets`: `<FILE id="AppIc1" name="AppIcon.png" compile="0" resource="1" .../>`.
2. У экспортёра: `<VS2026 targetFolder="Builds/VisualStudio2026" smallIcon="AppIc1" bigIcon="AppIc1">`.
3. `--resave` создаёт `Builds/VisualStudio2026/icon.ico` (16/32/48/256) и добавляет
   `IDI_ICON1/2 ICON DISCARDABLE "icon.ico"` в `resources.rc`. Оба файла нужны в Git:
   `resources.rc` ссылается на `icon.ico`.
4. Окно и таскбар используют ту же картинку через `BrandAssets::createLogoImage()`
   (BinaryData `AppIcon_png`), поэтому иконка ярлыка и запущенного приложения совпадает.

## Типовые ошибки

| Симптом | Причина | Что делать |
|---|---|---|
| `cannot open include file 'JuceHeader.h'` | Неверные `MODULEPATH` или проект не перегенерирован | Проверить пути в `.jucer`, выполнить `--resave` |
| `LNK2019` на функции из `Generation` | `.cpp` не входит в проект | Он включён через `#include` в `MainComponent.cpp`; не добавлять повторно |
| `C2084`/duplicate symbols | Файл одновременно в `.vcxproj` и включён через `#include` | Убрать один из способов |
| `MSB8020`/не найден toolset | Нет компонента VS 2026 C++ | Установить workload «Разработка классических приложений на C++» |
| `LNK1104 cannot open file ...exe` | Приложение запущено | Закрыть процесс |
| Ошибки после `--resave` | Projucer перезаписал ручные правки `.vcxproj` | Правки делать в `.jucer`, а не в `.vcxproj` |

## Правила

1. Источник истины — `GuitarBattle.jucer`. После добавления файла добавь его в `<FILE .../>` и перегенерируй проект.
2. `MainComponent.cpp` включает `Generation/MidiMelodyGenerator.cpp` и `Generation/SongArrangement.cpp`. Не добавляй их в проект, пока не удалил `#include`.
3. `PitchDetectionThread.h` — header-only реализация, не добавляй его отдельным compile unit.
4. Не редактируй `Builds/**/x64` и не коммить артефакты.
5. После сборки проверь, что `GuitarBattle.exe` обновился по времени.

## Проверка

```powershell
Get-Item "D:\GuitarBattle\GuitarBattle\Builds\VisualStudio2026\x64\Debug\App\GuitarBattle.exe" |
  Select-Object Name, Length, LastWriteTime
```
