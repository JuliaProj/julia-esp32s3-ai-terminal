param(
    [string]$InputDir = "$PSScriptRoot\..\file\ui_animation\standby",
    [string]$OutputDir = "$PSScriptRoot\..\main\ui\generated"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
[IO.Directory]::CreateDirectory($OutputDir) | Out-Null

$header = @'
#pragma once
#include "lvgl.h"
const lv_img_dsc_t *julia_blink_asset(bool closed);
const lv_img_dsc_t *julia_blink_frame(uint8_t frame);
const lv_img_dsc_t *julia_breath_asset(void);
const lv_img_dsc_t *julia_mouth_asset(uint8_t level);
'@
[IO.File]::WriteAllText((Join-Path $OutputDir 'julia_blink_assets.h'), $header,
                        [Text.UTF8Encoding]::new($false))

$writer = [IO.StreamWriter]::new((Join-Path $OutputDir 'julia_blink_assets.c'), $false,
                                 [Text.UTF8Encoding]::new($false))
$writer.WriteLine('#include "julia_blink_assets.h"')
$writer.WriteLine()

$assets = @(
    @{ Name = 'julia_blink_open'; File = 'blink_00_open.png' },
    @{ Name = 'julia_blink_half'; File = 'blink_00_open.png'; Blend = 'blink_01_closed.png' },
    @{ Name = 'julia_blink_closed'; File = 'blink_01_closed.png' },
    @{ Name = 'julia_breath_inhale'; File = 'breath_inhale.png' }
)

foreach ($asset in $assets) {
    $image = [Drawing.Bitmap]::new((Resolve-Path (Join-Path $InputDir $asset.File)).Path)
    $blend = if ($asset.Blend) {
        [Drawing.Bitmap]::new((Resolve-Path (Join-Path $InputDir $asset.Blend)).Path)
    } else { $null }
    if ($image.Width -ne 360 -or $image.Height -ne 360) {
        throw "Unexpected blink frame size $($image.Width)x$($image.Height)"
    }
    $symbol = $asset.Name
    $writer.WriteLine("static const uint8_t $($symbol)_map[] LV_ATTRIBUTE_MEM_ALIGN = {")
    for ($y = 0; $y -lt 360; $y++) {
        for ($x = 0; $x -lt 360; $x++) {
            $c = $image.GetPixel($x, $y)
            if ($blend) {
                $b = $blend.GetPixel($x, $y)
                $c = [Drawing.Color]::FromArgb(
                    [int](($c.A + $b.A) / 2), [int](($c.R + $b.R) / 2),
                    [int](($c.G + $b.G) / 2), [int](($c.B + $b.B) / 2))
            }
            [int]$rgb565 = (([int]$c.R -shr 3) -shl 11) -bor (([int]$c.G -shr 2) -shl 5) -bor ([int]$c.B -shr 3)
            $writer.Write(("0x{0:X2},0x{1:X2}," -f ($rgb565 -band 0xFF), (($rgb565 -shr 8) -band 0xFF)))
        }
        $writer.WriteLine()
    }
    $writer.WriteLine('};')
    $writer.WriteLine("static const lv_img_dsc_t $symbol = {")
    $writer.WriteLine("    .header.always_zero = 0, .header.w = 360, .header.h = 360,")
    $writer.WriteLine("    .data_size = sizeof($($symbol)_map), .header.cf = LV_IMG_CF_TRUE_COLOR,")
    $writer.WriteLine("    .data = $($symbol)_map,")
    $writer.WriteLine('};')
    $writer.WriteLine()
    $image.Dispose()
    if ($blend) { $blend.Dispose() }
}

$mouthAssets = @('mouth_00_closed.png', 'mouth_01_small.png',
                 'mouth_02_medium.png', 'mouth_03_wide.png')
for ($index = 0; $index -lt $mouthAssets.Count; $index++) {
    $image = [Drawing.Bitmap]::new((Resolve-Path (Join-Path $InputDir $mouthAssets[$index])).Path)
    $symbol = "julia_mouth_$index"
    $writer.WriteLine("static const uint8_t $($symbol)_map[] LV_ATTRIBUTE_MEM_ALIGN = {")
    for ($y = 0; $y -lt 64; $y++) {
        for ($x = 0; $x -lt 64; $x++) {
            $c = $image.GetPixel($x + 148, $y + 175)
            [int]$rgb565 = (([int]$c.R -shr 3) -shl 11) -bor (([int]$c.G -shr 2) -shl 5) -bor ([int]$c.B -shr 3)
            $writer.Write(("0x{0:X2},0x{1:X2}," -f ($rgb565 -band 0xFF), (($rgb565 -shr 8) -band 0xFF)))
        }
        $writer.WriteLine()
    }
    $writer.WriteLine('};')
    $writer.WriteLine("static const lv_img_dsc_t $symbol = {")
    $writer.WriteLine("    .header.always_zero = 0, .header.w = 64, .header.h = 64,")
    $writer.WriteLine("    .data_size = sizeof($($symbol)_map), .header.cf = LV_IMG_CF_TRUE_COLOR,")
    $writer.WriteLine("    .data = $($symbol)_map,")
    $writer.WriteLine('};')
    $writer.WriteLine()
    $image.Dispose()
}

$writer.WriteLine('const lv_img_dsc_t *julia_blink_asset(bool closed)')
$writer.WriteLine('{')
$writer.WriteLine('    return closed ? &julia_blink_closed : &julia_blink_open;')
$writer.WriteLine('}')
$writer.WriteLine('const lv_img_dsc_t *julia_blink_frame(uint8_t frame)')
$writer.WriteLine('{')
$writer.WriteLine('    static const lv_img_dsc_t *frames[] = {')
$writer.WriteLine('        &julia_blink_open, &julia_blink_half, &julia_blink_closed, &julia_blink_half')
$writer.WriteLine('    };')
$writer.WriteLine('    return frames[frame < 4 ? frame : 0];')
$writer.WriteLine('}')
$writer.WriteLine('const lv_img_dsc_t *julia_mouth_asset(uint8_t level)')
$writer.WriteLine('{')
$writer.WriteLine('    static const lv_img_dsc_t *assets[] = {')
$writer.WriteLine('        &julia_mouth_0, &julia_mouth_1, &julia_mouth_2, &julia_mouth_3')
$writer.WriteLine('    };')
$writer.WriteLine('    return assets[level < 4 ? level : 3];')
$writer.WriteLine('}')
$writer.WriteLine('const lv_img_dsc_t *julia_breath_asset(void)')
$writer.WriteLine('{')
$writer.WriteLine('    return &julia_breath_inhale;')
$writer.WriteLine('}')
$writer.Dispose()

Write-Host "Generated Julia blink assets in $OutputDir"
