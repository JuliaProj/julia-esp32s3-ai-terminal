#!/usr/bin/env python3
"""Build validated Julia v2 loops and directed transitions as deployable TRN files."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "state_assets_consistent_v2"
OUTPUT = ROOT / "assets" / "transitions" / "consistent_v2"
IDLE = {
    "S0_1": "S0_1_sleep_breathing.trn", "S0_2": "S0_2_sleep_waiting.trn",
    "S0_3": "S0_3_sleep_goodnight.trn", "S1_1": "S1_1_standby_peek.trn",
    "S1_2": "S1_2_standby_deep.trn", "S1_3": "S1_3_standby_charge.trn",
    "S2_1": "S2_1_companion_read.trn", "S2_2": "S2_2_companion_activity.trn",
    "S2_3": "S2_3_companion_sleepy.trn", "S3_1": "S3_1_approach_concern.trn",
    "S3_2": "S3_2_approach_worry.trn", "S3_3": "S3_3_approach_happy.trn",
    "S3_4": "S3_4_approach_careful.trn", "S4_1": "S4_1_chat_natural.trn",
    "S4_2": "S4_2_chat_listen.trn", "S4_3": "S4_3_chat_playful.trn",
    "S4_4": "S4_4_chat_confused.trn", "S5_1": "S5_1_reject_sad.trn",
    "S5_2": "S5_2_reject_hurt.trn",
}
TRANSITIONS = ("S0_S1", "S1_S0", "S1_S2", "S2_S1", "S1_S3", "S2_S3",
               "S3_S4", "S4_S1", "S4_S5", "S5_S1", "S5_S2", "S5_S4")


def convert(source: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / "tools" / "convert_media_to_trn.py"),
                    "--input", str(source), "--output", str(output), "--fps", "12",
                    "--resolution", "360", "--rle"], cwd=ROOT, check=True)


def main() -> None:
    for state, filename in IDLE.items():
        convert(SOURCE / f"{state}_frames", OUTPUT / "idle" / filename)
    for route in TRANSITIONS:
        convert(SOURCE / f"{route}_frames",
                OUTPUT / "transitions" / route / f"{route}.trn")
    print(f"built idle={len(IDLE)} transitions={len(TRANSITIONS)} output={OUTPUT}")


if __name__ == "__main__":
    main()
