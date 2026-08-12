#!/usr/bin/env python3
"""Safely prepare or flash Julia transitions to a dedicated data partition."""

from __future__ import annotations

import argparse
import binascii
import csv
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK_HEADER = struct.Struct("<4sHHII")
ENTRY_HEADER = struct.Struct("<HHII")


def integer(value: str) -> int:
    return int(value, 0)


def find_partition(path: Path, label: str) -> tuple[int, int]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.reader(line for line in stream if not line.lstrip().startswith("#")):
            if len(row) >= 5 and row[0].strip() == label:
                return integer(row[3].strip()), integer(row[4].strip())
    raise SystemExit(
        f"REFUSED: dedicated partition '{label}' is absent from {path}. "
        "Current OTA/storage partitions will not be overwritten."
    )


def collect(source: Path) -> list[tuple[str, bytes]]:
    files = []
    for path in sorted(source.rglob("*.trn")):
        name = path.relative_to(source).as_posix()
        encoded = name.encode("utf-8")
        if len(encoded) > 255:
            raise SystemExit(f"path too long: {name}")
        files.append((name, path.read_bytes()))
    if not files:
        raise SystemExit(f"no .trn files found under {source}")
    return files


def build_pack(files: list[tuple[str, bytes]], capacity: int) -> bytes:
    body = bytearray()
    for name, data in files:
        encoded = name.encode("utf-8")
        body += ENTRY_HEADER.pack(len(encoded), 0, len(data), binascii.crc32(data) & 0xFFFFFFFF)
        body += encoded
        body += data
    image = PACK_HEADER.pack(b"JTPK", 1, len(files), len(body), binascii.crc32(body) & 0xFFFFFFFF) + body
    if len(image) > capacity:
        raise SystemExit(f"transition pack is {len(image)} bytes; partition capacity is {capacity}")
    return image + b"\xff" * (capacity - len(image))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=ROOT / "assets/transitions")
    parser.add_argument("--partition-table", type=Path, default=ROOT / "partitions.csv")
    parser.add_argument("--partition-label", default="transitions")
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--output", type=Path, default=ROOT / "build/transitions-pack.bin")
    parser.add_argument("--flash", action="store_true", help="write the pack with esptool")
    args = parser.parse_args()

    offset, capacity = find_partition(args.partition_table, args.partition_label)
    files = collect(args.input)
    image = build_pack(files, capacity)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"TRANSITION_PACK files={len(files)} bytes={len(image)} offset=0x{offset:x} output={args.output}")
    if not args.flash:
        return
    esptool = shutil.which("esptool.py") or shutil.which("esptool")
    command = ([esptool] if esptool else [sys.executable, "-m", "esptool"])
    command += ["--chip", "esp32s3", "--port", args.port, "write_flash", hex(offset), str(args.output)]
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
