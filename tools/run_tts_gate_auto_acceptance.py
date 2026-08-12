#!/usr/bin/env python3
"""Validate delayed mock TTS and collect the automatic 10-minute leak report."""
import argparse
import time
from pathlib import Path

import serial


def collect(port: serial.Serial, seconds: float) -> bytes:
    deadline = time.time() + seconds
    output = bytearray()
    while time.time() < deadline:
        output.extend(port.read(4096))
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--seconds", type=int, default=630)
    parser.add_argument("--output", type=Path, default=Path("tts_gate_auto_leak_10min.log"))
    args = parser.parse_args()

    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        port.dtr = False
        port.rts = False
        data = collect(port, 4)
        port.write('asr "你好"\r\n'.encode("utf-8"))
        data += collect(port, args.seconds)
    args.output.write_bytes(data)
    print(data[-20000:].decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
