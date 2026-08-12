#!/usr/bin/env python3
"""Build a filming-friendly four-action idle showcase frame sequence."""

from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "idle"
OUTPUT = ROOT / "assets" / "idle_showcase_frames"
ACTIONS = ("drink", "read", "stretch", "daze")
ACTION_FRAMES = 18
HOLD_FRAMES = 0
FPS = 6


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for old in OUTPUT.glob("*.png"):
        old.unlink()
    output_index = 1
    for action in ACTIONS:
        source = sorted((SOURCE / f"{action}_frames").glob("*.png"))
        if len(source) < 2:
            raise SystemExit(f"missing frames for {action}")
        chosen = [source[round(i * (len(source) - 1) / (ACTION_FRAMES - 1))]
                  for i in range(ACTION_FRAMES)]
        for frame in chosen:
            shutil.copyfile(frame, OUTPUT / f"frame_{output_index:04d}.png")
            output_index += 1
        for _ in range(HOLD_FRAMES):
            shutil.copyfile(chosen[-1], OUTPUT / f"frame_{output_index:04d}.png")
            output_index += 1
    print(f"showcase={OUTPUT} frames={output_index - 1} fps={FPS} duration_s={(output_index - 1) / FPS:.2f}")


if __name__ == "__main__":
    main()
