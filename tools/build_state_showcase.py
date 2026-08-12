#!/usr/bin/env python3
"""Join all generated Julia state loops and transitions into one review video."""

from __future__ import annotations

import argparse
from pathlib import Path

import cv2


def is_state(name: str) -> bool:
    return name.rsplit("_", 1)[-1].isdigit()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("assets/state_assets_raw"))
    parser.add_argument("--output", type=Path,
                        default=Path("assets/state_assets_raw/julia_all_states_showcase.mp4"))
    args = parser.parse_args()

    sources = [path for path in sorted(args.input.glob("*.mp4"))
               if path.resolve() != args.output.resolve()]
    sources.sort(key=lambda path: (not is_state(path.stem), path.stem))
    if not sources:
        raise SystemExit("no source MP4 files found")

    fps, size = 12.0, (640, 640)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    writer = cv2.VideoWriter(str(args.output), cv2.VideoWriter_fourcc(*"mp4v"), fps, size)
    if not writer.isOpened():
        raise SystemExit("could not create showcase video")

    for source in sources:
        capture = cv2.VideoCapture(str(source))
        kind = "STATE LOOP" if is_state(source.stem) else "TRANSITION"
        last_frame = None
        while True:
            ok, frame = capture.read()
            if not ok:
                break
            frame = cv2.resize(frame, size, interpolation=cv2.INTER_AREA)
            cv2.rectangle(frame, (12, 12), (310, 58), (245, 245, 245), -1)
            cv2.putText(frame, f"{kind}  {source.stem}", (24, 44),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.72, (25, 55, 55), 2, cv2.LINE_AA)
            writer.write(frame)
            last_frame = frame
        capture.release()
        if last_frame is not None:
            for _ in range(4):
                writer.write(last_frame)
    writer.release()
    print(f"wrote {args.output} ({len(sources)} clips)")


if __name__ == "__main__":
    main()
