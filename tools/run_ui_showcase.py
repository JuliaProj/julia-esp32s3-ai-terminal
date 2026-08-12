#!/usr/bin/env python3
"""Drive the stable Julia firmware through its registered UI transitions."""

import argparse
import time

import serial


SEQUENCE = (3, 6, 3, 9, 13, 17, 3, 0, 3, 6, 9, 13, 17, 6, 9, 13, 17, 13, 3)
LOG_MARKERS = (
    "TRANSITION:",
    "JULIA_ANIM:",
    "AVATAR_ANIM:",
    "State:",
    "load failed",
    "CRC",
    "invalid",
    "Panic",
    "panic",
    "LCD flush failed",
    "UI_SHOWCASE",
    "SHOWCASE",
)


def read_for(port: serial.Serial, seconds: float) -> bytes:
    deadline = time.time() + seconds
    output = bytearray()
    while time.time() < deadline:
        output.extend(port.read(4096))
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--hold", type=float, default=2.2)
    parser.add_argument("--startup-wait", type=float, default=8.0)
    parser.add_argument("--firmware", action="store_true",
                        help="run the firmware showcase used by the physical power key")
    args = parser.parse_args()

    output = bytearray()
    with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=5) as port:
        port.dtr = False
        port.rts = False
        time.sleep(1)
        port.write(b"demo off\r\n")
        port.flush()
        output.extend(read_for(port, args.startup_wait))
        if args.firmware:
            port.write(b"showcase start\r\n")
            port.flush()
            # The firmware first serially validates/preloads all showcase assets.
            output.extend(read_for(port, len(SEQUENCE) * 2.2 + 15.0))
        else:
            for state in SEQUENCE:
                port.write(f"state {state}\r\n".encode("ascii"))
                port.flush()
                output.extend(read_for(port, args.hold))

    for line in output.decode("utf-8", "replace").splitlines():
        if any(marker in line for marker in LOG_MARKERS):
            print(line)


if __name__ == "__main__":
    main()
