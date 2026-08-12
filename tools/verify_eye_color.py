#!/usr/bin/env python3
"""Audit and optionally normalize Julia eye-layer pupil colors."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image

TARGET_RGB = (32, 200, 40)
TARGET_565 = 0x2645
EYE_ROIS = {
    "left": (20, 11, 47, 43),
    "right": (10, 11, 37, 43),
}


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def iris_candidate(red: int, green: int, blue: int) -> bool:
    green_iris = green >= 48 and green > red * 1.4 and green > blue * 1.4
    brown_iris = red >= 40 and green >= 20 and red > blue * 1.7 and red >= green
    return green_iris or brown_iris


def write_rgb565(path: Path, image: Image.Image) -> None:
    payload = bytearray()
    for red, green, blue in image.convert("RGB").get_flattened_data():
        value = rgb565(red, green, blue)
        payload.extend((value >> 8, value & 0xFF))
    path.write_bytes(payload)


def audit_layer(path: Path, fix: bool) -> dict[str, object]:
    image = Image.open(path).convert("RGB")
    side = "left" if "left" in path.stem else "right"
    x0, y0, x1, y1 = EYE_ROIS[side]
    closed = "closed" in path.stem
    candidates = target = changed = 0
    pixels = image.load()
    for y in range(y0, y1):
        for x in range(x0, x1):
            red, green, blue = pixels[x, y]
            if iris_candidate(red, green, blue):
                candidates += 1
                if rgb565(red, green, blue) == TARGET_565:
                    target += 1
                elif fix and not closed:
                    pixels[x, y] = TARGET_RGB
                    changed += 1
    if fix and changed:
        image.save(path)
        write_rgb565(path.with_suffix(".bin"), image)
        target += changed
    ratio = 1.0 if closed and candidates == 0 else target / max(candidates, 1)
    passed = candidates == 0 if closed else candidates > 0 and ratio >= 0.80
    return {"asset": str(path), "closed": closed, "candidates": candidates,
            "target": target, "ratio": round(ratio, 4), "changed": changed,
            "pass": passed}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--eyes", type=Path,
                        default=Path("main/ui/generated/avatar_layers"))
    parser.add_argument("--fix", action="store_true",
                        help="normalize open/half iris pixels and regenerate .bin files")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    files = sorted(args.eyes.glob("eye_*.png"))
    results = [audit_layer(path, args.fix) for path in files]
    report = {"target_rgb565": "0x2645", "assets": results,
              "pass": len(results) == 6 and all(item["pass"] for item in results)}
    rendered = json.dumps(report, ensure_ascii=False, indent=2)
    print(rendered)
    if args.json:
        args.json.write_text(rendered + "\n", encoding="utf-8")
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
