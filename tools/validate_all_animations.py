#!/usr/bin/env python3
"""Run validate_animation.py for every generated Julia loop and transition."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_state_assets_wan22 import SOURCE_ANCHORS, STATES, TARGET_ANCHORS, TRANSITIONS


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "assets" / "state_assets_consistent_v2"
    reports = source / "validation"
    reports.mkdir(parents=True, exist_ok=True)
    results = {}
    for name in (*STATES, *TRANSITIONS):
        command = [sys.executable, str(ROOT / "tools" / "validate_animation.py"),
                   str(source / f"{name}_frames"), "--report", str(reports / f"{name}.json")]
        if name in TRANSITIONS:
            anchors = ROOT / "assets" / "state_anchors"
            command += ["--source", str(anchors / SOURCE_ANCHORS[name]),
                        "--target", str(anchors / TARGET_ANCHORS[name])]
        result = subprocess.run(command, stdout=subprocess.DEVNULL)
        report_path = reports / f"{name}.json"
        report = json.loads(report_path.read_text(encoding="utf-8")) if report_path.exists() else {}
        results[name] = {"passed": result.returncode == 0, "failures": report.get("failures", [])}
        print(f"{name}: {'PASS' if result.returncode == 0 else 'FAIL'}")
    summary = {"passed": all(item["passed"] for item in results.values()), "assets": results}
    (reports / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    passed = sum(item["passed"] for item in results.values())
    print(f"summary: {passed}/{len(results)} passed")
    raise SystemExit(0 if summary["passed"] else 1)


if __name__ == "__main__":
    main()
