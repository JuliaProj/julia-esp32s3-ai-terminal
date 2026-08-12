# Julia Dynamic State Asset Matrix

All assets use `julia_standby_reference_v3.png` as the identity reference.
State loops are 33 frames at 12 fps; directed transitions are 17 frames at
12 fps. Sources are generated at 640x640 and postprocessed to 360x360 RGB565.

## State Loops

| Asset | State | Visual action |
|---|---|---|
| S0_1 | Night sleep | Deep sleep and extremely subtle breathing |
| S0_2 | Day away | Sleeping with one wistful brow movement |
| S0_3 | Manual sleep | Good-night nod and settle |
| S1_1 | Near standby | Shallow rest and a brief peek |
| S1_2 | Far standby | Fully closed eyes, nearly motionless |
| S1_3 | Charging standby | Rest and brief grateful smile |
| S2_1 | Observe | Read, look up, smile, resume reading |
| S2_2 | Shared activity | Listen and gently sway |
| S2_3 | Bedtime companion | Rub eye, yawn, half-close eyes |
| S3_1 | Emotion trigger | Concerned lean-in |
| S3_2 | Routine break | Gentle worried head tilt |
| S3_3 | User call | Bright recognition and welcome |
| S3_4 | Recovery probe | Tentative wave and wait |
| S4_1 | Light dialog | Friendly listening nod |
| S4_2 | Deep talk | Empathetic focused nods |
| S4_3 | Multi-turn | Chin-rest, playful tilt and laugh |
| S4_4 | Interrupted | Puzzled pause and patient wait |
| S5_1 | Rejected | Lower gaze and close eyes |
| S5_2 | Perfunctory | Hurt/confused, then accept silence |

## Directed Transitions

`S0_S1`, `S1_S0`, `S1_S2`, `S2_S1`, `S1_S3`, `S2_S3`, `S3_S4`,
`S4_S1`, `S4_S5`, `S5_S1`, `S5_S2`, and `S5_S4` are generated independently.
Reverse playback is prohibited because facial motion and object handling are
not temporally symmetric.

## Commands

```powershell
python tools/generate_state_assets_wan22.py --kind state --name S1_1
python tools/generate_state_assets_wan22.py --kind transition --name S1_S2
python tools/generate_state_assets_wan22.py --kind all
```

The generator skips existing MP4 files, so interrupted batches resume safely.
Convert accepted sources with `tools/convert_media_to_trn.py`; retain the same
asset names when professional artwork replaces the generated demo resources.

## Generated Demo Set

The current demo set contains all 19 state loops and 12 directed transitions.
Raw review videos are stored in `assets/state_assets_raw/`. Device assets are
stored under `assets/transitions/generated/` with a machine-readable
`manifest.json`.

```powershell
python tools/review_state_assets.py
python tools/build_state_showcase.py
python tools/build_state_trn.py --resolution 360
```

The 360x360 RGB565 RLE set is about 86.7 MB. It is intended for SD-card
on-demand playback; do not preload the full set into 8 MB PSRAM. Assets marked
`one_shot_hold_last` in the manifest deliberately finish in a new expression
and must not hard-loop back to frame zero.
