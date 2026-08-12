param(
    [string]$Atlas = "$PSScriptRoot\..\main\ui\layers\julia_layer_atlas_v1.png",
    [string]$OutputDir = "$PSScriptRoot\..\main\ui\layers\rig_v1"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
[IO.Directory]::CreateDirectory($OutputDir) | Out-Null

function Get-TrimmedCell([Drawing.Bitmap]$source, [int]$column, [int]$row, [int]$split = 0) {
    $cell = 418
    $x0 = $column * $cell
    $y0 = $row * $cell
    $sx0 = if ($split -eq 2) { $cell / 2 } else { 0 }
    $sx1 = if ($split -eq 1) { $cell / 2 } else { $cell }
    $minX = $sx1; $minY = $cell; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $cell; $y++) {
        for ($x = $sx0; $x -lt $sx1; $x++) {
            if ($source.GetPixel($x0 + $x, $y0 + $y).A -gt 12) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    if ($maxX -lt $minX) { throw "Empty atlas cell $column,$row split=$split" }
    $rect = [Drawing.Rectangle]::new($x0 + $minX, $y0 + $minY,
                                     $maxX - $minX + 1, $maxY - $minY + 1)
    return $source.Clone($rect, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
}

function Resize-Layer([Drawing.Bitmap]$source, [int]$width, [int]$height) {
    $result = [Drawing.Bitmap]::new($width, $height, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($result)
    $g.Clear([Drawing.Color]::Transparent)
    $g.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
    $g.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($source, [Drawing.Rectangle]::new(0, 0, $width, $height))
    $g.Dispose()
    return $result
}

$specs = @(
    @{ Name='body';       Col=0; Row=0; Split=0; X=55;  Y=214; W=250; H=166 },
    @{ Name='hair_back';  Col=1; Row=0; Split=0; X=63;  Y=8;   W=234; H=240 },
    @{ Name='face';       Col=2; Row=0; Split=0; X=103; Y=52;  W=154; H=210 },
    @{ Name='hair_front'; Col=0; Row=1; Split=0; X=59;  Y=13;  W=242; H=226 },
    @{ Name='eye_left';   Col=1; Row=1; Split=0; X=108; Y=132; W=51;  H=51  },
    @{ Name='eye_right';  Col=2; Row=1; Split=0; X=199; Y=132; W=51;  H=51  },
    @{ Name='pupil_left'; Col=0; Row=2; Split=1; X=121; Y=143; W=30;  H=36  },
    @{ Name='pupil_right';Col=0; Row=2; Split=2; X=211; Y=143; W=30;  H=36  },
    @{ Name='mouth';      Col=1; Row=2; Split=0; X=161; Y=194; W=38;  H=13  }
)

$atlasBitmap = [Drawing.Bitmap]::new((Resolve-Path $Atlas).Path)
$preview = [Drawing.Bitmap]::new(360, 360, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
$pg = [Drawing.Graphics]::FromImage($preview)
$pg.Clear([Drawing.Color]::FromArgb(255, 250, 249, 246))
$pg.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality

foreach ($spec in $specs) {
    $trimmed = Get-TrimmedCell $atlasBitmap $spec.Col $spec.Row $spec.Split
    $layer = Resize-Layer $trimmed $spec.W $spec.H
    $path = Join-Path $OutputDir ($spec.Name + '.png')
    $layer.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $pg.DrawImageUnscaled($layer, $spec.X, $spec.Y)
    $trimmed.Dispose()
    $layer.Dispose()
}

$pg.Dispose()
$preview.Save((Join-Path $OutputDir 'composite_preview.png'), [Drawing.Imaging.ImageFormat]::Png)
$preview.Dispose()
$atlasBitmap.Dispose()
Write-Host "Extracted $($specs.Count) transparent rig layers to $OutputDir"
