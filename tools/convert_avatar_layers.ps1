param(
    [string]$InputDir = "$PSScriptRoot\..\main\ui\layers\rig_v1"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# LVGL is configured with LV_COLOR_DEPTH=16 and LV_COLOR_16_SWAP=1.
# TRUE_COLOR_ALPHA therefore stores each pixel as RGB565 high byte, low byte,
# then alpha. Transparent pixels retain their RGB value to avoid dark fringes.
Get-ChildItem -LiteralPath $InputDir -Filter '*.png' |
    Where-Object { $_.BaseName -ne 'composite_preview' } |
    ForEach-Object {
        $bitmap = [Drawing.Bitmap]::new($_.FullName)
        $output = [IO.File]::Open((Join-Path $InputDir ($_.BaseName + '.bin')),
                                 [IO.FileMode]::Create, [IO.FileAccess]::Write)
        try {
            for ($y = 0; $y -lt $bitmap.Height; $y++) {
                for ($x = 0; $x -lt $bitmap.Width; $x++) {
                    $pixel = $bitmap.GetPixel($x, $y)
                    [uint16]$rgb565 = ([uint16]($pixel.R -shr 3) -shl 11) -bor
                                       ([uint16]($pixel.G -shr 2) -shl 5) -bor
                                       [uint16]($pixel.B -shr 3)
                    $output.WriteByte([byte](($rgb565 -shr 8) -band 0xff))
                    $output.WriteByte([byte]($rgb565 -band 0xff))
                    $output.WriteByte([byte]$pixel.A)
                }
            }
        }
        finally {
            $output.Dispose()
            $bitmap.Dispose()
        }
        Write-Host "Converted $($_.Name)"
    }

# The verified 360x360 composite is also embedded as a stable rig baseline.
# It has no alpha and therefore uses two swapped RGB565 bytes per pixel.
$composite = [Drawing.Bitmap]::new((Join-Path $InputDir 'composite_preview.png'))
$compositePath = Join-Path $InputDir 'composite_rgb565.bin'
$compositeOut = [IO.File]::Open($compositePath, [IO.FileMode]::Create, [IO.FileAccess]::Write)
try {
    for ($y = 0; $y -lt $composite.Height; $y++) {
        for ($x = 0; $x -lt $composite.Width; $x++) {
            $pixel = $composite.GetPixel($x, $y)
            [uint16]$rgb565 = ([uint16]($pixel.R -shr 3) -shl 11) -bor
                               ([uint16]($pixel.G -shr 2) -shl 5) -bor
                               [uint16]($pixel.B -shr 3)
            $compositeOut.WriteByte([byte](($rgb565 -shr 8) -band 0xff))
            $compositeOut.WriteByte([byte]($rgb565 -band 0xff))
        }
    }
}
finally {
    $compositeOut.Dispose()
    $composite.Dispose()
}
Write-Host 'Converted composite_preview.png'
