#!/usr/bin/env python3
"""Build per-frame ComfyUI API workflows for Julia state transitions."""

from __future__ import annotations

import copy
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = Path(__file__).with_name("comfyui_base_workflow.json")

COMMON = (
    "masterpiece, best quality, 1girl, solo, chibi anime girl, same face, "
    "same hairstyle, same outfit, pastel colors, cel shading, clean lines, "
    "minimal detail, simple background, centered full body, 360x360"
)

TRANSITIONS = {
    "S1_S3": {"seed": 31003, "frames": 12, "phases": ["standing relaxed, eyes open", "leaning forward slightly", "eyes widening, attentive", "mouth just opening to speak"]},
    "S3_S4": {"seed": 33004, "frames": 12, "phases": ["leaning forward, concerned", "straightening up", "warm smile", "engaged speaking pose, gentle hand gesture"]},
    "S4_S1": {"seed": 34001, "frames": 12, "phases": ["talking pose", "closing mouth", "soft smile", "relaxed neutral posture, eyes open"]},
    "S1_S0": {"seed": 31000, "frames": 12, "phases": ["standing awake", "head tilting, drowsy", "eyes half closed", "eyes closed, peaceful sleep pose"]},
    "S0_S1": {"seed": 30001, "frames": 12, "phases": ["peacefully sleeping, eyes closed", "eyes fluttering half open", "eyes fully open, surprised", "happy alert relaxed posture"]},
}


def prompt_for(config: dict, frame: int) -> str:
    phases = config["phases"]
    position = frame * (len(phases) - 1) / max(config["frames"] - 1, 1)
    left = min(int(position), len(phases) - 1)
    right = min(left + 1, len(phases) - 1)
    blend = position - left
    return f"{COMMON}, animation keyframe, transition from ({phases[left]}) toward ({phases[right]}), progress {blend:.2f}"


def build(transition: str, frame: int, checkpoint: str, reference: str) -> dict:
    config = TRANSITIONS[transition]
    workflow = json.loads(BASE.read_text(encoding="utf-8"))
    workflow.pop("_meta", None)
    workflow["1"]["inputs"]["ckpt_name"] = checkpoint
    workflow["2"]["inputs"]["text"] = prompt_for(config, frame)
    workflow["5"]["inputs"]["image"] = reference
    # Keep the noise seed fixed across a route. IPAdapter and ControlNet then
    # anchor identity while the prompt supplies the pose progression.
    workflow["12"]["inputs"]["seed"] = config["seed"]
    workflow["14"]["inputs"]["filename_prefix"] = f"julia/{transition}/frame_{frame + 1:04d}"
    return workflow


def write_all(output: Path, checkpoint: str, reference: str) -> None:
    output.mkdir(parents=True, exist_ok=True)
    for transition, config in TRANSITIONS.items():
        target = output / transition
        target.mkdir(parents=True, exist_ok=True)
        for frame in range(config["frames"]):
            workflow = build(transition, frame, checkpoint, reference)
            (target / f"frame_{frame + 1:04d}.json").write_text(
                json.dumps(workflow, indent=2), encoding="utf-8"
            )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "assets/comfyui_workflows")
    parser.add_argument("--checkpoint", default="Counterfeit-V3.0_fp16.safetensors")
    parser.add_argument("--reference", default="julia_reference.png")
    args = parser.parse_args()
    write_all(args.output, args.checkpoint, args.reference)
    print(f"workflows written to {args.output}")
