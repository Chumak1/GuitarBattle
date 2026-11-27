param(
    [int]$X = 281,
    [int]$Y = 293
)

$signature = @"
using System;
using System.Runtime.InteropServices;
public class Win32Click {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, IntPtr dwExtraInfo);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
}
"@

if (-not ("Win32Click" -as [type])) { Add-Type -TypeDefinition $signature }

$proc = Get-Process GuitarBattle -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "client not running" }

$hwnd = $proc.MainWindowHandle
[Win32Click]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400

$point = New-Object Win32Click+POINT
$point.X = $X
$point.Y = $Y
[Win32Click]::ClientToScreen($hwnd, [ref]$point) | Out-Null

[Win32Click]::SetCursorPos($point.X, $point.Y) | Out-Null
Start-Sleep -Milliseconds 150
[Win32Click]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # left down
Start-Sleep -Milliseconds 80
[Win32Click]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # left up

Write-Host ("clicked at screen {0},{1}" -f $point.X, $point.Y)
