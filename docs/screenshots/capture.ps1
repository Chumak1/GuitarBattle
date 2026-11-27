param(
    [string]$Exe = "D:\GuitarBattle\GuitarBattle\Builds\VisualStudio2026\x64\Debug\App\GuitarBattle.exe",
    [string]$OutDir = "D:\GuitarBattle\docs\screenshots",
    [string]$Name = "shot",
    [int]$Width = 1920,
    [int]$Height = 1080,
    [switch]$NoResize
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$signature = @"
using System;
using System.Runtime.InteropServices;
public class Win32Shot {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hWnd, int X, int Y, int nWidth, int nHeight, bool repaint);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

if (-not ("Win32Shot" -as [type])) { Add-Type -TypeDefinition $signature }

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$proc = Get-Process GuitarBattle -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) {
    Write-Host "starting client"
    Start-Process -FilePath $Exe | Out-Null
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 700
        $proc = Get-Process GuitarBattle -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if ($proc) { break }
    }
}

if (-not $proc) { throw "client window not found" }

$hwnd = $proc.MainWindowHandle
[Win32Shot]::ShowWindow($hwnd, 9) | Out-Null   # restore
if (-not $NoResize) {
    [Win32Shot]::MoveWindow($hwnd, 40, 40, $Width, $Height, $true) | Out-Null
}
[Win32Shot]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 1500

$rect = New-Object Win32Shot+RECT
[Win32Shot]::GetClientRect($hwnd, [ref]$rect) | Out-Null
$origin = New-Object Win32Shot+POINT
[Win32Shot]::ClientToScreen($hwnd, [ref]$origin) | Out-Null

$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { throw "empty client area" }

$bmp = New-Object System.Drawing.Bitmap $w, $h
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($origin.X, $origin.Y, 0, 0, (New-Object System.Drawing.Size $w, $h))
$gfx.Dispose()

$path = Join-Path $OutDir ($Name + ".png")
$bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

Write-Host ("saved {0} ({1}x{2})" -f $path, $w, $h)
