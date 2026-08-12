#!/usr/bin/env python3
"""Reapply exact loop/transition endpoints and pupil normalization."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

from generate_state_assets_wan22 import (SOURCE_ANCHORS, STATES, TARGET_ANCHORS,
                                         lock_green_pupils)


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    args = parser.parse_args()
    anchors = ROOT / "assets" / "state_anchors"
    for name in STATES:
        frames = sorted((args.input / f"{name}_frames").glob("frame_*.png"))
        anchor = anchors / f"julia_anchor_{name}.png"
        if len(frames) < 2 or not anchor.exists():
            raise SystemExit(f"missing frames or anchor for {name}")
        shutil.copyfile(anchor, frames[0])
        shutil.copyfile(anchor, frames[-1])
        for frame in frames:
            lock_green_pupils(frame)
    for name, target_name in TARGET_ANCHORS.items():
        frames = sorted((args.input / f"{name}_frames").glob("frame_*.png"))
        source = anchors / SOURCE_ANCHORS[name]
        target = anchors / target_name
        if len(frames) < 2 or not source.exists() or not target.exists():
            raise SystemExit(f"missing frames or anchors for {name}")
        shutil.copyfile(source, frames[0])
        shutil.copyfile(target, frames[-1])
        for frame in frames:
            lock_green_pupils(frame)
    print("postprocessed loops=19 transitions=12")


if __name__ == "__main__":
    main()
