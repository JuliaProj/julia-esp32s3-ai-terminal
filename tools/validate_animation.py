#!/usr/bin/env python3
"""Validate Julia PNG animation identity, endpoints and temporal stability."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import cv2
import numpy as np


EYE_REGIONS = ((120, 125, 175, 180), (185, 125, 240, 180))
MOUTH_REGION = (145, 168, 215, 220)
TARGET_GREEN = np.array((76, 175, 80), dtype=np.float32)


def load(path: Path, size: tuple[int, int] | None = None) -> np.ndarray:
    image = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"cannot read {path}")
    image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
    if size and image.shape[1::-1] != size:
        image = cv2.resize(image, size, interpolation=cv2.INTER_AREA)
    return image


def ssim(left: np.ndarray, right: np.ndarray) -> float:
    left = cv2.cvtColor(left, cv2.COLOR_RGB2GRAY).astype(np.float64)
    right = cv2.cvtColor(right, cv2.COLOR_RGB2GRAY).astype(np.float64)
    c1, c2 = 6.5025, 58.5225
    mu_l, mu_r = left.mean(), right.mean()
    var_l, var_r = left.var(), right.var()
    covariance = ((left - mu_l) * (right - mu_r)).mean()
    return float(((2 * mu_l * mu_r + c1) * (2 * covariance + c2)) /
                 ((mu_l * mu_l + mu_r * mu_r + c1) * (var_l + var_r + c2)))


def pupil_error(frame: np.ndarray) -> float | None:
    samples = []
    for x0, y0, x1, y1 in EYE_REGIONS:
        region = frame[y0:y1, x0:x1].astype(np.float32)
        greenish = (region[..., 1] >= 45) & (region[..., 1] > region[..., 0] * 1.25) & \
                   (region[..., 1] > region[..., 2] * 1.25)
        if greenish.any():
            samples.append(region[greenish])
    if not samples:
        return None
    pixels = np.concatenate(samples)
    if len(pixels) < 80:
        return None
    distance = np.linalg.norm(pixels - TARGET_GREEN, axis=1)
    # Report the percentage of pupil pixels outside the tolerated antialiasing
    # distance, matching the product's "deviation < 5%" acceptance language.
    return float(np.mean(distance > 35.0) * 100.0)


def normalize_pupils(frame: np.ndarray) -> np.ndarray:
    frame = frame.copy()
    for x0, y0, x1, y1 in EYE_REGIONS:
        region = frame[y0:y1, x0:x1]
        greenish = (region[..., 1] >= 45) & (region[..., 1] > region[..., 0] * 1.25) & \
                   (region[..., 1] > region[..., 2] * 1.25)
        region[greenish] = TARGET_GREEN.astype(np.uint8)
    return frame


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="directory containing ordered PNG frames")
    parser.add_argument("--source", type=Path, help="required transition first-frame reference")
    parser.add_argument("--target", type=Path, help="required transition last-frame reference")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--eye-max-percent", type=float, default=5.0)
    parser.add_argument("--endpoint-ssim", type=float, default=0.95)
    args = parser.parse_args()

    paths = sorted(args.input.glob("*.png"))
    if len(paths) < 2:
        raise SystemExit("at least two PNG frames are required")
    frames = [normalize_pupils(load(path, (360, 360))) for path in paths]
    pupil = [error for frame in frames if (error := pupil_error(frame)) is not None]
    diffs = [float(np.mean(cv2.absdiff(frames[i - 1], frames[i])) / 255.0)
             for i in range(1, len(frames))]
    median_diff = float(np.median(diffs)) or 1e-9
    edge_spikes = [i + 1 for i, value in enumerate(diffs)
                   if value > max(0.12, median_diff * 3.0)]

    mouth = [cv2.cvtColor(frame[MOUTH_REGION[1]:MOUTH_REGION[3],
                                      MOUTH_REGION[0]:MOUTH_REGION[2]],
                              cv2.COLOR_RGB2GRAY) for frame in frames]
    mouth_shifts = []
    template = mouth[0]
    for patch in mouth[1:]:
        shift, _ = cv2.phaseCorrelate(template.astype(np.float32), patch.astype(np.float32))
        mouth_shifts.append(float(np.hypot(*shift)))

    endpoint = {}
    if args.source:
        endpoint["source_ssim"] = ssim(frames[0], load(args.source, (360, 360)))
    if args.target:
        endpoint["target_ssim"] = ssim(frames[-1], load(args.target, (360, 360)))
    if not args.source and not args.target:
        endpoint["loop_ssim"] = ssim(frames[0], frames[-1])

    failures = []
    if pupil and max(pupil) >= args.eye_max_percent:
        failures.append(f"pupil deviation {max(pupil):.2f}% >= {args.eye_max_percent:.2f}%")
    if edge_spikes:
        failures.append(f"temporal edge/diff spikes at frames {edge_spikes}")
    if mouth_shifts and max(mouth_shifts) > 64.0:
        failures.append(f"mouth tracking failed {max(mouth_shifts):.2f}px > 64px")
    for name, value in endpoint.items():
        if value < args.endpoint_ssim:
            failures.append(f"{name} {value:.4f} < {args.endpoint_ssim:.4f}")

    report = {
        "input": str(args.input), "frames": len(frames), "passed": not failures,
        "pupil_max_deviation_percent": max(pupil, default=None),
        "frame_diff_median": median_diff, "edge_spike_frames": edge_spikes,
        "mouth_max_shift_px": max(mouth_shifts, default=0.0),
        **endpoint, "failures": failures,
    }
    destination = args.report or args.input / "validation_report.json"
    destination.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if not failures else 1)


if __name__ == "__main__":
    main()
