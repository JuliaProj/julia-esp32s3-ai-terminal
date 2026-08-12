#!/usr/bin/env python3
"""Run serial mock-ASR acceptance cases and save the complete device log."""
import argparse
import time
from pathlib import Path
import serial


def collect(port, seconds: float) -> bytes:
    deadline = time.time() + seconds
    output = bytearray()
    while time.time() < deadline:
        output.extend(port.read(4096))
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--case", choices=("hello", "interrupt", "reject", "leak_short"), default="hello")
    parser.add_argument("--output", type=Path, default=Path("mock_voice_acceptance.log"))
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=.2) as port:
        port.dtr = False; port.rts = False
        collect(port, 4)
        port.reset_input_buffer()
        if args.case == "hello":
            port.write('asr "你好"\r\n'.encode())
            data = collect(port, 42)
        elif args.case == "interrupt":
            port.write('asr "你好"\r\n'.encode())
            data = collect(port, .5)
            port.write('asr "天气怎么样"\r\n'.encode())
            data += collect(port, 8)
        elif args.case == "reject":
            port.write('asr "想静静"\r\n'.encode())
            data = collect(port, 8)
        else:
            port.write(b"test leak start\r\n")
            data = collect(port, 13)
            port.write(b"test leak stop\r\n")
            data += collect(port, 5)
    args.output.write_bytes(data)
    print(data.decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
