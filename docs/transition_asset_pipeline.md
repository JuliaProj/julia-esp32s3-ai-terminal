# Julia Transition Asset Pipeline

## Generate and convert

```powershell
python tools/generate_transitions_comfyui.py --transition S1_S3 --comfy-input <ComfyUI-input-directory>

python tools/convert_media_to_trn.py `
  --input assets/transitions_raw/S1_S3 `
  --output S1_S3/lean_forward.trn `
  --fps 15 --rle --resolution 360
```

Relative outputs are placed under `assets/transitions/`. Format `0` remains the original raw RGB565 layout. Format `1` stores each frame with the existing RGB565 RLE codec. Both 360x360 and optional 180x180 files are accepted; 180x180 playback expands pixels 2x with nearest-neighbor scaling. The converter reads `CONFIG_LV_COLOR_16_SWAP` from `sdkconfig`; this repository currently generates high-byte-first RGB565.

Raw 360x360 is 259,200 bytes per frame, so a 12-frame route is about 3.0 MiB before compression, not under 2 MiB. Use `--rle`, `--resolution 180`, fewer frames, or shorter clips when the size budget matters. RLE effectiveness depends on the artwork.

## Deploy to SD

The reliable development path is direct SD copying. Copy the directory so the card contains:

```text
/julia/transitions/S1_S3/lean_forward.trn
/julia/transitions/S3_S4/open_mouth.trn
...
```

On the device `/sdcard` is only the mount point; do not create an `/sdcard` folder on the card itself. After insertion, use `transition list`, then `transition play S1 S3`.

Serial upload uses 4 KiB acknowledged blocks and end-to-end CRC:

```powershell
python tools/upload_transitions.py --input assets/transitions --port COM5
```

The current hardware previously returned SD `errno=5` at binary upload start. That is not considered fixed until a real transfer completes and CRC is confirmed. Direct SD copy remains the fallback.

## Flash deployment safety

The current 16 MiB partition table has two 4.5 MiB OTA slots, a 3 MiB model partition, and a 3.875 MiB storage partition. It has no free dedicated transition partition. `tools/flash_transitions.py` therefore refuses the current layout instead of overwriting OTA or storage.

After explicitly adding a `transitions` data partition and updating firmware partition-pack reading, prepare and flash with:

```powershell
python tools/flash_transitions.py --input assets/transitions
python tools/flash_transitions.py --input assets/transitions --port COM5 --flash
```

Do not enable this by shrinking partitions without reviewing OTA image size and persistent storage requirements. The current application has only about 6% free in its app slot, so embedding multi-megabyte raw transitions in the application is not viable.

## Runtime source order

The current runtime resolves assets in this order:

1. PSRAM LRU cache for an already loaded `.trn`.
2. SD `.trn`, loaded into PSRAM.
3. Embedded Flash `.clip` fallback for the available key routes.
4. Existing sleep/wake sequence or direct static state behavior.

During direct frame playback LVGL refresh is paused, local eye/mouth animation is hidden, and completion returns through the existing synchronized static-frame commit. New transition requests invalidate the current generation and are handled by the same playback task.

Useful commands:

```text
transition list
transition play S1 S3
transition cache
transition fallback on
transition fallback off
transition upload S1_S3 lean_forward.trn <bytes> <crc32>
```
