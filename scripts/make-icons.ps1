# Draws the Glance icon and writes Resources/Glance.icns (Mac) and windows/glance.ico (Windows).
# Run on Windows in PowerShell: scripts/make-icons.ps1 [previewFolder]
# The drawing is code so it stays sharp at every size; small sizes get a simplified version.
param([string]$Preview = "")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName PresentationCore, PresentationFramework, WindowsBase
$root = Split-Path -Parent $PSScriptRoot

function Colour([string]$hex) { [System.Windows.Media.ColorConverter]::ConvertFromString($hex) }
function Brush([string]$hex) { $b = New-Object System.Windows.Media.SolidColorBrush (Colour $hex); $b.Freeze(); $b }
function Point([double]$x, [double]$y) { [System.Windows.Point]::new($x, $y) }

# Everything is drawn on a 1024 canvas. Mac: the standard 824 rounded square with a shadow.
# Windows: the square fills more of the canvas, no shadow (Windows draws none behind icons).
function Render([int]$size, [bool]$mac) {
    $small = $size -le 32
    $visual = New-Object System.Windows.Media.DrawingVisual
    $dc = $visual.RenderOpen()
    $dc.PushTransform(([System.Windows.Media.ScaleTransform]::new($size / 1024.0, $size / 1024.0)))

    if ($mac) { $inset = 100; $radius = 185 } else { $inset = if ($small) { 0 } else { 32 }; $radius = if ($small) { 200 } else { 210 } }
    $body = [System.Windows.Rect]::new($inset, $inset, 1024 - 2 * $inset, 1024 - 2 * $inset)
    $background = [System.Windows.Media.LinearGradientBrush]::new((Colour "#4C8DFF"), (Colour "#6B3FE6"), (Point 0 0), (Point 1 1))
    if ($mac) {
        # Soft shadow under the square, as macOS icons have.
        foreach ($i in 1..14) {
            $spread = $i * 3.0
            $shadow = [System.Windows.Rect]::new($body.X - $spread / 2, $body.Y + 10 + $spread / 3, $body.Width + $spread, $body.Height + $spread / 2)
            $dc.DrawRoundedRectangle((Brush ("#{0:X2}000000" -f [int](9 - $i * 0.55))), $null, $shadow, $radius + $spread / 2, $radius + $spread / 2)
        }
    }
    $dc.DrawRoundedRectangle($background, $null, $body, $radius, $radius)
    # A faint light from the top.
    $sheen = [System.Windows.Media.LinearGradientBrush]::new((Colour "#30FFFFFF"), (Colour "#00FFFFFF"), (Point 0.5 0), (Point 0.5 0.6))
    $dc.DrawRoundedRectangle($sheen, $null, $body, $radius, $radius)

    # The eye: a white almond, scaled up a little when there are few pixels.
    $k = if ($small) { 1.27 } else { 1.08 }
    $half = 300 * $k; $lift = 205 * $k
    $eye = New-Object System.Windows.Media.StreamGeometry
    $ctx = $eye.Open()
    $ctx.BeginFigure((Point (512 - $half) 512), $true, $true)
    $ctx.BezierTo((Point (512 - $half * 0.5) (512 - $lift)), (Point (512 + $half * 0.5) (512 - $lift)), (Point (512 + $half) 512), $true, $true)
    $ctx.BezierTo((Point (512 + $half * 0.5) (512 + $lift)), (Point (512 - $half * 0.5) (512 + $lift)), (Point (512 - $half) 512), $true, $true)
    $ctx.Close()
    $eyeShadow = [System.Windows.Media.TranslateTransform]::new(0, 14)
    $dc.PushTransform($eyeShadow)
    $dc.DrawGeometry((Brush "#26000040"), $null, $eye)
    $dc.Pop()
    $dc.DrawGeometry((Brush "#FFFFFF"), $null, $eye)

    # The iris is a photo: sky, sun and hills, inside a dark ring.
    $r = 138 * $k
    $iris = [System.Windows.Media.EllipseGeometry]::new((Point 512 512), $r, $r)
    $dc.PushClip($iris)
    $sky = [System.Windows.Media.LinearGradientBrush]::new((Colour "#2E9BFF"), (Colour "#9FE0FF"), (Point 0.5 0), (Point 0.5 1))
    $dc.DrawRectangle($sky, $null, ([System.Windows.Rect]::new(300, 300, 424, 424)))
    if (-not $small) { $dc.DrawEllipse((Brush "#FFD84D"), $null, (Point 566 462), 34, 34) }
    $hills = New-Object System.Windows.Media.StreamGeometry
    $ctx = $hills.Open()
    $ctx.BeginFigure((Point 360 700), $true, $true)
    $ctx.LineTo((Point 360 560), $true, $true)
    $ctx.LineTo((Point 450 488), $true, $true)
    $ctx.LineTo((Point 512 548), $true, $true)
    $ctx.LineTo((Point 590 476), $true, $true)
    $ctx.LineTo((Point 680 560), $true, $true)
    $ctx.LineTo((Point 680 700), $true, $true)
    $ctx.Close()
    $dc.DrawGeometry(([System.Windows.Media.LinearGradientBrush]::new((Colour "#1FA67A"), (Colour "#0E6B57"), (Point 0.5 0), (Point 0.5 1))), $null, $hills)
    $dc.Pop()
    $ring = [System.Windows.Media.Pen]::new((Brush "#1E2A78"), $(if ($small) { 34 } else { 22 }))
    $dc.DrawEllipse($null, $ring, (Point 512 512), $r, $r)
    # The glint that makes it read as an eye.
    $glint = if ($small) { 30 } else { 24 }
    $dc.DrawEllipse((Brush "#F0FFFFFF"), $null, (Point (512 - $r * 0.42) (512 - $r * 0.42)), $glint, $glint)

    $dc.Pop()
    $dc.Close()
    $bitmap = [System.Windows.Media.Imaging.RenderTargetBitmap]::new($size, $size, 96, 96, [System.Windows.Media.PixelFormats]::Pbgra32)
    $bitmap.Render($visual)
    $encoder = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
    $encoder.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $stream = New-Object System.IO.MemoryStream
    $encoder.Save($stream)
    , $stream.ToArray()
}

function BigEndian([int]$value) { $b = [BitConverter]::GetBytes([int]$value); [Array]::Reverse($b); , $b }

# Mac: an .icns of PNGs (the types macOS 10.7+ reads).
$icns = New-Object System.IO.MemoryStream
$entries = @(@("icp4", 16), @("icp5", 32), @("icp6", 64), @("ic07", 128), @("ic08", 256), @("ic09", 512), @("ic10", 1024),
             @("ic11", 32), @("ic12", 64), @("ic13", 256), @("ic14", 512))
$body = New-Object System.IO.MemoryStream
foreach ($e in $entries) {
    $png = Render $e[1] $true
    $type = [Text.Encoding]::ASCII.GetBytes($e[0])
    $body.Write($type, 0, 4); $len = BigEndian ($png.Length + 8); $body.Write($len, 0, 4); $body.Write($png, 0, $png.Length)
}
$magic = [Text.Encoding]::ASCII.GetBytes("icns"); $icns.Write($magic, 0, 4)
$len = BigEndian ($body.Length + 8); $icns.Write($len, 0, 4); $body.WriteTo($icns)
[IO.File]::WriteAllBytes((Join-Path $root "Resources\Glance.icns"), $icns.ToArray())

# Windows: an .ico of PNGs, every size Explorer and the taskbar ask for.
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 256)
$images = foreach ($s in $sizes) { , (Render $s $false) }
$ico = New-Object System.IO.MemoryStream
$w = [System.IO.BinaryWriter]::new($ico)
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$images[$i].Length); $w.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
[IO.File]::WriteAllBytes((Join-Path $root "windows\glance.ico"), $ico.ToArray())

if ($Preview) {
    New-Item -ItemType Directory -Force $Preview | Out-Null
    foreach ($s in 1024, 256, 64, 32, 24, 16) {
        [IO.File]::WriteAllBytes((Join-Path $Preview "mac-$s.png"), (Render $s $true))
        [IO.File]::WriteAllBytes((Join-Path $Preview "win-$s.png"), (Render $s $false))
    }
}
"Wrote Resources\Glance.icns and windows\glance.ico"
