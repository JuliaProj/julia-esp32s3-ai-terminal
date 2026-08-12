#!/usr/bin/env python3
"""Upload the generated offline reply library to Julia's mounted SD card."""

import argparse
import binascii
import math
import re
import struct
import time
from pathlib import Path

import serial

RX_BUFFER = bytearray()

def wait_line(port, pattern, timeout):
    deadline = time.time() + timeout
    regex = re.compile(pattern)
    while time.time() < deadline:
        match = regex.search(RX_BUFFER)
        if match:
            value = bytes(match.group(0)).decode("utf-8", "replace")
            del RX_BUFFER[:match.end()]
            return value
        RX_BUFFER.extend(port.read(4096))
    raise TimeoutError(bytes(RX_BUFFER[-400:]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--directory", type=Path,
                        default=Path("main/audio/generated/pcm"))
    parser.add_argument("--names", nargs="*", help="optional PCM filenames")
    args = parser.parse_args()
    files = sorted(args.directory.glob("reply_*.pcm"))
    if args.names:
        requested = set(args.names)
        files = [path for path in files if path.name in requested]
    if not args.names and len(files) < 20:
        raise SystemExit(f"expected at least 20 PCM files, found {len(files)}")
    if not files:
        raise SystemExit("no matching PCM files")
    with serial.Serial(args.port, 115200, timeout=0.2, write_timeout=15) as port:
        for number, path in enumerate(files, 1):
            payload = path.read_bytes()
            crc = binascii.crc32(payload) & 0xFFFFFFFF
            port.reset_input_buffer()
            RX_BUFFER.clear()
            command = f"reply-upload {path.name} {len(payload)} {crc:08x}\r\n"
            port.write(command.encode("ascii")); port.flush()
            wait_line(port, rb"REPLY_UPLOAD_READY[^\r\n]*", 5)
            block_size = 4096
            block_count = math.ceil(len(payload) / block_size)
            begin = struct.pack("<10sIHI", b"CLIP_BEGIN", len(payload), block_count, crc)
            time.sleep(0.2)
            port.write(begin); port.flush()
            begin_result = wait_line(port, rb"CLIP_BEGIN (?:OK|ERR)[^\r\n]*", 10)
            if " OK" not in begin_result:
                raise RuntimeError(begin_result)
            for sequence in range(block_count):
                data = payload[sequence * block_size:(sequence + 1) * block_size]
                packet = (struct.pack("<HH", sequence, len(data)) + data +
                          struct.pack("<I", binascii.crc32(data) & 0xFFFFFFFF))
                for _ in range(3):
                    port.write(packet); port.flush()
                    ack = wait_line(port, rb"(?:ACK|NAK) " + str(sequence).encode() + rb"\r?\n", 10)
                    if "ACK" in ack:
                        break
                else:
                    raise RuntimeError(f"block {sequence} failed")
            result = wait_line(port, rb"REPLY_UPLOAD file=[^\r\n]+", 20)
            if "result=ESP_OK" not in result:
                raise RuntimeError(result)
            print(f"[{number:02d}/{len(files)}] {result}")


if __name__ == "__main__":
    main()
