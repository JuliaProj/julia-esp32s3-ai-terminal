#!/usr/bin/env python3
"""Exercise automatic Doze and report serial stability evidence."""
import argparse
import re
import time
import serial


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--seconds", type=int, default=645)
    args = parser.parse_args()
    device = serial.Serial(args.port, 115200, timeout=0.1)
    device.dtr = False
    device.rts = False
    started = time.time()
    captured = bytearray()
    sent = set()
    while time.time() - started < args.seconds:
        elapsed = time.time() - started
        captured.extend(device.read(4096))
        if elapsed > 7 and "start" not in sent:
            device.write(b"demo off\nstatus\n")
            sent.add("start")
        if elapsed > 45 and "doze" not in sent:
            device.write(b"doze status\nstatus\n")
            sent.add("doze")
        if elapsed > args.seconds - 10 and "final" not in sent:
            device.write(b"status\ndoze status\n")
            sent.add("final")
    device.close()
    keys = ("STATUS ", "doze enter", "DOZE active", "Panic",
            "panic", "LCD flush failed", "Guru Meditation", "BACKLIGHT: breathe")
    text = captured.decode("utf-8", "replace")
    doze_at = text.find("doze enter")
    tail = text[doze_at:] if doze_at >= 0 else ""
    flushes = [int(value) for value in re.findall(r"lvgl_flush=(\d+)", tail)]
    print(f"SUMMARY auto_enter={int(doze_at >= 0)} doze_flush_samples={len(flushes)} "
          f"steady_nonzero_flush={sum(value != 0 for value in flushes[1:])} "
          f"panic={text.count('Panic') + text.count('Guru Meditation') + text.count('stack overflow')} "
          f"lcd_fail={text.count('LCD flush failed')}")
    print("\n".join(line for line in text.splitlines() if any(key in line for key in keys)))


if __name__ == "__main__":
    main()
