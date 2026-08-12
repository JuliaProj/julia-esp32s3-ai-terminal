param(
    [string]$StateDir = "$PSScriptRoot\..\file\ui_animation\states_v3",
    [string]$OutputDir = "$PSScriptRoot\..\main\ui\generated"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
[IO.Directory]::CreateDirectory($OutputDir) | Out-Null

$states = @(
    's0_1_night_sleep', 's0_2_day_away', 's0_3_manual_sleep', 's1_1_near_standby', 's1_2_far_standby',
    's1_3_charging_standby', 's2_1_observe', 's2_2_shared_activity', 's2_3_bedtime_companion', 's3_1_emotion_trigger',
    's3_2_routine_break', 's3_3_user_call', 's3_4_recovery_probe', 's4_1_light_dialog', 's4_2_deep_talk',
    's4_3_multi_turn', 's4_4_interrupt_handle', 's5_1_user_reject', 's5_2_user_perfunctory', 's5_3_user_left'
)
$enums = @(
    'JULIA_SUB_STATE_S0_1_NIGHT_SLEEP', 'JULIA_SUB_STATE_S0_2_DAY_AWAY', 'JULIA_SUB_STATE_S0_3_MANUAL_SLEEP',
    'JULIA_SUB_STATE_S1_1_NEAR_STANDBY', 'JULIA_SUB_STATE_S1_2_FAR_STANDBY', 'JULIA_SUB_STATE_S1_3_CHARGING_STANDBY',
    'JULIA_SUB_STATE_S2_1_OBSERVE', 'JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY', 'JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION',
    'JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER', 'JULIA_SUB_STATE_S3_2_ROUTINE_BREAK', 'JULIA_SUB_STATE_S3_3_USER_CALL',
    'JULIA_SUB_STATE_S3_4_RECOVERY_PROBE', 'JULIA_SUB_STATE_S4_1_LIGHT_DIALOG', 'JULIA_SUB_STATE_S4_2_DEEP_TALK',
    'JULIA_SUB_STATE_S4_3_MULTI_TURN', 'JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE', 'JULIA_SUB_STATE_S5_1_USER_REJECT',
    'JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY', 'JULIA_SUB_STATE_S5_3_USER_LEFT'
)

if (!(Test-Path $StateDir)) { throw "V3 state directory was not found: $StateDir" }

$header = @'
#pragma once
#include "lvgl.h"
#include "julia_fsm.h"
const lv_img_dsc_t *julia_ui_asset_for_state(julia_sub_state_t state);
const lv_img_dsc_t *julia_ui_reference_asset(void);
'@
[IO.File]::WriteAllText((Join-Path $OutputDir 'julia_ui_assets.h'), $header, [Text.UTF8Encoding]::new($false))

$assetSize = 360
$binPath = Join-Path $OutputDir 'julia_ui_assets.bin'
$binStream = [IO.File]::Open($binPath, [IO.FileMode]::Create, [IO.FileAccess]::Write)
$binWriter = [IO.BinaryWriter]::new($binStream)
$offsets = @()
$lengths = @()

for ($i = 0; $i -lt $states.Count; $i++) {
    $statePath = Join-Path $StateDir ($states[$i] + '.png')
    if (!(Test-Path $statePath)) { throw "Missing V3 state portrait: $statePath" }
    $source = [Drawing.Bitmap]::new((Resolve-Path $statePath).Path)
    $side = [Math]::Min($source.Width, $source.Height)
    $x0 = [Math]::Floor(($source.Width - $side) / 2)
    $y0 = [Math]::Floor(($source.Height - $side) / 2)
    $small = [Drawing.Bitmap]::new($assetSize, $assetSize, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($small)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($source, [Drawing.Rectangle]::new(0, 0, $assetSize, $assetSize),
                 [Drawing.Rectangle]::new($x0, $y0, $side, $side), [Drawing.GraphicsUnit]::Pixel)
    if ($states[$i] -eq 's3_3_user_call') {
        # Lean closer with a slight head tilt: a human listening gesture that
        # keeps the character identity intact and needs no symbolic overlay.
        $pose = $small.Clone()
        $g.Clear($small.GetPixel(4, 4))
        $g.TranslateTransform($assetSize / 2, $assetSize / 2)
        $g.RotateTransform(-2.0)
        $g.ScaleTransform(1.05, 1.05)
        $g.TranslateTransform(-$assetSize / 2, -$assetSize / 2)
        $g.DrawImage($pose, 0, 0, $assetSize, $assetSize)
        $g.ResetTransform()
        $pose.Dispose()
    }
    $g.Dispose()
    $source.Dispose()
    $pngPath = Join-Path $OutputDir ("julia_{0}.png" -f $states[$i])
    $small.Save($pngPath, [Drawing.Imaging.ImageFormat]::Png)
    $pngStream = [IO.MemoryStream]::new()
    $small.Save($pngStream, [Drawing.Imaging.ImageFormat]::Png)
    $pngBytes = $pngStream.ToArray()
    $offsets += $binStream.Position
    $lengths += $pngBytes.Length
    $binWriter.Write($pngBytes)
    $pngStream.Dispose()
    $small.Dispose()
}

$binWriter.Dispose()
[IO.File]::Copy((Join-Path $OutputDir 'julia_s1_1_near_standby.png'),
                (Join-Path $OutputDir 'julia_reference_ui.png'), $true)

$writer = [IO.StreamWriter]::new((Join-Path $OutputDir 'julia_ui_assets.c'), $false, [Text.UTF8Encoding]::new($false))
$writer.WriteLine('#include "julia_ui_assets.h"')
$writer.WriteLine('extern const uint8_t julia_ui_assets_start[] asm("_binary_julia_ui_assets_bin_start");')
$writer.WriteLine()
for ($i = 0; $i -lt $states.Count; $i++) {
    $symbol = 'julia_' + $states[$i]
    $offset = $offsets[$i]
    $length = $lengths[$i]
    $writer.WriteLine("static const lv_img_dsc_t $symbol = {")
    $writer.WriteLine("    .header.always_zero = 0, .header.w = $assetSize, .header.h = $assetSize,")
    $writer.WriteLine("    .data_size = $length, .header.cf = LV_IMG_CF_RAW,")
    $writer.WriteLine("    .data = julia_ui_assets_start + $offset,")
    $writer.WriteLine('};')
}
$writer.WriteLine('const lv_img_dsc_t *julia_ui_reference_asset(void) { return &julia_s1_1_near_standby; }')
$writer.WriteLine('const lv_img_dsc_t *julia_ui_asset_for_state(julia_sub_state_t state)')
$writer.WriteLine('{')
$writer.WriteLine('    switch (state) {')
for ($i = 0; $i -lt $states.Count; $i++) {
    $writer.WriteLine("    case $($enums[$i]): return &julia_$($states[$i]);")
}
$writer.WriteLine('    default: return &julia_s1_1_near_standby;')
$writer.WriteLine('    }')
$writer.WriteLine('}')
$writer.Dispose()

Write-Host "Generated $($states.Count) full-resolution state assets and $([IO.FileInfo]::new($binPath).Length)-byte PNG bundle in $OutputDir"
