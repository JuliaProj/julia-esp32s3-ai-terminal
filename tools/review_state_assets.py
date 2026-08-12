#!/usr/bin/env python3
"""Build compact keyframe contact sheets for generated Julia state assets."""

from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np


def sample_video(path: Path, size: int) -> tuple[list[np.ndarray], int]:
    capture = cv2.VideoCapture(str(path))
    count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    wanted = {round((count - 1) * fraction) for fraction in (0, 0.25, 0.5, 0.75, 1)}
    frames = []
    index = 0
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        if index in wanted:
            frames.append(cv2.resize(frame, (size, size), interpolation=cv2.INTER_AREA))
        index += 1
    capture.release()
    if len(frames) != 5:
        raise RuntimeError(f"{path}: expected 5 samples, got {len(frames)}")
    return frames, count


def make_sheet(paths: list[Path], output: Path, size: int) -> None:
    rows = []
    for path in paths:
        frames, count = sample_video(path, size)
        row = np.hstack(frames)
        cv2.rectangle(row, (0, 0), (size * 5, 28), (255, 255, 255), -1)
        cv2.putText(row, f"{path.stem}  {count} frames", (8, 20),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (25, 25, 25), 1, cv2.LINE_AA)
        rows.append(row)
    output.parent.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(output), np.vstack(rows))
    print(f"wrote {output} ({len(rows)} assets)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("assets/state_assets_raw"))
    parser.add_argument("--output", type=Path, default=Path("tmp/state_asset_review"))
    parser.add_argument("--size", type=int, default=180)
    args = parser.parse_args()

    paths = sorted(args.input.glob("*.mp4"))
    # State names end in a numeric substate; transition names end in a state token.
    states = [path for path in paths if path.stem.rsplit("_", 1)[-1].isdigit()]
    transitions = [path for path in paths if path not in states]
    make_sheet(states, args.output / "state_loops_contact.png", args.size)
    make_sheet(transitions, args.output / "state_transitions_contact.png", args.size)


if __name__ == "__main__":
    main()
