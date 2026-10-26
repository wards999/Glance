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
# Rotate by `angle`, scale by `scale`, then move the centre to (x, y).
function Placed([double]$angle, [double]$x, [double]$y, [double]$scale) {
    $g = [System.Windows.Media.TransformGroup]::new()
    $g.Children.Add([System.Windows.Media.RotateTransform]::new($angle))
    $g.Children.Add([System.Windows.Media.ScaleTransform]::new($scale, $scale))
    $g.Children.Add([System.Windows.Media.TranslateTransform]::new($x, $y))
    $g
}

# Everything is drawn on a 1024 canvas. Mac: the standard 824 rounded square with a shadow.
# Windows: the square fills more of the canvas, no shadow (Windows draws none behind icons).
function Render([int]$size, [bool]$mac) {
    $small = $size -le 32
    $visual = New-Object System.Windows.Media.DrawingVisual
    $dc = $visual.RenderOpen()
    $dc.PushTransform(([System.Windows.Media.ScaleTransform]::new($size / 1024.0, $size / 1024.0)))

    if ($mac) { $inset = 100; $radius = 185 } else { $inset = if ($small) { 0 } else { 32 }; $radius = if ($small) { 200 } else { 210 } }
    $body = [System.Windows.Rect]::new($inset, $inset, 1024 - 2 * $inset, 1024 - 2 * $inset)
    $background = [System.Windows.Media.LinearGradientBrush]::new((Colour "#FF9A5A"), (Colour "#F2457A"), (Point 0 0), (Point 1 1))
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

    # Two prints, one tilted behind the other. The artwork is laid out for the Windows square
    # (960) and scaled to the Mac's (824); small sizes get bigger prints with thicker borders.
    $k = ($body.Width / 960.0) * $(if ($small) { 1.1 } else { 1.0 })
    $card = [System.Windows.Rect]::new(-290, -225, 580, 450)
    $border = if ($small) { 52 } else { 40 }
    $photo = [System.Windows.Rect]::new(-290 + $border, -225 + $border, 580 - 2 * $border, 450 - 2 * $border)

    $dc.PushTransform((Placed -11 (512 + (490 - 512) * $k) (512 + (500 - 512) * $k) $k))
    $dc.DrawRoundedRectangle((Brush "#A0FFFFFF"), $null, $card, 40, 40)
    $dc.Pop()

    $dc.PushTransform((Placed 5 (512 + (530 - 512) * $k) (512 + (540 - 512) * $k) $k))
    $dc.PushTransform([System.Windows.Media.TranslateTransform]::new(0, 12))
    $dc.DrawRoundedRectangle((Brush "#30701030"), $null, $card, 40, 40)  # the front print's shadow
    $dc.Pop()
    $dc.DrawRoundedRectangle((Brush "#FFFFFF"), $null, $card, 40, 40)
    # The picture on it: sky, sun and hills.
    $dc.PushClip([System.Windows.Media.RectangleGeometry]::new($photo, 18, 18))
    $sky = [System.Windows.Media.LinearGradientBrush]::new((Colour "#3FA2FF"), (Colour "#A8E3FF"), (Point 0.5 0), (Point 0.5 1))
    $dc.DrawRectangle($sky, $null, $photo)
    $x = $photo.X; $y = $photo.Y; $w = $photo.Width; $h = $photo.Height
    $dc.DrawEllipse((Brush "#FFD54A"), $null, (Point ($x + 0.7 * $w) ($y + 0.3 * $h)), (0.11 * $w), (0.11 * $w))
    $hills = New-Object System.Windows.Media.StreamGeometry
    $ctx = $hills.Open()
    $ctx.BeginFigure((Point $x ($y + $h)), $true, $true)
    foreach ($p in @(@(0, 0.74), @(0.3, 0.42), @(0.52, 0.66), @(0.7, 0.5), @(1, 0.78), @(1, 1))) {
        $ctx.LineTo((Point ($x + $p[0] * $w) ($y + $p[1] * $h)), $true, $true)
    }
    $ctx.Close()
    $dc.DrawGeometry((Brush "#18A77C"), $null, $hills)
    $dc.Pop()
    $dc.Pop()


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
