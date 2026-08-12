# Julia ComfyUI Setup Guide

## Current machine status

ComfyUI is installed at:

```text
D:\ComfyUI\ComfyUI_windows_portable_nvidia\ComfyUI_windows_portable\ComfyUI
```

The API was not running during implementation. The installation also lacked an anime checkpoint, IPAdapter Plus, and OpenPose/DWPose nodes and models, so no generated PNGs are reported as completed.

## Required installation

1. Install `ComfyUI_IPAdapter_plus` and `comfyui_controlnet_aux` through ComfyUI Manager.
2. Put an SD 1.5 anime checkpoint such as Counterfeit V3 or AOM3 under `models/checkpoints/`.
3. Put `CLIP-ViT-H-14-laion2B-s32B-b79K.safetensors` under `models/clip_vision/`.
4. Put `ip-adapter-plus-face_sd15.safetensors` under `models/ipadapter/`.
5. Put an SD 1.5 OpenPose ControlNet model under `models/controlnet/`.
6. Restart ComfyUI with API listening on `127.0.0.1:8188`.

Model filenames vary between node versions. Update nodes `6`, `7`, and `9` in `tools/comfyui_base_workflow.json` when the installed filenames differ. The generator calls `/object_info` first and reports missing node classes or checkpoints before queueing work.

## Generate five routes

```powershell
python tools/generate_transitions_comfyui.py `
  --transition all `
  --comfy-input "D:\ComfyUI\ComfyUI_windows_portable_nvidia\ComfyUI_windows_portable\ComfyUI\input"
```

The five routes are `S1_S3`, `S3_S4`, `S4_S1`, `S1_S0`, and `S0_S1`. Each route produces 12 numbered PNGs. A fixed route seed plus IPAdapter and OpenPose conditioning is used to reduce character drift. Inspect the sequences before conversion; regenerate a route with adjusted prompts or reference weight if face, clothing, or body proportions drift.

The supplied graph is an API-format template, not a guarantee that every third-party node version uses identical input names. Export one working graph in API format from the local ComfyUI UI and align the three custom nodes if preflight succeeds but prompt validation fails.

On a 5090D, generation speed depends mainly on checkpoint, sampler, and custom-node versions. The requested 12 frames in 30 seconds must be measured locally; it was not measurable while the API and models were unavailable.
