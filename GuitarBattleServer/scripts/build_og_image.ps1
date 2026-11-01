#Requires -Version 5.1
<#
.SYNOPSIS
    Builds the Open Graph preview images (1200x630 PNG) for the public site.

.DESCRIPTION
    The social preview is a static asset shipped with the API image, so it is
    generated once and committed. Run this script again only when the brand
    changes (logo, colours, tagline).

    Output (both served from the /app static mount):
      app/static/app/og-image.png     Russian card   -> https://fretfight.online/app/og-image.png
      app/static/app/og-image-en.png  English card   -> https://fretfight.online/app/og-image-en.png

    The layout is intentionally quiet: the #070b10 page background, a soft gold
    glow, the logo on the left and the wordmark with the tagline on the right.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build_og_image.ps1
#>
[CmdletBinding()]
param(
    [string]$LogoPath,
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$serverRoot = Split-Path -Parent $PSScriptRoot
if (-not $LogoPath) {
    # Brand source lives next to the server checkout, in the client repository.
    $LogoPath = Join-Path (Split-Path -Parent $serverRoot) 'Logo\Logo.png'
}
if (-not $OutputDir) {
    $OutputDir = Join-Path $serverRoot 'app\static\app'
}

$width = 1200
$height = 630
$background = [System.Drawing.Color]::FromArgb(255, 0x07, 0x0B, 0x10)
$gold = [System.Drawing.Color]::FromArgb(255, 0xF3, 0xC4, 0x51)
$textColor = [System.Drawing.Color]::FromArgb(255, 0xC3, 0xCE, 0xDB)
$mutedColor = [System.Drawing.Color]::FromArgb(255, 0x8D, 0x9C, 0xAB)

$cards = @(
    @{ File = 'og-image.png'; Subtitle = 'Гитарный батл онлайн' },
    @{ File = 'og-image-en.png'; Subtitle = 'Guitar battle online' }
)

function New-Card {
    param(
        [string]$Path,
        [string]$Subtitle,
        [System.Drawing.Image]$Logo
    )

    $bitmap = New-Object System.Drawing.Bitmap($width, $height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality

        $graphics.Clear($background)

        # The card stays flat #070b10: a glow gradient bands visibly on this
        # dark palette, and the logo already carries the only accent colour.

        # Logo: the transparent gold mark, 264 px square.
        $logoSize = 264
        $logoTop = [int](($height - $logoSize) / 2)
        $graphics.DrawImage($Logo, 152, $logoTop, $logoSize, $logoSize)

        # Thin gold rule between the mark and the copy.
        $rulePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(64, 243, 196, 81), 2)
        $graphics.DrawLine($rulePen, 464, 196, 464, 434)
        $rulePen.Dispose()

        $format = New-Object System.Drawing.StringFormat
        $format.Alignment = [System.Drawing.StringAlignment]::Near
        $format.LineAlignment = [System.Drawing.StringAlignment]::Near

        $wordmarkFont = New-Object System.Drawing.Font('Segoe UI', 112, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
        $wordmarkBrush = New-Object System.Drawing.SolidBrush($gold)
        $wordmarkRect = New-Object System.Drawing.RectangleF(508, 176, 620, 150)
        $graphics.DrawString('fretfight', $wordmarkFont, $wordmarkBrush, $wordmarkRect, $format)

        $subtitleFont = New-Object System.Drawing.Font('Segoe UI', 38, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
        $subtitleBrush = New-Object System.Drawing.SolidBrush($textColor)
        $subtitleRect = New-Object System.Drawing.RectangleF(512, 336, 600, 60)
        $graphics.DrawString($Subtitle, $subtitleFont, $subtitleBrush, $subtitleRect, $format)

        $domainFont = New-Object System.Drawing.Font('Segoe UI', 26, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
        $domainBrush = New-Object System.Drawing.SolidBrush($mutedColor)
        $domainRect = New-Object System.Drawing.RectangleF(514, 408, 600, 40)
        $graphics.DrawString('fretfight.online', $domainFont, $domainBrush, $domainRect, $format)

        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)

        foreach ($item in @($wordmarkFont, $wordmarkBrush, $subtitleFont, $subtitleBrush, $domainFont, $domainBrush, $format)) {
            $item.Dispose()
        }
    }
    finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

$logo = [System.Drawing.Image]::FromFile((Resolve-Path $LogoPath).Path)
try {
    foreach ($card in $cards) {
        $target = Join-Path $OutputDir $card.File
        New-Card -Path $target -Subtitle $card.Subtitle -Logo $logo
        $info = Get-Item $target
        Write-Host ("{0} -> {1} ({2} bytes)" -f $card.File, $info.FullName, $info.Length)
    }
}
finally {
    $logo.Dispose()
}
