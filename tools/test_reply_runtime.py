#!/usr/bin/env python3
"""Trigger offline reply playback repeatedly and print relevant device logs."""

import argparse
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--count", type=int, default=5)
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        port.dtr = False
        port.rts = True
        time.sleep(0.1)
        port.rts = False
        deadline = time.time() + 8
        data = bytearray()
        while time.time() < deadline:
            data.extend(port.read(4096))
        for _ in range(args.count):
            port.write(b"reply-test\r\n")
            port.flush()
            deadline = time.time() + 3
            while time.time() < deadline:
                data.extend(port.read(4096))
    text = data.decode("utf-8", "replace")
    pattern = re.compile(r"^.*(?:WAKE_REPLY|REPLY_TEST|PCM complete|PCM incomplete|"
                         r"Guru Meditation|LCD flush failed|rst:).*$",
                         re.MULTILINE)
    print("\n".join(pattern.findall(text)))


if __name__ == "__main__":
    main()
