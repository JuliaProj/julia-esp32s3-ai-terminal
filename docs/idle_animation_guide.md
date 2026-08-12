# Julia substate idle animation pipeline

Generate the Wan clips through the local ComfyUI API:

```powershell
python tools/generate_idle_wan21.py --all
```

Build device assets and a size/loop report. The deployed substate set uses
33-frame, 12 fps clips:

```powershell
python tools/idle_postprocess.py --resolution 180 --frames 36 --fps 12
```

The deployed assets use 180x180 RLE storage because a 360x360 generated clip
is 3-4 MiB and cannot fit the 2.5 MiB runtime cache. The player expands each
decoded frame to the native 360x360 LCD dimensions. The RGB565 byte order is
read from `sdkconfig`. Final artwork may use `--resolution 360` only after its
compressed file size is confirmed to fit the cache.

Copy the generated files to the SD card under:

```text
/julia/idle/S0_1_sleep_breathing.trn
/julia/idle/S1_1_standby_peek.trn
/julia/idle/S2_1_companion_read.trn
/julia/idle/S2_1_companion_tea.trn
/julia/idle/S4_2_chat_listen.trn
# ...the remaining substate files follow the director-table names
```

The current deployment contains 19 primary substate clips. S5.3 intentionally
reuses S5.2 because the state model has 20 enum substates, and S2.1 adds a
second drinking/tea clip for its 30-second alternation. There are therefore 20
files in `/julia/idle/`.

## Runtime cache

Idle and transition assets share an LRU PSRAM pool capped at 2,621,440 bytes.
The active file is acquired and cannot be evicted. One predicted or alternate
file is kept unreferenced and is the first eviction candidate when another
asset is needed. Flash-mapped fallbacks do not count against this PSRAM cap.

Loading is asynchronous and the state static image remains visible until the
clip is ready. Measured SD-to-PSRAM load time is about 1.0-1.8 seconds. The
loader reports `load slow` after 500 ms but continues loading; aborting at 500
ms would prevent the current 0.9-1.4 MiB files from ever playing. Playback
starts only after the main-state transition has completed.

S2.1 randomly starts with read or tea. The other file is preloaded, then the
two cache roles swap at the 30-second loop boundary without another SD read.
During S4 playback the existing RMS mouth shape is composited over the full
idle frame. Other locked-mouth states keep the idle mouth behavior.

## Deployment and diagnostics

Upload the converted directory with:

```powershell
python tools/upload_idle.py --input assets/transitions/idle --port COM5
```

Each upload is chunked and checked with a whole-file CRC. Useful serial
commands are `idle list`, `idle play <substate>`, `idle preload <substate>`,
`idle cache`, `idle clear`, and `idle switch`.
