#!/usr/bin/env python3
"""Submit the Julia standby stretch Wan 2.2 I2V workflow to ComfyUI."""

import json
import time
import urllib.request


BASE_URL = "http://127.0.0.1:8188"

POSITIVE = (
    "The exact same chibi anime girl from the reference remains centered and faces the camera. "
    "She begins in a relaxed idle pose, slowly raises both forearms and shoulders into a modest "
    "sleepy stretch, gently tilts her head, holds the stretch naturally for a brief moment, then "
    "lowers her arms and returns to the exact original relaxed idle pose. Smooth continuous subtle "
    "motion, stable face and body proportions, same short white hair with teal tips, same golden eyes, "
    "same star hair clip, same cream cardigan and mint collar, clean pure white background, fixed "
    "camera, consistent framing, polished clean cel shading, seamless start and end pose."
)

NEGATIVE = (
    "identity change, different character, different clothes, different hairstyle, camera movement, "
    "camera shake, zoom, scene change, extra fingers, missing fingers, fused hands, malformed hands, "
    "extra arms, missing arms, duplicated limbs, deformed arms, detached limbs, cropped face, face "
    "distortion, body morphing, background motion, text, watermark, flicker, jitter, abrupt motion, "
    "low quality, blur"
)


def workflow(seed: int = 8062026) -> dict:
    return {
        "1": {"class_type": "LoadImage", "inputs": {"image": "julia_standby_reference_v3.png"}},
        "2": {"class_type": "CLIPLoader", "inputs": {
            "clip_name": "umt5_xxl_fp8_e4m3fn_scaled.safetensors", "type": "wan", "device": "default"}},
        "3": {"class_type": "CLIPTextEncode", "inputs": {"text": POSITIVE, "clip": ["2", 0]}},
        "4": {"class_type": "CLIPTextEncode", "inputs": {"text": NEGATIVE, "clip": ["2", 0]}},
        "5": {"class_type": "VAELoader", "inputs": {"vae_name": "wan_2.1_vae.safetensors"}},
        "6": {"class_type": "CLIPVisionLoader", "inputs": {"clip_name": "clip_vision_h.safetensors"}},
        "7": {"class_type": "CLIPVisionEncode", "inputs": {
            "clip_vision": ["6", 0], "image": ["1", 0], "crop": "none"}},
        "8": {"class_type": "WanImageToVideo", "inputs": {
            "positive": ["3", 0], "negative": ["4", 0], "vae": ["5", 0],
            "width": 640, "height": 640, "length": 33, "batch_size": 1,
            "clip_vision_output": ["7", 0], "start_image": ["1", 0]}},
        "9": {"class_type": "UNETLoader", "inputs": {
            "unet_name": "wan2.2_i2v_high_noise_14B_fp8_scaled.safetensors", "weight_dtype": "default"}},
        "10": {"class_type": "ModelSamplingSD3", "inputs": {"model": ["9", 0], "shift": 5.0}},
        "11": {"class_type": "KSamplerAdvanced", "inputs": {
            "model": ["10", 0], "add_noise": "enable", "noise_seed": seed, "steps": 20,
            "cfg": 4.0, "sampler_name": "euler", "scheduler": "simple",
            "positive": ["8", 0], "negative": ["8", 1], "latent_image": ["8", 2],
            "start_at_step": 0, "end_at_step": 10, "return_with_leftover_noise": "enable"}},
        "12": {"class_type": "UNETLoader", "inputs": {
            "unet_name": "wan2.2_i2v_low_noise_14B_fp8_scaled.safetensors", "weight_dtype": "default"}},
        "13": {"class_type": "ModelSamplingSD3", "inputs": {"model": ["12", 0], "shift": 5.0}},
        "14": {"class_type": "KSamplerAdvanced", "inputs": {
            "model": ["13", 0], "add_noise": "disable", "noise_seed": seed, "steps": 20,
            "cfg": 4.0, "sampler_name": "euler", "scheduler": "simple",
            "positive": ["8", 0], "negative": ["8", 1], "latent_image": ["11", 0],
            "start_at_step": 10, "end_at_step": 20, "return_with_leftover_noise": "disable"}},
        "15": {"class_type": "VAEDecode", "inputs": {"samples": ["14", 0], "vae": ["5", 0]}},
        "16": {"class_type": "CreateVideo", "inputs": {"images": ["15", 0], "fps": 16.0, "bit_depth": 8}},
        "17": {"class_type": "SaveVideo", "inputs": {
            "video": ["16", 0], "filename_prefix": "julia/standby_stretch_v1", "format": "mp4", "codec": "h264"}},
    }


def request_json(path: str, payload=None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(BASE_URL + path, data=data,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.load(response)


def main():
    result = request_json("/prompt", {"prompt": workflow()})
    prompt_id = result["prompt_id"]
    print(f"prompt_id={prompt_id}", flush=True)
    while True:
        history = request_json(f"/history/{prompt_id}")
        if prompt_id in history:
            item = history[prompt_id]
            if item.get("status", {}).get("status_str") == "error":
                raise RuntimeError(json.dumps(item["status"], ensure_ascii=False))
            if item.get("outputs"):
                print(json.dumps(item["outputs"], ensure_ascii=False, indent=2))
                return
        time.sleep(5)


if __name__ == "__main__":
    main()
