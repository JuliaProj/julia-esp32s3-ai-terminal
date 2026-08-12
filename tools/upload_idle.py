#!/usr/bin/env python3
"""Upload Julia idle TRN assets through the block/CRC maintenance protocol."""

from __future__ import annotations

import argparse
import binascii
import math
import struct
import time
from pathlib import Path

import serial

from upload_clips import SerialReceiver


def upload(port, receiver: SerialReceiver, path: Path) -> None:
    payload = path.read_bytes()
    crc = binascii.crc32(payload) & 0xFFFFFFFF
    port.reset_input_buffer()
    receiver.clear()
    port.write(f"idle upload {path.name} {len(payload)} {crc:08x}\n".encode())
    receiver.wait_regex(rb"IDLE_UPLOAD_READY[^\r\n]*\r?\n", 5, "ready")
    block_size = 4096
    blocks = math.ceil(len(payload) / block_size)
    time.sleep(1.0)
    port.write(struct.pack("<10sIHI", b"CLIP_BEGIN", len(payload), blocks, crc))
    port.flush()
    begin, groups, _ = receiver.wait_regex(rb"CLIP_BEGIN (OK|ERR)[^\r\n]*", 10, "begin")
    if groups[0] != b"OK":
        raise RuntimeError(begin.decode("utf-8", "replace"))
    started = time.time()
    for sequence in range(blocks):
        data = payload[sequence * block_size : (sequence + 1) * block_size]
        packet = struct.pack("<HH", sequence, len(data)) + data + struct.pack(
            "<I", binascii.crc32(data) & 0xFFFFFFFF
        )
        for attempt in range(3):
            port.write(packet)
            port.flush()
            response = receiver.wait_block(sequence, timeout=12)
            if response == "ACK":
                break
            print(f"{path.name}: NAK block={sequence} retry={attempt + 1}/3")
        else:
            raise RuntimeError(f"{path.name}: block {sequence} failed")
    result, _, _ = receiver.wait_regex(rb"IDLE_UPLOAD result=[^\r\n]+", 30, "result")
    line = result.decode("utf-8", "replace")
    print(f"{path.name}: {line} elapsed={time.time() - started:.1f}s crc={crc:08x}")
    if "result=OK" not in line:
        raise RuntimeError(line)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--directory", type=Path, default=Path("assets/transitions/idle"))
    parser.add_argument("--files", nargs="+", help="optional file names to upload")
    args = parser.parse_args()
    files = sorted(args.directory.glob("*.trn"))
    if args.files:
        wanted = set(args.files)
        files = [path for path in files if path.name in wanted]
    if not files:
        raise SystemExit(f"no .trn files in {args.directory}")
    with serial.Serial(args.port, 115200, timeout=0.2, write_timeout=15) as port:
        receiver = SerialReceiver(port)
        try:
            receiver.wait_regex(rb"Julia maintenance commands:[^\r\n]*", 5, "console ready")
        except TimeoutError:
            receiver.clear()
        time.sleep(1)
        for path in files:
            upload(port, receiver, path)


if __name__ == "__main__":
    main()
