# Draws the Glance icons: Resources/Glance.icns (Mac), windows/glance.ico (Windows), and the
# Windows file icons for pictures, PDFs and videos (windows/picture.ico, pdf.ico, video.ico).
# Run on Windows in PowerShell: scripts/make-icons.ps1 [previewFolder]
# The drawing is code so it stays sharp at every size; small sizes get a simplified version.
param([string]$Preview = "")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName PresentationCore, PresentationFramework, WindowsBase
$root = Split-Path -Parent $PSScriptRoot

function Colour([string]$hex) { [System.Windows.Media.ColorConverter]::ConvertFromString($hex) }
function Brush([string]$hex) { $b = New-Object System.Windows.Media.SolidColorBrush (Colour $hex); $b.Freeze(); $b }
function Point([double]$x, [double]$y) { [System.Windows.Point]::new($x, $y) }
function Vertical([string]$top, [string]$bottom) {
    [System.Windows.Media.LinearGradientBrush]::new((Colour $top), (Colour $bottom), (Point 0.5 0), (Point 0.5 1))
}
# A closed shape through points given relative to (cx, cy), in units of `unit`.
function Shape([double]$cx, [double]$cy, [double]$unit, $points) {
    $g = New-Object System.Windows.Media.StreamGeometry
    $ctx = $g.Open()
    $ctx.BeginFigure((Point ($cx + $points[0][0] * $unit) ($cy + $points[0][1] * $unit)), $true, $true)
    foreach ($p in $points[1..($points.Count - 1)]) { $ctx.LineTo((Point ($cx + $p[0] * $unit) ($cy + $p[1] * $unit)), $true, $true) }
    $ctx.Close()
    $g
}

# Everything is drawn on a 1024 canvas. Mac: the standard 824 rounded square with a shadow.
# Windows: the square fills more of the canvas, no shadow (Windows draws none behind icons).
# The icon is a lens with a lightning bolt: a quick look. File icons are the same icon with the
# glass tinted by kind (green pictures, red PDFs, violet videos) and, from 40 px, a corner badge
# showing what's in the file: a landscape, a page, or a play button.
function Render([int]$size, [bool]$mac, [string]$kind = "app") {
    $small = $size -le 32
    $visual = New-Object System.Windows.Media.DrawingVisual
    $dc = $visual.RenderOpen()
    $dc.PushTransform(([System.Windows.Media.ScaleTransform]::new($size / 1024.0, $size / 1024.0)))

    if ($mac) { $inset = 100; $radius = 185 } else { $inset = if ($small) { 0 } else { 32 }; $radius = if ($small) { 200 } else { 210 } }
    $body = [System.Windows.Rect]::new($inset, $inset, 1024 - 2 * $inset, 1024 - 2 * $inset)
    if ($mac) {
        # Soft shadow under the square, as macOS icons have.
        foreach ($i in 1..14) {
            $spread = $i * 3.0
            $shadow = [System.Windows.Rect]::new($body.X - $spread / 2, $body.Y + 10 + $spread / 3, $body.Width + $spread, $body.Height + $spread / 2)
            $dc.DrawRoundedRectangle((Brush ("#{0:X2}000000" -f [int](9 - $i * 0.55))), $null, $shadow, $radius + $spread / 2, $radius + $spread / 2)
        }
    }
    $dc.DrawRoundedRectangle((Vertical "#2E7FD9" "#1458A8"), $null, $body, $radius, $radius)
    # A faint light from the top.
    $sheen = [System.Windows.Media.LinearGradientBrush]::new((Colour "#28FFFFFF"), (Colour "#00FFFFFF"), (Point 0.5 0), (Point 0.5 0.6))
    $dc.DrawRoundedRectangle($sheen, $null, $body, $radius, $radius)

    # The lens: a white ring around a coloured glass. Bigger and bolder at small sizes.
    $c = 512.0
    $unit = $body.Width / 960.0  # artwork is laid out for the Windows square
    $ringR = $unit * $(if ($small) { 350 } else { 300 })
    $glassR = $unit * $(if ($small) { 250 } else { 215 })
    $centre = Point $c $c
    $dc.DrawEllipse((Brush "#FFFFFF"), $null, $centre, $ringR, $ringR)
    $glass = [System.Windows.Media.EllipseGeometry]::new($centre, $glassR, $glassR)

    # The glass is tinted by kind; every icon keeps the bolt.
    $tints = @{ app = @("#A9D2F7", "#6FA9E6"); picture = @("#5FD3A6", "#159A6E"); pdf = @("#F06262", "#C93535"); video = @("#8C84EC", "#4A40B0") }
    $dc.DrawGeometry((Vertical $tints[$kind][0] $tints[$kind][1]), $null, $glass)
    # The bolt, crossing the ring like a flash across the lens.
    $u = $unit * $(if ($small) { 1.15 } else { 1.0 })
    $bolt = Shape $c $c $u @(@(43, -213), @(-117, 43), @(0, 43), @(-43, 213), @(117, -43), @(0, -43))
    $dc.DrawGeometry((Brush "#FFFFFF"), $null, $bolt)

    if ($kind -ne "app" -and $size -ge 40) {
        # A badge in the corner says what's in the file: a landscape, a page, or a play button.
        $r = 150 * $unit
        $b = Point ($body.Right - $r - 20 * $unit) ($body.Bottom - $r - 20 * $unit)
        $dc.DrawEllipse((Brush "#FFFFFF"), $null, $b, $r, $r)
        $inner = $r * 0.8
        $disc = [System.Windows.Media.EllipseGeometry]::new($b, $inner, $inner)
        switch ($kind) {
            "picture" {
                $dc.PushClip($disc)
                $dc.DrawGeometry((Vertical "#3FA2FF" "#A8E3FF"), $null, $disc)
                $dc.DrawEllipse((Brush "#FFD54A"), $null, (Point ($b.X + 0.38 * $inner) ($b.Y - 0.38 * $inner)), (0.24 * $inner), (0.24 * $inner))
                $dc.DrawGeometry((Brush "#18A77C"), $null, (Shape $b.X $b.Y $inner @(@(-1, 0.45), @(-0.45, -0.05), @(-0.05, 0.35), @(0.3, 0.05), @(1, 0.55), @(1, 1), @(-1, 1))))
                $dc.Pop()
            }
            "pdf" {
                $dc.DrawGeometry((Brush "#D93E3E"), $null, $disc)
                $w = 0.95 * $inner; $h = 1.25 * $inner; $fold = 0.32 * $inner
                $dc.DrawGeometry((Brush "#FFFFFF"), $null, (Shape ($b.X - $w / 2) ($b.Y - $h / 2) 1 @(@(0, 0), @(($w - $fold), 0), @($w, $fold), @($w, $h), @(0, $h))))
            }
            "video" {
                $dc.DrawGeometry((Brush "#5A50C8"), $null, $disc)
                $dc.DrawGeometry((Brush "#FFFFFF"), $null, (Shape $b.X $b.Y $inner @(@(-0.32, -0.5), @(0.55, 0), @(-0.32, 0.5))))
            }
        }
    }

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

# Windows: .ico files of PNGs, every size Explorer and the taskbar ask for.
function WriteIco([string]$kind, [string]$path) {
    $sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 256)
    $images = foreach ($s in $sizes) { , (Render $s $false $kind) }
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
    [IO.File]::WriteAllBytes((Join-Path $root $path), $ico.ToArray())
}
WriteIco "app" "windows\glance.ico"
WriteIco "picture" "windows\picture.ico"
WriteIco "pdf" "windows\pdf.ico"
WriteIco "video" "windows\video.ico"

if ($Preview) {
    New-Item -ItemType Directory -Force $Preview | Out-Null
    foreach ($s in 1024, 256, 64, 32, 24, 16) {
        [IO.File]::WriteAllBytes((Join-Path $Preview "mac-$s.png"), (Render $s $true))
        foreach ($kind in "app", "picture", "pdf", "video") {
            [IO.File]::WriteAllBytes((Join-Path $Preview "win-$kind-$s.png"), (Render $s $false $kind))
        }
    }
}
"Wrote Resources\Glance.icns and windows\glance.ico, picture.ico, pdf.ico, video.ico"
