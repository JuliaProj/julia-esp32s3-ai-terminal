#!/usr/bin/env python3
"""Generate Julia's four standby actions with the local ComfyUI Wan I2V stack."""

from __future__ import annotations

import argparse
import json
import time
import urllib.request


BASE_URL = "http://127.0.0.1:8188"
REFERENCE = "julia_standby_reference_v3.png"

IDENTITY = (
    "the exact same chibi anime girl as the reference, short white bob hair with teal tips, "
    "golden eyes, teal star hair clip, cream cardigan with a mint collar and teal star badge, "
    "same face, same hairstyle, same outfit, centered waist-up composition, fixed camera, "
    "clean white softly lit background, clean pastel cel shading"
)

ACTIONS = {
    "stretch": {
        "seed": 8062101,
        "motion": (
            "She starts in the exact relaxed idle pose. Over two full seconds she very slowly raises "
            "both forearms and shoulders into a lazy morning stretch, closes her eyes and yawns gently, "
            "then slowly lowers both arms and returns to the exact starting pose. The motion is calm, "
            "unhurried, continuous, low amplitude, with gentle ease-in and ease-out and a seamless loop."
        ),
    },
    "drink": {
        "seed": 8062102,
        "motion": (
            "She calmly holds a small clear cup with both hands near her chest, slowly lifts it to her "
            "lips, takes one small quiet sip, then slowly lowers it to the original position and returns "
            "to the exact starting pose. Subtle natural movement, gentle ease-in and ease-out, seamless loop."
        ),
    },
    "read": {
        "seed": 8062103,
        "motion": (
            "She holds a small open book at chest height, quietly reads, her eyes move subtly across a "
            "page, then she slowly turns one page and returns her hands and gaze to the exact starting "
            "pose. Peaceful, minimal, unhurried motion, gentle ease-in and ease-out, seamless loop."
        ),
    },
    "daze": {
        "seed": 8062104,
        "motion": (
            "She slowly rests her chin on one palm, gazes dreamily slightly to the side, blinks once, "
            "her hair tips sway almost imperceptibly, then she slowly returns to the exact relaxed starting "
            "pose. Extremely subtle peaceful movement, gentle ease-in and ease-out, seamless loop."
        ),
    },
}

NEGATIVE = (
    "fast motion, sudden motion, jerky motion, camera movement, camera shake, zoom, scene cut, identity "
    "change, different face, different hairstyle, different outfit, extra arms, extra hands, extra fingers, "
    "missing fingers, fused fingers, malformed hands, duplicated objects, deformed limbs, detached limbs, "
    "face distortion, body morphing, flicker, jitter, background motion, text, watermark, blur, low quality"
)


def request_json(path: str, payload=None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(BASE_URL + path, data=data,
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def workflow(name: str, seed: int, motion: str) -> dict:
    positive = f"{IDENTITY}. {motion}"
    return {
        "1": {"class_type": "LoadImage", "inputs": {"image": REFERENCE}},
        "2": {"class_type": "CLIPLoader", "inputs": {
            "clip_name": "umt5_xxl_fp8_e4m3fn_scaled.safetensors", "type": "wan", "device": "default"}},
        "3": {"class_type": "CLIPTextEncode", "inputs": {"text": positive, "clip": ["2", 0]}},
        "4": {"class_type": "CLIPTextEncode", "inputs": {"text": NEGATIVE, "clip": ["2", 0]}},
        "5": {"class_type": "VAELoader", "inputs": {"vae_name": "wan_2.1_vae.safetensors"}},
        "6": {"class_type": "CLIPVisionLoader", "inputs": {"clip_name": "clip_vision_h.safetensors"}},
        "7": {"class_type": "CLIPVisionEncode", "inputs": {
            "clip_vision": ["6", 0], "image": ["1", 0], "crop": "none"}},
        "8": {"class_type": "WanImageToVideo", "inputs": {
            "positive": ["3", 0], "negative": ["4", 0], "vae": ["5", 0],
            "width": 640, "height": 640, "length": 45, "batch_size": 1,
            "clip_vision_output": ["7", 0], "start_image": ["1", 0]}},
        "9": {"class_type": "UNETLoader", "inputs": {
            "unet_name": "wan2.2_i2v_high_noise_14B_fp8_scaled.safetensors", "weight_dtype": "default"}},
        "10": {"class_type": "ModelSamplingSD3", "inputs": {"model": ["9", 0], "shift": 5.0}},
        "11": {"class_type": "KSamplerAdvanced", "inputs": {
            "model": ["10", 0], "add_noise": "enable", "noise_seed": seed, "steps": 24,
            "cfg": 5.5, "sampler_name": "euler", "scheduler": "simple",
            "positive": ["8", 0], "negative": ["8", 1], "latent_image": ["8", 2],
            "start_at_step": 0, "end_at_step": 12, "return_with_leftover_noise": "enable"}},
        "12": {"class_type": "UNETLoader", "inputs": {
            "unet_name": "wan2.2_i2v_low_noise_14B_fp8_scaled.safetensors", "weight_dtype": "default"}},
        "13": {"class_type": "ModelSamplingSD3", "inputs": {"model": ["12", 0], "shift": 5.0}},
        "14": {"class_type": "KSamplerAdvanced", "inputs": {
            "model": ["13", 0], "add_noise": "disable", "noise_seed": seed, "steps": 24,
            "cfg": 5.5, "sampler_name": "euler", "scheduler": "simple",
            "positive": ["8", 0], "negative": ["8", 1], "latent_image": ["11", 0],
            "start_at_step": 12, "end_at_step": 24, "return_with_leftover_noise": "disable"}},
        "15": {"class_type": "VAEDecode", "inputs": {"samples": ["14", 0], "vae": ["5", 0]}},
        "16": {"class_type": "CreateVideo", "inputs": {"images": ["15", 0], "fps": 12.0, "bit_depth": 8}},
        "17": {"class_type": "SaveVideo", "inputs": {
            "video": ["16", 0], "filename_prefix": f"julia/idle/{name}",
            "format": "mp4", "codec": "h264"}},
        "18": {"class_type": "SaveImage", "inputs": {
            "images": ["15", 0], "filename_prefix": f"julia/idle/{name}_frames/frame"}},
    }


def generate(name: str) -> None:
    config = ACTIONS[name]
    result = request_json("/prompt", {"prompt": workflow(name, config["seed"], config["motion"])})
    prompt_id = result["prompt_id"]
    print(f"{name}: prompt_id={prompt_id}", flush=True)
    while True:
        history = request_json(f"/history/{prompt_id}")
        if prompt_id in history:
            item = history[prompt_id]
            if item.get("status", {}).get("status_str") == "error":
                raise RuntimeError(json.dumps(item["status"], ensure_ascii=False))
            if item.get("outputs"):
                print(f"{name}: complete outputs={json.dumps(item['outputs'], ensure_ascii=False)}",
                      flush=True)
                return
        time.sleep(5)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--action", choices=[*ACTIONS, "all"], default="all")
    args = parser.parse_args()
    names = ACTIONS if args.action == "all" else [args.action]
    for name in names:
        generate(name)


if __name__ == "__main__":
    main()
