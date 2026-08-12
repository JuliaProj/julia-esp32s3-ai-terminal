#!/usr/bin/env python3
"""Validate Julia RGB565 C assets and JTRN transition files."""

from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FRAME_WIDTH = 360
FRAME_HEIGHT = 360
FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 2
TRN_HEADER = struct.Struct("<4sBHHHBBII11s")
GREEN_LIMIT = 0.05


@dataclass
class Finding:
    path: Path
    item: str
    reason: str


def swap_enabled(sdkconfig: Path) -> bool:
    text = sdkconfig.read_text(encoding="utf-8", errors="replace")
    if "CONFIG_LV_COLOR_16_SWAP=y" in text:
        return True
    if "# CONFIG_LV_COLOR_16_SWAP is not set" in text:
        return False
    raise ValueError(f"CONFIG_LV_COLOR_16_SWAP missing from {sdkconfig}")


def pixel_stats(payload: bytes, swap: bool) -> tuple[float, float, int]:
    if len(payload) % 2:
        raise ValueError("odd RGB565 payload length")
    green = exact_green = key = 0
    for offset in range(0, len(payload), 2):
        value = ((payload[offset] << 8) | payload[offset + 1]) if swap else (
            payload[offset] | (payload[offset + 1] << 8)
        )
        red, channel_green, blue = value >> 11, (value >> 5) & 0x3F, value & 0x1F
        green_5bit = channel_green >> 1
        if green_5bit >= 24 and green_5bit > red * 2 and green_5bit > blue * 2:
            green += 1
        if value == 0x07E0:
            exact_green += 1
        if value == 0xF81F:
            key += 1
    pixels = len(payload) // 2
    return green / pixels, exact_green / pixels, key


def decode_rle(payload: bytes, expected_pixels: int) -> bytes:
    output = bytearray()
    cursor = 0
    while cursor < len(payload):
        if cursor + 2 > len(payload):
            raise ValueError("truncated RLE control")
        control = struct.unpack_from("<H", payload, cursor)[0]
        cursor += 2
        count = (control & 0x7FFF) + 1
        if control & 0x8000:
            if cursor + 2 > len(payload):
                raise ValueError("truncated RLE run")
            pixel = payload[cursor:cursor + 2]
            cursor += 2
            output.extend(pixel * count)
        else:
            size = count * 2
            if cursor + size > len(payload):
                raise ValueError("truncated RLE literal")
            output.extend(payload[cursor:cursor + size])
            cursor += size
        if len(output) > expected_pixels * 2:
            raise ValueError("RLE output overflow")
    if len(output) != expected_pixels * 2:
        raise ValueError(f"decoded bytes={len(output)}, expected={expected_pixels * 2}")
    return bytes(output)


def validate_trn(path: Path, swap: bool, findings: list[Finding]) -> tuple[int, int]:
    data = path.read_bytes()
    if len(data) < TRN_HEADER.size:
        findings.append(Finding(path, "header", "truncated"))
        return 0, 0
    magic, version, width, height, count, fps, fmt, frame_bytes, total, _ = TRN_HEADER.unpack_from(data)
    if magic != b"JTRN" or version != 1:
        findings.append(Finding(path, "header", f"magic/version={magic!r}/{version}"))
        return 0, 0
    if (width, height) != (FRAME_WIDTH, FRAME_HEIGHT):
        findings.append(Finding(path, "header", f"resolution={width}x{height}, expected=360x360"))
    if frame_bytes != width * height * 2:
        findings.append(Finding(path, "header", f"bytes_per_frame={frame_bytes}, expected={width * height * 2}"))
    if total != len(data):
        findings.append(Finding(path, "header", f"declared_size={total}, actual={len(data)}"))
    if not count or not fps or fmt not in (0, 1):
        findings.append(Finding(path, "header", f"frames/fps/format={count}/{fps}/{fmt}"))
        return 0, 0
    table_end = TRN_HEADER.size + count * 4
    if table_end > len(data):
        findings.append(Finding(path, "offsets", "truncated"))
        return 0, 0
    offsets = struct.unpack_from(f"<{count}I", data, TRN_HEADER.size)
    frame_data = data[table_end:]
    bad = 0
    for index, offset in enumerate(offsets):
        end = offsets[index + 1] if index + 1 < count else len(frame_data)
        try:
            if offset > end or end > len(frame_data):
                raise ValueError(f"invalid offset range {offset}:{end}")
            packed = frame_data[offset:end]
            payload = decode_rle(packed, width * height) if fmt == 1 else packed
            if len(payload) != frame_bytes:
                raise ValueError(f"frame bytes={len(payload)}, expected={frame_bytes}")
            green_ratio, exact_ratio, _ = pixel_stats(payload, swap)
            if green_ratio > GREEN_LIMIT or exact_ratio > GREEN_LIMIT:
                bad += 1
                findings.append(Finding(
                    path, f"frame[{index}]",
                    f"green={green_ratio:.2%} exact_0x07E0={exact_ratio:.2%}",
                ))
        except ValueError as error:
            bad += 1
            findings.append(Finding(path, f"frame[{index}]", str(error)))
    return count, bad


ARRAY_RE = re.compile(
    r"static const uint8_t\s+(\w+)_map\[\]\s*=\s*\{(.*?)\};\s*"
    r"const lv_img_dsc_t\s+\w+\s*=\s*\{(.*?)\};",
    re.S,
)


def validate_c_assets(path: Path, swap: bool, findings: list[Finding]) -> int:
    text = path.read_text(encoding="ascii", errors="replace")
    count = 0
    for match in ARRAY_RE.finditer(text):
        count += 1
        name, array, descriptor = match.groups()
        payload = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", array))
        width_match = re.search(r"\.header\.w\s*=\s*(\d+)", descriptor)
        height_match = re.search(r"\.header\.h\s*=\s*(\d+)", descriptor)
        chroma = "LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED" in descriptor
        if not width_match or not height_match:
            findings.append(Finding(path, name, "descriptor dimensions missing"))
            continue
        width, height = int(width_match.group(1)), int(height_match.group(1))
        expected = width * height * 2
        if len(payload) != expected:
            findings.append(Finding(path, name, f"bytes={len(payload)}, expected={expected}"))
            continue
        if not chroma and (width, height) != (FRAME_WIDTH, FRAME_HEIGHT):
            findings.append(Finding(path, name, f"opaque base resolution={width}x{height}"))
        green_ratio, exact_ratio, key_count = pixel_stats(payload, swap)
        if green_ratio > GREEN_LIMIT or exact_ratio > GREEN_LIMIT:
            findings.append(Finding(
                path, name, f"green={green_ratio:.2%} exact_0x07E0={exact_ratio:.2%}",
            ))
        if chroma and key_count == 0:
            findings.append(Finding(path, name, "chroma-keyed asset contains no RGB565 0xF81F pixels"))
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--sdkconfig", type=Path, default=ROOT / "sdkconfig")
    args = parser.parse_args()
    paths = args.paths or [ROOT / "main" / "ui" / "generated", ROOT / "assets" / "transitions"]
    swap = swap_enabled(args.sdkconfig)
    files: list[Path] = []
    for candidate in paths:
        if candidate.is_dir():
            files.extend(candidate.rglob("*.trn"))
            files.extend(candidate.rglob("*.c"))
        elif candidate.suffix.lower() in {".trn", ".c"}:
            files.append(candidate)
    findings: list[Finding] = []
    trn_files = trn_frames = c_assets = 0
    for path in sorted(set(file.resolve() for file in files)):
        if path.suffix.lower() == ".trn":
            trn_files += 1
            frames, _ = validate_trn(path, swap, findings)
            trn_frames += frames
        else:
            c_assets += validate_c_assets(path, swap, findings)
    print(f"DISPLAY_ASSET_AUDIT byte_order={'high-first' if swap else 'low-first'} "
          f"c_assets={c_assets} trn_files={trn_files} trn_frames={trn_frames} anomalies={len(findings)}")
    for finding in findings:
        try:
            display_path = finding.path.relative_to(ROOT)
        except ValueError:
            display_path = finding.path
        print(f"ANOMALY {display_path}:{finding.item}: {finding.reason}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
