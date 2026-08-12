#!/usr/bin/env python3
"""Generate all Julia loops/transitions with the consistency-locked Wan workflow."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    command = [sys.executable, str(ROOT / "tools" / "generate_state_assets_wan22.py"),
               "--kind", "all", *sys.argv[1:]]
    raise SystemExit(subprocess.call(command, cwd=ROOT))


if __name__ == "__main__":
    main()
