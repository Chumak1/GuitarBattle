#Requires -Version 5.1
<#
.SYNOPSIS
    Removes the JUCE VST3 restartComponent message-thread assertion.

.DESCRIPTION
    Some VST3 plugins (notably the Neural DSP Archetype series) call
    IComponentHandler::restartComponent from their audio or worker threads.
    JUCE's ComponentRestarter already defers the actual work to the message
    thread, but VST3HostContextHeadless::restartComponent also contains a
    JUCE_ASSERT_MESSAGE_THREAD. In a host application that assertion is only a
    diagnostic aimed at plugin developers, and under a debugger it stops Debug
    builds for no functional reason.

    This script patches the JUCE installation used by GuitarBattle. It is
    idempotent, keeps a .orig backup, and can be reverted with -Revert.

    Run it after installing or updating JUCE 9.0.2. Release builds are not
    affected by the assertion in the first place; the patch only changes what
    Debug builds do when a plugin misbehaves.

.PARAMETER JucePath
    Path to the JUCE directory (the one that contains modules/).

.PARAMETER Revert
    Restore the original header from the .orig backup.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\GuitarBattle\scripts\patch-juce-vst3-restart-assert.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\GuitarBattle\scripts\patch-juce-vst3-restart-assert.ps1 -Revert
#>
[CmdletBinding()]
param(
    [string] $JucePath = 'D:\juce-9.0.2-windows\JUCE',
    [switch] $Revert
)

$ErrorActionPreference = 'Stop'

$relativePath = 'modules\juce_audio_processors_headless\format_types\juce_VST3PluginFormatImpl.h'
$file = Join-Path $JucePath $relativePath
$backup = "$file.orig"

if (-not (Test-Path -LiteralPath $file))
{
    throw "JUCE VST3 header not found: $file"
}

if ($Revert)
{
    if (-not (Test-Path -LiteralPath $backup))
    {
        throw "Backup not found: $backup"
    }

    Copy-Item -LiteralPath $backup -Destination $file -Force
    Write-Host "Reverted: $file"
    exit 0
}

$text = [System.IO.File]::ReadAllText($file)
$usesCrlf = $text.Contains("`r`n")
$normalized = $text.Replace("`r`n", "`n")

$oldBlock = @'
tresult VST3HostContextHeadless::restartComponent (Steinberg::int32 flags)
{
    // If you hit this, the plugin has requested a restart from a thread other than
    // the UI thread. JUCE should be able to cope, but you should consider filing a bug
    // report against the plugin.
    JUCE_ASSERT_MESSAGE_THREAD

    componentRestarter.restart (flags);
    return kResultTrue;
}
'@
$oldBlock = $oldBlock.Replace("`r`n", "`n")

$newBlock = @'
tresult VST3HostContextHeadless::restartComponent (Steinberg::int32 flags)
{
    // GuitarBattle patch: some plugins (e.g. the Neural DSP Archetype series)
    // call restartComponent from their audio or worker threads. ComponentRestarter
    // already defers the work to the message thread, so this is safe. JUCE's
    // JUCE_ASSERT_MESSAGE_THREAD here is a diagnostic aimed at plugin developers;
    // in a host it only stops Debug builds under a debugger for no functional
    // reason, so it is intentionally omitted.
    componentRestarter.restart (flags);
    return kResultTrue;
}
'@
$newBlock = $newBlock.Replace("`r`n", "`n")

if ($normalized.Contains($newBlock))
{
    Write-Host "Already patched: $file"
    exit 0
}

if (-not $normalized.Contains($oldBlock))
{
    throw "restartComponent block not found. The JUCE version may differ: $file"
}

if (-not (Test-Path -LiteralPath $backup))
{
    Copy-Item -LiteralPath $file -Destination $backup
    Write-Host "Backup created: $backup"
}

$patched = $normalized.Replace($oldBlock, $newBlock)

if ($usesCrlf)
{
    $patched = $patched.Replace("`n", "`r`n")
}

[System.IO.File]::WriteAllText($file, $patched)
Write-Host "Patched: $file"
