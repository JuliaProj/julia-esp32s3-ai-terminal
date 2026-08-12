#!/usr/bin/env python3
"""Verify byte-order diagnostics and one synchronized Doze cycle."""
import argparse
import time
import serial


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--boot-wait", type=float, default=12.0)
    parser.add_argument("--doze-seconds", type=float, default=7.5)
    parser.add_argument("--wake-seconds", type=float, default=1.5)
    args = parser.parse_args()
    device = serial.Serial(args.port, 115200, timeout=0.1)
    device.dtr = False
    device.rts = False
    captured = bytearray()
    started = time.time()
    while time.time() - started < args.boot_wait:
        captured.extend(device.read(4096))
    device.write(b"demo off\nswap test\n")
    for _ in range(args.cycles):
        device.write(b"doze enter\n")
        deadline = time.time() + args.doze_seconds
        while time.time() < deadline:
            captured.extend(device.read(4096))
        device.write(b"doze status\ndoze exit\n")
        deadline = time.time() + args.wake_seconds
        while time.time() < deadline:
            captured.extend(device.read(4096))
    device.write(b"doze status\nstatus\n")
    deadline = time.time() + 1
    while time.time() < deadline:
        captured.extend(device.read(4096))
    device.close()
    keys = ("SWAP_TEST", "doze image", "doze enter", "doze exit", "breathe ",
            "DOZE active", "LCD flush failed", "Panic", "Guru", "STATUS ")
    text = captured.decode("utf-8", "replace")
    print(f"SUMMARY requested={args.cycles} entered={text.count('command=enter')} "
          f"exited={text.count('command=exit')} frame_sync_ok={text.count('frame_sync=ESP_OK')} "
          f"panic={text.count('Panic') + text.count('Guru Meditation') + text.count('stack overflow')} "
          f"lcd_fail={text.count('LCD flush failed')}")
    print("\n".join(line for line in text.splitlines() if any(key in line for key in keys)))


if __name__ == "__main__":
    main()
