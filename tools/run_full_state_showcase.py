#!/usr/bin/env python3
"""Drive the Julia device through every transition and substate for filming."""

from __future__ import annotations

import argparse
import time

import serial


TRANSITIONS = (
    ("S0", "S1", "wake up"),
    ("S1", "S0", "fall asleep"),
    ("S1", "S2", "wake to companion"),
    ("S2", "S1", "companion to standby"),
    ("S1", "S3", "standby to approach"),
    ("S2", "S3", "companion to approach"),
    ("S3", "S4", "approach to dialog"),
    ("S4", "S1", "dialog to standby"),
    ("S4", "S5", "dialog to silence"),
    ("S5", "S1", "silence to standby"),
    ("S5", "S2", "silence to companion"),
    ("S5", "S4", "silence to dialog"),
)

SUBSTATES = (
    ("S0.1", "night sleep"),
    ("S0.2", "day away"),
    ("S0.3", "manual sleep"),
    ("S1.1", "near standby"),
    ("S1.2", "far standby"),
    ("S1.3", "charging standby"),
    ("S2.1", "observe / read"),
    ("S2.2", "shared activity"),
    ("S2.3", "bedtime companion"),
    ("S3.1", "emotion trigger"),
    ("S3.2", "routine break"),
    ("S3.3", "user call"),
    ("S3.4", "recovery probe"),
    ("S4.1", "light dialog"),
    ("S4.2", "deep talk with RMS mouth"),
    ("S4.3", "multi-turn dialog"),
    ("S4.4", "interrupted dialog"),
    ("S5.1", "user rejected"),
    ("S5.2", "perfunctory response"),
    ("S5.3", "user left (reuses S5.2 art)"),
)


def send(device: serial.Serial, command: str) -> None:
    device.write((command + "\n").encode("ascii"))
    device.flush()


def drain(device: serial.Serial) -> None:
    data = device.read(device.in_waiting or 1)
    if data:
        print(data.decode("utf-8", "replace"), end="")


def hold(device: serial.Serial, seconds: float) -> None:
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        drain(device)
        time.sleep(0.1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--transition-hold", type=float, default=4.0)
    parser.add_argument("--state-hold", type=float, default=7.0)
    parser.add_argument("--lead-in", type=float, default=5.0)
    parser.add_argument("--boot-settle", type=float, default=15.0,
                        help="wait for the USB-UART reset and SD scan before filming")
    args = parser.parse_args()

    with serial.Serial(args.port, 115200, timeout=0.1) as device:
        device.dtr = False
        device.rts = False
        print(f"SHOWCASE boot settle {args.boot_settle:.0f}s (do not film yet)")
        hold(device, args.boot_settle)
        print(f"SHOWCASE lead-in {args.lead_in:.0f}s - start filming now")
        hold(device, args.lead_in)
        send(device, "demo off")
        send(device, "doze exit")

        print("SHOWCASE section 1/2: 12 directed transitions")
        for index, (source, target, label) in enumerate(TRANSITIONS, 1):
            print(f"TRANSITION {index:02d}/12 {source}->{target}: {label}")
            send(device, f"transition play {source} {target}")
            hold(device, args.transition_hold)

        print("SHOWCASE section 2/2: all 20 enum substates")
        for index, (state, label) in enumerate(SUBSTATES, 1):
            print(f"STATE {index:02d}/20 {state}: {label}")
            send(device, f"state set {state}")
            hold(device, args.state_hold)
            if state == "S4.2":
                send(device, "speak 100")
                hold(device, 1.5)
                send(device, "speak 0")

        print("BONUS S2.1: cached read/tea switch")
        send(device, "state set S2.1")
        hold(device, args.state_hold)
        send(device, "idle switch")
        hold(device, args.state_hold)

        print("SHOWCASE complete - restoring S1.1 standby")
        send(device, "speak 0")
        send(device, "state set S1.1")
        hold(device, 3.0)
        send(device, "state get")
        send(device, "idle cache")
        hold(device, 1.0)


if __name__ == "__main__":
    main()
