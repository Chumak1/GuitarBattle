param(
    [string]$Source = "D:\GuitarBattle\docs\screenshots",
    [string]$Target = "D:\GuitarBattle\docs\screenshots\upload",
    [int]$Width = 1600,
    [int]$Quality = 90,
    [string[]]$Names = @("home_ru", "towers_ru", "tower_ru", "gameplay4", "tuner_ru")
)

Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Path $Target -Force | Out-Null

$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
    Where-Object { $_.MimeType -eq "image/jpeg" }
$params = New-Object System.Drawing.Imaging.EncoderParameters 1
$params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
    [System.Drawing.Imaging.Encoder]::Quality, [long]$Quality)

foreach ($name in $Names) {
    $input = Join-Path $Source ($name + ".png")
    if (-not (Test-Path $input)) { Write-Host "skip $name"; continue }

    $image = [System.Drawing.Image]::FromFile($input)
    $height = [int][math]::Round($image.Height * ($Width / $image.Width))
    $bitmap = New-Object System.Drawing.Bitmap $Width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.DrawImage($image, 0, 0, $Width, $height)
    $graphics.Dispose()
    $image.Dispose()

    $output = Join-Path $Target ($name + ".jpg")
    $bitmap.Save($output, $codec, $params)
    $bitmap.Dispose()

    $size = [int]((Get-Item $output).Length / 1024)
    Write-Host ("{0} -> {1}x{2}, {3} KB" -f $name, $Width, $height, $size)
}
