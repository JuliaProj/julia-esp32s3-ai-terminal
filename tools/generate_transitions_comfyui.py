#!/usr/bin/env python3
"""Generate Julia transition frames through a local ComfyUI API."""

from __future__ import annotations

import argparse
import json
import shutil
import time
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

from comfyui_workflow_builder import TRANSITIONS, build

ROOT = Path(__file__).resolve().parents[1]


def request_json(base: str, path: str, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    request = urllib.request.Request(base + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


def preflight(base: str, checkpoint: str) -> None:
    objects = request_json(base, "/object_info")
    required = {"CheckpointLoaderSimple", "CLIPTextEncode", "KSampler", "VAEDecode", "SaveImage", "LoadImage", "CLIPVisionLoader", "IPAdapterModelLoader", "IPAdapterAdvanced", "ControlNetLoader", "DWPreprocessor", "ControlNetApplyAdvanced"}
    missing = sorted(required - objects.keys())
    checkpoints = objects.get("CheckpointLoaderSimple", {}).get("input", {}).get("required", {}).get("ckpt_name", [[]])[0]
    if checkpoint not in checkpoints:
        missing.append(f"checkpoint:{checkpoint}")
    if missing:
        raise SystemExit("ComfyUI preflight failed; missing: " + ", ".join(missing))


def download_images(base: str, history: dict, output: Path, frame: int) -> None:
    images = []
    for node in history.get("outputs", {}).values():
        images.extend(node.get("images", []))
    if not images:
        raise RuntimeError("ComfyUI completed without an image output")
    image = images[-1]
    query = urllib.parse.urlencode({"filename": image["filename"], "subfolder": image.get("subfolder", ""), "type": image.get("type", "output")})
    with urllib.request.urlopen(base + "/view?" + query, timeout=30) as response:
        (output / f"frame_{frame + 1:04d}.png").write_bytes(response.read())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transition", choices=[*TRANSITIONS, "all"], default="all")
    parser.add_argument("--url", default="http://127.0.0.1:8188")
    parser.add_argument("--checkpoint", default="Counterfeit-V3.0_fp16.safetensors")
    parser.add_argument("--reference", type=Path, default=ROOT / "main/ui/generated/julia_reference_ui.png")
    parser.add_argument("--comfy-input", type=Path, required=True, help="ComfyUI input directory")
    parser.add_argument("--output", type=Path, default=ROOT / "assets/transitions_raw")
    args = parser.parse_args()
    if not args.reference.is_file():
        raise SystemExit(f"reference image not found: {args.reference}")
    preflight(args.url, args.checkpoint)
    args.comfy_input.mkdir(parents=True, exist_ok=True)
    reference_name = "julia_reference.png"
    shutil.copy2(args.reference, args.comfy_input / reference_name)
    client_id = str(uuid.uuid4())
    names = TRANSITIONS.keys() if args.transition == "all" else [args.transition]
    for name in names:
        output = args.output / name
        output.mkdir(parents=True, exist_ok=True)
        for frame in range(TRANSITIONS[name]["frames"]):
            queued = request_json(args.url, "/prompt", {"prompt": build(name, frame, args.checkpoint, reference_name), "client_id": client_id})
            prompt_id = queued["prompt_id"]
            deadline = time.time() + 120
            while time.time() < deadline:
                history = request_json(args.url, f"/history/{prompt_id}").get(prompt_id)
                if history:
                    download_images(args.url, history, output, frame)
                    print(f"{name} frame {frame + 1}/{TRANSITIONS[name]['frames']}")
                    break
                time.sleep(0.5)
            else:
                raise TimeoutError(f"ComfyUI timeout: {name} frame {frame + 1}")


if __name__ == "__main__":
    main()
