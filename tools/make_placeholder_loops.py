#!/usr/bin/env python3
"""用现有立绘烘焙首批占位循环帧；正式素材可同名替换。"""

import math
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tmp" / "loop_frames"
SOURCES = {
    "idle_loop": "julia_s1_1_near_standby.png",
    "listening_loop": "julia_s3_3_user_call.png",
    "thinking_loop": "julia_s4_2_deep_talk.png",
    "speaking_base_loop": "julia_s4_1_light_dialog.png",
    "sleep_loop": "julia_s0_1_night_sleep.png",
}


def shifted(source, dx, dy, angle=0.0):
    canvas = Image.new("RGB", (360, 360), source.getpixel((0, 0)))
    image = source.rotate(angle, resample=Image.Resampling.BICUBIC, center=(180, 190))
    canvas.paste(image, (round(dx), round(dy)))
    return canvas.quantize(colors=4, method=Image.Quantize.MEDIANCUT,
                           dither=Image.Dither.NONE).convert("RGB")


def main():
    base = ROOT / "main" / "ui" / "generated"
    for name, filename in SOURCES.items():
        target = OUT / name; target.mkdir(parents=True, exist_ok=True)
        for old in target.glob("frame_*.png"):
            old.unlink()
        # 占位素材先统一到 64 色平涂调色板，贴近正式二次元素材，也显著提高 RLE 比率。
        source = Image.open(base / filename).convert("RGB").quantize(
            colors=4, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).convert("RGB")
        frames = 9 if name == "listening_loop" else 8 if name == "thinking_loop" else 12
        if name == "sleep_loop": frames = 16
        for i in range(frames):
            phase = 2 * math.pi * i / frames
            dy, dx, angle = math.sin(phase), 0.0, 0.0
            if name == "idle_loop": dy *= 2.0
            elif name == "listening_loop": dy = 2.0 + 1.5 * (0.5 - 0.5 * math.cos(phase)); angle = .35 * math.sin(phase)
            elif name == "thinking_loop": dx = 1.2 * math.sin(phase); angle = 0.7 + .45 * math.sin(phase)
            elif name == "speaking_base_loop": dy *= 1.5; dx = .5 * math.sin(phase * 2)
            else: dy *= .8
            shifted(source, dx, dy, angle).save(target / f"frame_{i:02d}.png", optimize=True)
        print(f"{name}: {frames} frames")


if __name__ == "__main__":
    main()
