"""Exercise avatar motion priority and state coexistence on the serial console."""
import argparse
import time
from pathlib import Path

import serial


def drain(device: serial.Serial, duration: float, output: bytearray) -> None:
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        output.extend(device.read(4096))


def command(device: serial.Serial, text: str) -> None:
    device.write((text + "\n").encode("ascii"))
    device.flush()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--output", default="task7_motion_states.log")
    args = parser.parse_args()
    received = bytearray()
    with serial.Serial(args.port, 115200, timeout=0.05) as device:
        device.dtr = False
        drain(device, 6, received)
        command(device, "state 3")
        command(device, "phase 0")
        drain(device, 8, received)
        command(device, "phase 1")
        drain(device, 10, received)
        command(device, "phase 3")
        command(device, "mouth 0")
        drain(device, 1, received)
        command(device, "mouth 1")
        drain(device, 1, received)
        command(device, "mouth 3")
        drain(device, 4, received)
        command(device, "phase 0")
        drain(device, 6, received)
    text = received.decode("utf-8", "replace")
    Path(args.output).write_text(text, encoding="utf-8")
    print(f"MOTION_STATES output={args.output} bytes={len(received)}")


if __name__ == "__main__":
    main()
