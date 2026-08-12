#!/usr/bin/env python3
"""Generate identity-locked Julia state anchor images with Flux + PuLID."""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_state_assets_wan22 import BASE_URL, IDENTITY, NEGATIVE, STATES

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = "julia_standby_reference_v3.png"


def request_json(path: str, payload=None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(BASE_URL + path, data=data,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.load(response)


def workflow(name: str, pose: str, seed: int) -> dict:
    prompt = (
        f"{IDENTITY}. Single polished animation keyframe. {pose}. "
        "hands anatomically simple and visible only when required, generous margin around hair and shoulders."
    )
    return {
        "1": {"class_type": "UNETLoader", "inputs": {
            "unet_name": "flux1-dev.safetensors", "weight_dtype": "default"}},
        "2": {"class_type": "ModelSamplingFlux", "inputs": {"model": ["1", 0],
            "max_shift": 1.15, "base_shift": 0.5, "width": 1024, "height": 1024}},
        "3": {"class_type": "DualCLIPLoader", "inputs": {
            "clip_name1": "clip_l.safetensors", "clip_name2": "t5xxl_fp16.safetensors",
            "type": "flux", "device": "default"}},
        "4": {"class_type": "CLIPTextEncode", "inputs": {"text": prompt, "clip": ["3", 0]}},
        "5": {"class_type": "FluxGuidance", "inputs": {"conditioning": ["4", 0], "guidance": 3.5}},
        "6": {"class_type": "CLIPTextEncode", "inputs": {"text": NEGATIVE, "clip": ["3", 0]}},
        "7": {"class_type": "ConditioningZeroOut", "inputs": {"conditioning": ["6", 0]}},
        "8": {"class_type": "LoadImage", "inputs": {"image": REFERENCE}},
        "9": {"class_type": "PulidFluxModelLoader", "inputs": {
            "pulid_file": "pulid_flux_v0.9.0.safetensors"}},
        "10": {"class_type": "PulidFluxEvaClipLoader", "inputs": {}},
        "11": {"class_type": "PulidFluxInsightFaceLoader", "inputs": {"provider": "CUDA"}},
        "12": {"class_type": "ApplyPulidFlux", "inputs": {"model": ["2", 0],
            "pulid_flux": ["9", 0], "eva_clip": ["10", 0], "face_analysis": ["11", 0],
            "image": ["8", 0], "weight": 0.85, "start_at": 0.0, "end_at": 0.9}},
        "13": {"class_type": "VAEEncode", "inputs": {"pixels": ["8", 0], "vae": ["15", 0]}},
        "14": {"class_type": "KSampler", "inputs": {"model": ["12", 0], "seed": seed,
            "steps": 24, "cfg": 1.0, "sampler_name": "euler", "scheduler": "simple",
            "positive": ["5", 0], "negative": ["7", 0], "latent_image": ["13", 0],
            "denoise": 0.48}},
        "15": {"class_type": "VAELoader", "inputs": {"vae_name": "ae.safetensors"}},
        "16": {"class_type": "VAEDecode", "inputs": {"samples": ["14", 0], "vae": ["15", 0]}},
        "17": {"class_type": "SaveImage", "inputs": {"images": ["16", 0],
            "filename_prefix": f"julia/state_anchors/{name}"}},
    }


def generate(name: str, pose: str, seed: int, output: Path, force: bool) -> None:
    target = output / f"{name}.png"
    if target.exists() and not force:
        print(f"{name}: skip existing {target}")
        return
    prompt_id = request_json("/prompt", {"prompt": workflow(name, pose, seed)})["prompt_id"]
    print(f"{name}: queued prompt_id={prompt_id}", flush=True)
    while True:
        item = request_json(f"/history/{prompt_id}").get(prompt_id)
        if item:
            if item.get("status", {}).get("status_str") == "error":
                raise RuntimeError(json.dumps(item["status"], ensure_ascii=False))
            images = [image for node in item.get("outputs", {}).values()
                      for image in node.get("images", [])]
            if images:
                image = images[-1]
                query = urllib.parse.urlencode({"filename": image["filename"],
                    "subfolder": image.get("subfolder", ""), "type": image.get("type", "output")})
                output.mkdir(parents=True, exist_ok=True)
                with urllib.request.urlopen(BASE_URL + "/view?" + query, timeout=120) as response:
                    target.write_bytes(response.read())
                (output / f"{name}.json").write_text(json.dumps({"prompt_id": prompt_id,
                    "seed": seed, "pose": pose}, indent=2), encoding="utf-8")
                print(f"{name}: complete {target}", flush=True)
                return
        time.sleep(3)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", choices=[*STATES, "all"], default="all")
    parser.add_argument("--output", type=Path, default=ROOT / "assets" / "state_anchors")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    names = STATES if args.name == "all" else [args.name]
    for index, name in enumerate(names):
        generate(name, STATES[name], 810000 + list(STATES).index(name), args.output, args.force)


if __name__ == "__main__":
    main()
