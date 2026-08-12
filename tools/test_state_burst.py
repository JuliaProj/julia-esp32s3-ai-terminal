#!/usr/bin/env python3
"""向维护串口快速投递状态请求，并采集异步状态机日志。"""

import argparse
import time
from pathlib import Path

import serial


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--count", type=int, default=50)
    parser.add_argument("--observe-seconds", type=float, default=12.0)
    parser.add_argument("--interval-ms", type=float, default=20.0)
    parser.add_argument("--startup-seconds", type=float, default=1.0)
    parser.add_argument("--output")
    args = parser.parse_args()

    port = serial.Serial(args.port, 115200, timeout=0.01, write_timeout=None)
    port.dtr = False
    port.rts = False
    time.sleep(args.startup_seconds)
    port.reset_input_buffer()

    started = time.monotonic()
    received = bytearray()
    sent_count = 0
    for index in range(args.count):
        try:
            port.write(f"state {index % 20}\n".encode("ascii"))
        except serial.SerialTimeoutException:
            print(f"WRITE_TIMEOUT index={index} sent={sent_count}")
            raise
        sent_count += 1
        received.extend(port.read(4096))
        time.sleep(args.interval_ms / 1000.0)
    port.flush()
    send_seconds = time.monotonic() - started

    deadline = time.monotonic() + args.observe_seconds
    while time.monotonic() < deadline:
        received.extend(port.read(4096))
    port.close()

    text = received.decode("utf-8", errors="replace")
    acknowledgements = text.count("State: ")
    queue_full = text.count("state request queue full")
    print(
        f"STATE_BURST sent={args.count} send_ms={send_seconds * 1000:.2f} "
        f"acks={acknowledgements} queue_full={queue_full} bytes={len(received)}"
    )
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
        print(f"STATE_BURST_LOG output={args.output}")
    else:
        print(text)


if __name__ == "__main__":
    main()
