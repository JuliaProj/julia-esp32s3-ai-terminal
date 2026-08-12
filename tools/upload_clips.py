#!/usr/bin/env python3
"""通过 Julia 维护串口将 clip 原子上传到 SD。"""

import argparse
import binascii
import json
import math
import re
import struct
import time
from pathlib import Path
import serial


class SerialReceiver:
    """串口持久接收缓冲；匹配只消费到 match.end()，尾部留给下一次匹配。"""

    def __init__(self, port):
        self.port = port
        self.buffer = bytearray()

    def clear(self):
        self.buffer.clear()

    def wait_regex(self, pattern, timeout, label):
        if isinstance(pattern, bytes):
            pattern = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            match = pattern.search(self.buffer)
            if match:
                matched = bytes(match.group(0))
                groups = tuple(bytes(value) if value is not None else None
                               for value in match.groups())
                consumed = bytes(self.buffer[:match.end()])
                del self.buffer[:match.end()]
                return matched, groups, consumed
            self.buffer.extend(self.port.read(4096))
        raise TimeoutError(f"{label}: buffered={bytes(self.buffer[-500:])!r}")

    def wait_block(self, sequence, timeout=10):
        pattern = re.compile(rb"(?:^|\r?\n)(ACK|NAK) " +
                             str(sequence).encode() + rb"\r?\n")
        _, groups, _ = self.wait_regex(pattern, timeout, f"block {sequence}")
        return groups[0].decode()


class FakePort:
    def __init__(self, chunks):
        self.chunks = list(chunks)

    def read(self, _size):
        return self.chunks.pop(0) if self.chunks else b""


def run_self_test():
    packet = b"noise\r\nACK 82\r\nCLIP_UPLOAD result=OK idle_loop 339330 error=ESP_OK\r\n"
    receiver = SerialReceiver(FakePort([packet]))
    assert receiver.wait_block(82) == "ACK"
    matched, _, _ = receiver.wait_regex(rb"CLIP_UPLOAD result=[^\r\n]+", 1, "result")
    result = matched.decode()
    assert result == "CLIP_UPLOAD result=OK idle_loop 339330 error=ESP_OK"
    print("SELF_TEST PASS: coalesced ACK+result preserved")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--directory", type=Path, default=Path("main/ui/generated/loops"))
    ap.add_argument("--handshake-only", action="store_true")
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--names", nargs="+", help="clip ids; defaults to all manifests")
    args = ap.parse_args()
    if args.self_test:
        run_self_test()
        return
    names = args.names or sorted(path.stem for path in args.directory.glob("*.jsn"))
    with serial.Serial(args.port, 115200, timeout=.2, write_timeout=10) as port:
        receiver = SerialReceiver(port)
        try:
            receiver.wait_regex(rb"Julia maintenance commands:[^\r\n]*", 5, "console ready")
        except TimeoutError:
            # USB Serial/JTAG can preserve an already-running session, so the boot
            # banner is not guaranteed to appear when the uploader opens the port.
            receiver.clear()
        port.write(b"demo off\r\n")
        port.flush()
        time.sleep(3)
        for name in names:
            clip = args.directory / f"{name}.clip"
            manifest = json.loads((args.directory / f"{name}.jsn").read_text(encoding="utf-8"))
            payload = clip.read_bytes()
            crc = manifest["file_crc32"]
            port.reset_input_buffer()
            receiver.clear()
            command = "clip-upload-debug" if args.debug else "clip-upload"
            port.write(f"{command} {name}\r\n".encode())
            _, _, ready = receiver.wait_regex(rb"CLIP_UPLOAD_READY[^\r\n]*\r?\n", 5, "ready")
            started = time.time()
            block_size = 4096
            block_count = math.ceil(len(payload) / block_size)
            expected_crc = int(crc, 16)
            begin_frame = struct.pack("<10sIHI", b"CLIP_BEGIN", len(payload),
                                      block_count, expected_crc)
            if args.debug:
                print(f"{name}: READY_RAW={ready.decode('utf-8', 'replace').rstrip()}")
                print(f"{name}: CLIP_BEGIN_HEX={begin_frame.hex()} bytes={len(begin_frame)}")
            port.flush()
            time.sleep(.2)
            port.write(begin_frame); port.flush()
            if args.handshake_only:
                deadline, response = time.time() + 12, bytearray()
                while time.time() < deadline:
                    response.extend(port.read(4096))
                print(f"{name}: DEVICE_200MS_AND_LATER_RAW=\n{response.decode('utf-8', 'replace')}")
                return
            begin_match, begin_groups, _ = receiver.wait_regex(
                rb"CLIP_BEGIN (OK|ERR)[^\r\n]*", 10, "clip begin")
            if begin_groups[0] != b"OK":
                raise RuntimeError(begin_match.decode("utf-8", "replace"))
            overall_timeout = max(30.0, len(payload) * 10.0 / 115200.0 * 3.0)
            overall_deadline = time.time() + overall_timeout
            next_progress = 10
            for sequence in range(block_count):
                offset = sequence * block_size
                data = payload[offset:offset + block_size]
                packet = struct.pack("<HH", sequence, len(data)) + data + \
                         struct.pack("<I", binascii.crc32(data) & 0xffffffff)
                for attempt in range(1, 4):
                    if time.time() >= overall_deadline:
                        raise TimeoutError(f"{name}: overall timeout at block {sequence}")
                    port.write(packet); port.flush()
                    response = receiver.wait_block(sequence)
                    if response == "ACK": break
                    print(f"{name}: NAK block={sequence} retry={attempt}/3")
                else:
                    raise RuntimeError(f"{name}: block {sequence} failed after 3 attempts")
                progress = (sequence + 1) * 100 // block_count
                if progress >= next_progress:
                    print(f"{name}: {next_progress}% ({sequence + 1}/{block_count})")
                    next_progress += 10
            result_match, _, _ = receiver.wait_regex(rb"CLIP_UPLOAD result=[^\r\n]+", 30,
                                                      "upload result")
            line = result_match.decode("utf-8", "replace")
            print(f"{name}: bytes={len(payload)} crc={crc} elapsed={time.time()-started:.1f}s {line}")
            if "result=OK" not in line:
                raise SystemExit(1)


if __name__ == "__main__":
    main()
