#!/usr/bin/env python3
"""Convert reviewed Julia state videos into device-ready RGB565 TRN assets."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONVERTER = ROOT / "tools" / "convert_media_to_trn.py"
ONE_SHOT_STATES = {"S1_1", "S1_3", "S3_3", "S5_1"}


def is_state(name: str) -> bool:
    return name.rsplit("_", 1)[-1].isdigit()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=ROOT / "assets" / "state_assets_raw")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "assets" / "transitions" / "generated")
    parser.add_argument("--resolution", type=int, choices=(180, 360), default=360)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    records = []
    for source in sorted(args.input.glob("*.mp4")):
        name = source.stem
        category = "states" if is_state(name) else "transitions"
        target = args.output / category / name / f"{name}.trn"
        if args.force or not target.exists():
            subprocess.run([
                sys.executable, str(CONVERTER), "--input", str(source),
                "--output", str(target.resolve()), "--fps", "12", "--rle",
                "--resolution", str(args.resolution),
            ], check=True)
        records.append({
            "name": name,
            "kind": "state" if category == "states" else "transition",
            "playback": "one_shot_hold_last" if name in ONE_SHOT_STATES or
                        category == "transitions" else "loop",
            "path": target.relative_to(args.output).as_posix(),
            "bytes": target.stat().st_size,
            "resolution": args.resolution,
            "fps": 12,
            "format": "rgb565_rle",
        })
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = args.output / "manifest.json"
    manifest.write_text(json.dumps({"version": 1, "assets": records}, indent=2),
                        encoding="utf-8")
    print(f"wrote {manifest} ({len(records)} assets, {sum(r['bytes'] for r in records)} bytes)")


if __name__ == "__main__":
    main()
