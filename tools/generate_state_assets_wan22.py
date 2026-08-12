#!/usr/bin/env python3
"""Generate Julia state loops and transitions with the local Wan 2.2 I2V stack."""

from __future__ import annotations

import argparse
import json
import shutil
import time
import urllib.parse
import urllib.request
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
BASE_URL = "http://127.0.0.1:8188"
REFERENCE = "julia_standby_reference_v3.png"
SOURCE_ANCHORS = {
    "S0_S1": "julia_anchor_S0_1.png",
    "S1_S0": "julia_anchor_S1_1.png",
    "S1_S2": "julia_anchor_S1_1.png",
    "S2_S1": "julia_anchor_S2_1.png",
    "S1_S3": "julia_anchor_S1_1.png",
    "S2_S3": "julia_anchor_S2_1.png",
    "S3_S4": "julia_anchor_S3_3.png",
    "S4_S1": "julia_anchor_S4_1.png",
    "S4_S5": "julia_anchor_S4_1.png",
    "S5_S1": "julia_anchor_S5_1.png",
    "S5_S2": "julia_anchor_S5_1.png",
    "S5_S4": "julia_anchor_S5_1.png",
}
TARGET_ANCHORS = {
    "S0_S1": "julia_anchor_S1_1.png", "S1_S0": "julia_anchor_S0_1.png",
    "S1_S2": "julia_anchor_S2_1.png", "S2_S1": "julia_anchor_S1_1.png",
    "S1_S3": "julia_anchor_S3_3.png", "S2_S3": "julia_anchor_S3_3.png",
    "S3_S4": "julia_anchor_S4_1.png", "S4_S1": "julia_anchor_S1_1.png",
    "S4_S5": "julia_anchor_S5_1.png", "S5_S1": "julia_anchor_S1_1.png",
    "S5_S2": "julia_anchor_S2_1.png", "S5_S4": "julia_anchor_S4_1.png",
}


def lock_green_pupils(path: Path) -> None:
    with Image.open(path) as source:
        image = source.convert("RGB")
    pixels = image.load()
    scale_x, scale_y = image.width / 360.0, image.height / 360.0
    for x0, y0, x1, y1 in ((120, 125, 175, 180), (185, 125, 240, 180)):
        for y in range(round(y0 * scale_y), round(y1 * scale_y)):
            for x in range(round(x0 * scale_x), round(x1 * scale_x)):
                red, green, blue = pixels[x, y]
                if green >= 45 and green > red * 1.25 and green > blue * 1.25:
                    pixels[x, y] = (76, 175, 80)
    temporary = path.with_suffix(path.suffix + ".tmp")
    image.save(temporary, format="PNG")
    temporary.replace(path)

IDENTITY = (
    "the exact same Julia character as the reference image, same face and proportions, "
    "short ivory white bob hair with pale teal tips, fixed green eyes, same eye color in every frame, "
    "absolutely consistent pupil color #4CAF50, no eye color variation, "
    "no lighting-induced eye color change, teal star hair clip, "
    "cream cardigan, mint high collar, teal star badge, centered waist-up portrait, fixed camera, "
    "clean off-white softly lit background, polished pastel anime cel shading"
)
COMMON_MOTION = (
    "slow restrained natural motion, continuous ease-in and ease-out, stable face, stable outfit, "
    "stable background, no camera motion, no cut, no zoom, the final pose returns exactly to the "
    "starting pose for a seamless loop, full character silhouette remains inside the frame, "
    "no hair or clothing cropped at the image edge, simple stable solid-color gradient background"
)
NEGATIVE = (
    "different person, identity change, different face, different hair, different clothes, fast motion, "
    "jerky movement, camera movement, zoom, scene cut, morphing, flicker, jitter, extra arms, extra hands, "
    "extra fingers, malformed hands, duplicated object, deformed body, eye color change, blue eyes, "
    "amber eyes, red eyes, cropped hair, cropped clothing, complex background, text, watermark, logo, blur"
)

# Loops end near their starting pose. 33 frames at 12 fps is about 2.75 seconds.
STATES = {
    "S0_1": "sleeping deeply, eyes fully closed, peaceful expression, only extremely subtle slow breathing",
    "S0_2": "sleeping while alone in daytime, eyes closed, one tiny wistful brow movement, then relaxes",
    "S0_3": "finishes a gentle good-night nod, closes her eyes and settles into manual sleep",
    "S1_1": "shallow resting pose, eyes closed, briefly peeks with half-open eyes, tiny smile, closes eyes again",
    "S1_2": "far standby, eyes fully closed and body completely relaxed, nearly motionless except breathing",
    "S1_3": "resting while charging, eyes closed, briefly opens eyes with a grateful smile, settles again",
    "S2_1": "starts already holding and reading the same small dark teal book, slowly looks up once "
    "with a warm smile, then lowers her gaze and returns to the exact initial reading pose",
    "S2_2": "quietly shares an activity, listens to soft music, very gently sways once and smiles",
    "S2_3": "sleepy bedtime companion, rubs one eye, gives a small yawn, settles with half-closed eyes",
    "S3_1": "notices sadness, opens eyes with concern, leans forward slightly and listens, mouth closed",
    "S3_2": "notices a broken routine, tilts her head with gentle concern, raises one eyebrow, mouth closed",
    "S3_3": "hears her name, opens her eyes brightly, turns attentive and gives a delighted welcoming smile",
    "S3_4": "carefully checks in after silence, tentative small wave, soft worried smile, waits quietly",
    "S4_1": "light conversation, attentive listening, one natural nod and blink, relaxed friendly expression",
    "S4_2": "deep listening, focused warm eye contact, two slow empathetic nods, subtle concerned expression",
    "S4_3": "comfortable multi-turn chat, rests chin on one hand, playful head tilt and small laugh",
    "S4_4": "conversation interrupted, pauses with a puzzled expression, looks aside, then waits patiently",
    "S5_1": "accepts a clear rejection, expression becomes quietly sad, lowers gaze and slowly closes eyes",
    "S5_2": "notices perfunctory replies, briefly looks hurt and confused, then calmly accepts the silence",
}

# High-value directed edges. Reverse edges are explicit because motions are not safely reversible.
TRANSITIONS = {
    "S0_S1": "starts asleep with eyes closed, slowly wakes, half opens then fully opens eyes, calm small smile",
    "S1_S0": "starts shallow resting, slowly becomes drowsy, lowers head and fully closes eyes into deep sleep",
    "S1_S2": "starts with eyes closed resting, gently opens eyes, straightens posture and becomes quietly attentive",
    "S2_S1": "starts quietly attentive, puts down the book, relaxes shoulders and slowly closes eyes to rest",
    "S1_S3": "starts resting, hears the user, opens eyes with recognition and leans forward attentively",
    "S2_S3": "starts doing a quiet activity, looks up, puts the object down and turns attentively toward the user",
    "S3_S4": "starts attentive and concerned, receives a response, relaxes into warm engaged conversation",
    "S4_S1": "finishes speaking, closes mouth, gives a soft goodbye smile, relaxes and closes eyes to shallow rest",
    "S4_S5": "starts in conversation, hears rejection, accepts it, lowers gaze and slowly becomes still",
    "S5_S1": "starts quietly withdrawn, tension fades, returns to a peaceful eyes-closed standby pose",
    "S5_S2": "starts withdrawn, notices a positive response, cautiously opens eyes and resumes quiet companionship",
    "S5_S4": "starts withdrawn, user re-engages, opens eyes with relief and returns to warm conversation",
}


def request_json(path: str, payload=None, timeout: int = 30):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(BASE_URL + path, data=data,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.load(response)


def workflow(name: str, motion: str, frames: int, seed: int) -> dict:
    positive = f"{IDENTITY}. {motion}. {COMMON_MOTION}."
    split = 12 if frames >= 33 else 10
    steps = 24 if frames >= 33 else 20
    return {
        "1": {"class_type": "LoadImage", "inputs": {
            "image": SOURCE_ANCHORS.get(name, f"julia_anchor_{name}.png")}},
        "2": {"class_type": "CLIPLoader", "inputs": {"clip_name":
            "umt5_xxl_fp8_e4m3fn_scaled.safetensors", "type": "wan", "device": "default"}},
        "3": {"class_type": "CLIPTextEncode", "inputs": {"text": positive, "clip": ["2", 0]}},
        "4": {"class_type": "CLIPTextEncode", "inputs": {"text": NEGATIVE, "clip": ["2", 0]}},
        "5": {"class_type": "VAELoader", "inputs": {"vae_name": "wan_2.1_vae.safetensors"}},
        "6": {"class_type": "CLIPVisionLoader", "inputs": {"clip_name": "clip_vision_h.safetensors"}},
        "7": {"class_type": "CLIPVisionEncode", "inputs": {
            "clip_vision": ["6", 0], "image": ["1", 0], "crop": "none"}},
        "8": {"class_type": "WanImageToVideo", "inputs": {
            "positive": ["3", 0], "negative": ["4", 0], "vae": ["5", 0],
            "width": 640, "height": 640, "length": frames, "batch_size": 1,
            "clip_vision_output": ["7", 0], "start_image": ["1", 0]}},
        "9": {"class_type": "UNETLoader", "inputs": {"unet_name":
            "wan2.2_i2v_high_noise_14B_fp8_scaled.safetensors", "weight_dtype": "default"}},
        "10": {"class_type": "ModelSamplingSD3", "inputs": {"model": ["9", 0], "shift": 5.0}},
        "11": {"class_type": "KSamplerAdvanced", "inputs": {"model": ["10", 0],
            "add_noise": "enable", "noise_seed": seed, "steps": steps, "cfg": 5.5,
            "sampler_name": "euler", "scheduler": "simple", "positive": ["8", 0],
            "negative": ["8", 1], "latent_image": ["8", 2], "start_at_step": 0,
            "end_at_step": split, "return_with_leftover_noise": "enable"}},
        "12": {"class_type": "UNETLoader", "inputs": {"unet_name":
            "wan2.2_i2v_low_noise_14B_fp8_scaled.safetensors", "weight_dtype": "default"}},
        "13": {"class_type": "ModelSamplingSD3", "inputs": {"model": ["12", 0], "shift": 5.0}},
        "14": {"class_type": "KSamplerAdvanced", "inputs": {"model": ["13", 0],
            "add_noise": "disable", "noise_seed": seed, "steps": steps, "cfg": 5.5,
            "sampler_name": "euler", "scheduler": "simple", "positive": ["8", 0],
            "negative": ["8", 1], "latent_image": ["11", 0], "start_at_step": split,
            "end_at_step": steps, "return_with_leftover_noise": "disable"}},
        "15": {"class_type": "VAEDecode", "inputs": {"samples": ["14", 0], "vae": ["5", 0]}},
        "16": {"class_type": "CreateVideo", "inputs": {"images": ["15", 0], "fps": 12.0,
            "bit_depth": 8}},
        "17": {"class_type": "SaveVideo", "inputs": {"video": ["16", 0],
            "filename_prefix": f"julia/state_assets/{name}", "format": "mp4", "codec": "h264"}},
        "18": {"class_type": "SaveImage", "inputs": {"images": ["15", 0],
            "filename_prefix": f"julia/state_assets/{name}_frames/frame"}},
    }


def output_files(history: dict) -> list[dict]:
    files = []
    for node in history.get("outputs", {}).values():
        files.extend(node.get("images", []))
        files.extend(node.get("videos", []))
        files.extend(node.get("gifs", []))
    return files


def download(file: dict, destination: Path) -> None:
    query = urllib.parse.urlencode({"filename": file["filename"],
        "subfolder": file.get("subfolder", ""), "type": file.get("type", "output")})
    destination.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(BASE_URL + "/view?" + query, timeout=120) as response:
        destination.write_bytes(response.read())


def generate(name: str, motion: str, frames: int, seed: int, out_dir: Path,
             force: bool) -> None:
    target = out_dir / f"{name}.mp4"
    if target.exists() and not force:
        print(f"{name}: skip existing {target}", flush=True)
        return
    queued = request_json("/prompt", {"prompt": workflow(name, motion, frames, seed)})
    prompt_id = queued["prompt_id"]
    print(f"{name}: queued prompt_id={prompt_id}", flush=True)
    while True:
        item = request_json(f"/history/{prompt_id}").get(prompt_id)
        if item:
            if item.get("status", {}).get("status_str") == "error":
                raise RuntimeError(json.dumps(item["status"], ensure_ascii=False))
            files = output_files(item)
            video = next((f for f in files if Path(f["filename"]).suffix.lower() == ".mp4"), None)
            if video:
                download(video, target)
                images = [item for item in files
                          if Path(item["filename"]).suffix.lower() == ".png"]
                frame_dir = out_dir / f"{name}_frames"
                frame_dir.mkdir(parents=True, exist_ok=True)
                for index, image in enumerate(images, 1):
                    download(image, frame_dir / f"frame_{index:04d}.png")
                generated = sorted(frame_dir.glob("frame_*.png"))
                if len(generated) >= 2:
                    if name in TARGET_ANCHORS:
                        source = ROOT / "assets" / "state_anchors" / SOURCE_ANCHORS[name]
                        destination = ROOT / "assets" / "state_anchors" / TARGET_ANCHORS[name]
                        if source.exists() and destination.exists():
                            shutil.copyfile(source, generated[0])
                            shutil.copyfile(destination, generated[-1])
                    else:
                        shutil.copyfile(generated[0], generated[-1])
                for frame in generated:
                    lock_green_pupils(frame)
                (out_dir / f"{name}.json").write_text(json.dumps({"prompt_id": prompt_id,
                    "seed": seed, "frames": frames, "fps": 12, "motion": motion,
                    "endpoint_locked": len(generated) >= 2}, indent=2),
                    encoding="utf-8")
                print(f"{name}: complete {target}", flush=True)
                return
        time.sleep(5)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=["state", "transition", "all"], default="all")
    parser.add_argument("--name", help="single asset name, for example S1_1 or S1_S2")
    parser.add_argument("--output", type=Path, default=ROOT / "assets" / "state_assets_raw")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    jobs = []
    if args.kind in ("state", "all"):
        jobs += [(name, motion, 33, 820000 + i) for i, (name, motion) in enumerate(STATES.items())]
    if args.kind in ("transition", "all"):
        jobs += [(name, motion, 17, 830000 + i) for i, (name, motion) in enumerate(TRANSITIONS.items())]
    if args.name:
        jobs = [job for job in jobs if job[0] == args.name]
        if not jobs:
            raise SystemExit(f"unknown asset for selected kind: {args.name}")
    for job in jobs:
        generate(*job, args.output, args.force)


if __name__ == "__main__":
    main()
