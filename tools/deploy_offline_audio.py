#!/usr/bin/env python3
"""Upload generated offline PCM resources to Julia's SD card over USB serial."""
import argparse
import binascii
import math
import re
import struct
import time
from pathlib import Path
import serial


class Receiver:
    def __init__(self, port):
        self.port = port
        self.data = bytearray()

    def clear(self) -> None:
        self.data.clear()

    def wait(self, pattern: bytes, timeout: float) -> bytes:
        deadline = time.time() + timeout
        regex = re.compile(pattern)
        while time.time() < deadline:
            match = regex.search(self.data)
            if match:
                result = bytes(match.group(0))
                del self.data[:match.end()]
                return result
            self.data.extend(self.port.read(4096))
        raise TimeoutError(bytes(self.data[-400:]))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--directory", type=Path, default=Path("assets/offline_audio"))
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=.2, write_timeout=10) as port:
        port.dtr = False; port.rts = False
        time.sleep(4)
        receiver = Receiver(port)
        for path in sorted(args.directory.glob("*.pcm")):
            payload = path.read_bytes(); crc = binascii.crc32(payload) & 0xffffffff
            port.reset_input_buffer()
            receiver.clear()
            port.write(f"audio-upload {path.stem} {len(payload)} {crc:08x}\r\n".encode())
            receiver.wait(rb"AUDIO_UPLOAD_READY[^\r\n]*", 5)
            blocks = math.ceil(len(payload) / 4096)
            port.write(struct.pack("<10sIHI", b"CLIP_BEGIN", len(payload), blocks, crc))
            receiver.wait(rb"CLIP_BEGIN OK[^\r\n]*", 10)
            for sequence in range(blocks):
                chunk = payload[sequence * 4096:(sequence + 1) * 4096]
                packet = struct.pack("<HH", sequence, len(chunk)) + chunk + \
                         struct.pack("<I", binascii.crc32(chunk) & 0xffffffff)
                port.write(packet); port.flush()
                response = receiver.wait(rb"(?:ACK|NAK) " + str(sequence).encode() + rb"\r?\n", 10)
                if b"NAK" in response: raise RuntimeError(response)
            result = receiver.wait(rb"AUDIO_UPLOAD result=[^\r\n]+", 20)
            print(result.decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
