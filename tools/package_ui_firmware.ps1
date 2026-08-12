$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$out = Join-Path $root 'release\Julia-UI-Firmware-v1.0.0'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$files = @(
  @{ Source = 'build\bootloader\bootloader.bin'; Target = 'bootloader.bin' },
  @{ Source = 'build\julia-ai.bin'; Target = 'julia-ui.bin' },
  @{ Source = 'build\partition_table\partition-table.bin'; Target = 'partition-table.bin' },
  @{ Source = 'build\ota_data_initial.bin'; Target = 'ota_data_initial.bin' },
  @{ Source = 'build\srmodels\srmodels.bin'; Target = 'srmodels.bin' }
)
foreach ($f in $files) {
  $source = Join-Path $root $f.Source
  if (-not (Test-Path -LiteralPath $source)) { throw "Missing firmware artifact: $source" }
  Copy-Item -LiteralPath $source -Destination (Join-Path $out $f.Target) -Force
}
@'
--flash_mode dio --flash_freq 80m --flash_size 16MB
0x0 bootloader.bin
0x20000 julia-ui.bin
0x8000 partition-table.bin
0xf000 ota_data_initial.bin
0x920000 srmodels.bin
'@ | Set-Content -LiteralPath (Join-Path $out 'flash_args.txt') -Encoding ASCII
Get-FileHash (Get-ChildItem -LiteralPath $out -File -Filter '*.bin' | ForEach-Object { $_.FullName }) -Algorithm SHA256 |
  ForEach-Object { '{0}  {1}' -f $_.Hash.ToLowerInvariant(), $_.Path.Substring($out.Length + 1) } |
  Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Encoding ASCII
Write-Output "Created $out"
