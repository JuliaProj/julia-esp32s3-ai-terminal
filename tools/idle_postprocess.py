#!/usr/bin/env python3
"""Build Julia idle .trn clips and report loop/size characteristics."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[1]
ACTIONS = ("stretch", "drink", "read", "daze")


def loop_error(directory: Path) -> float:
    frames = sorted(directory.glob("*.png"))
    with Image.open(frames[0]) as first, Image.open(frames[-1]) as last:
        diff = ImageChops.difference(first.convert("RGB"), last.convert("RGB"))
        return sum(ImageStat.Stat(diff).mean) / (3.0 * 255.0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resolution", type=int, choices=(180, 360), default=180)
    parser.add_argument("--frames", type=int, default=36)
    parser.add_argument("--fps", type=int, default=12)
    parser.add_argument("--output", type=Path, default=ROOT / "assets" / "idle_trn")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"fps": args.fps, "frame_count": args.frames,
              "display_resolution": [360, 360], "encoded_resolution": args.resolution,
              "clips": []}
    for action in ACTIONS:
        source = ROOT / "assets" / "idle" / f"{action}_frames"
        output = args.output / f"S1_idle_{action}.trn"
        subprocess.run([
            sys.executable, str(ROOT / "tools" / "convert_media_to_trn.py"),
            "--input", str(source), "--output", str(output), "--fps", str(args.fps),
            "--rle", "--resolution", str(args.resolution),
            "--frame-count", str(args.frames),
        ], check=True)
        report["clips"].append({"action": action, "path": str(output.relative_to(ROOT)),
                                "bytes": output.stat().st_size,
                                "duration_s": args.frames / args.fps,
                                "first_last_mean_error": round(loop_error(source), 5)})
    report_path = args.output / "idle_assets_report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"report={report_path}")


if __name__ == "__main__":
    main()
