[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
if (!$OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot 'Assets' }
Add-Type -AssemblyName System.Drawing
[void][IO.Directory]::CreateDirectory($OutputDirectory)

# Original yarn-ball artwork.
$sizes = [ordered]@{
    'StoreLogo.png' = @(50, 50)
    'Square44x44Logo.png' = @(44, 44)
    'Square71x71Logo.png' = @(71, 71)
    'Square150x150Logo.png' = @(150, 150)
    'Wide310x150Logo.png' = @(310, 150)
    'Square310x310Logo.png' = @(310, 310)
}
foreach ($name in $sizes.Keys) {
    $width, $height = $sizes[$name]
    $bitmap = New-Object Drawing.Bitmap($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([Drawing.ColorTranslator]::FromHtml('#173638'))
    $scale = [Math]::Min($width, $height) / 100.0
    $graphics.TranslateTransform([single](($width - 100 * $scale) / 2), [single](($height - 100 * $scale) / 2))
    $graphics.ScaleTransform([single]$scale, [single]$scale)
    $fill = New-Object Drawing.SolidBrush([Drawing.ColorTranslator]::FromHtml('#E8BB87'))
    $wrap = New-Object Drawing.Pen([Drawing.ColorTranslator]::FromHtml('#855C42'), 2.6)
    $tail = New-Object Drawing.Pen([Drawing.ColorTranslator]::FromHtml('#E8BB87'), 3.8)
    $tail.StartCap = $tail.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $graphics.FillEllipse($fill, 19, 17, 62, 62)
    $graphics.DrawArc($wrap, 23, 15, 45, 63, 280, 160)
    $graphics.DrawArc($wrap, 32, 16, 40, 62, 280, 155)
    $graphics.DrawArc($wrap, 42, 20, 32, 56, 280, 145)
    $graphics.DrawArc($wrap, 21, 27, 63, 25, 5, 170)
    $graphics.DrawArc($wrap, 20, 37, 63, 25, 5, 170)
    $graphics.DrawArc($wrap, 22, 47, 59, 23, 5, 160)
    $graphics.DrawBezier($tail, 66, 73, 83, 84, 49, 93, 82, 88)
    $bitmap.Save((Join-Path $OutputDirectory $name), [Drawing.Imaging.ImageFormat]::Png)
    $tail.Dispose(); $wrap.Dispose(); $fill.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
}
