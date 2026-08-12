"""Capture one uninterrupted idle-motion acceptance window from COM5."""
import argparse
import time
from pathlib import Path

import serial


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--seconds", type=int, default=190)
    parser.add_argument("--all", action="store_true", help="print every serial line")
    parser.add_argument("--output", help="optional UTF-8 path for the complete capture")
    args = parser.parse_args()
    keys = ("AVATAR_EVENT", "Panic", "rst:", "underrun", "CRC", "hit", "PSRAM", "min_heap", "STATUS")
    lines: list[str] = []
    buffer = ""
    started = time.time()
    sent = False
    with serial.Serial(args.port, 115200, timeout=0.15) as device:
        device.dtr = False
        while time.time() - started < args.seconds:
            buffer += device.read(4096).decode("utf-8", "replace")
            if not sent and time.time() - started > 6:
                device.write(b"state 3\nphase 0\n")
                device.flush()
                sent = True
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                if args.all or any(key in line for key in keys):
                    lines.append(line.strip())
    result = "\n".join(lines) + "\n"
    if args.output:
        Path(args.output).write_text(result, encoding="utf-8")
        print(f"CAPTURE output={args.output} lines={len(lines)}")
    else:
        print(result, end="")


if __name__ == "__main__":
    main()
