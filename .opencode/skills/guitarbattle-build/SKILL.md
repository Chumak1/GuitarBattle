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
