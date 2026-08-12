#!/usr/bin/env python3
"""将 PNG 序列打包为 Julia RGB565-RLE clip 容器。"""

import argparse
import binascii
import json
import re
import struct
from pathlib import Path
from PIL import Image

MAGIC = b"JCLP"
HEADER = struct.Struct("<4sHHHBBHHIIII")  # 32 bytes
MODE = {"forward-loop": 0, "ping-pong": 1, "once-hold": 2}


def natural(path):
    return [int(x) if x.isdigit() else x.lower() for x in re.split(r"(\d+)", path.name)]


def rgb565(image):
    image = image.convert("RGB")
    values = []
    for r, g, b in image.getdata():
        values.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return values


def rle(values):
    out, i, count = bytearray(), 0, len(values)
    while i < count:
        run = 1
        while i + run < count and values[i + run] == values[i] and run < 32768:
            run += 1
        if run >= 3:
            out += struct.pack("<HH", 0x8000 | run - 1, values[i]); i += run; continue
        start = i; i += run
        while i < count and i - start < 32768:
            look = 1
            while i + look < count and values[i + look] == values[i] and look < 3:
                look += 1
            if look >= 3: break
            i += look
        literal = values[start:i]
        out += struct.pack("<H", len(literal) - 1)
        out += struct.pack(f"<{len(literal)}H", *literal)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--loop", choices=MODE, default="forward-loop")
    ap.add_argument("--crop", type=int, nargs=4, default=(0, 0, 360, 360))
    ap.add_argument("--contains-blink", action="store_true")
    args = ap.parse_args()
    if not 4 <= args.fps <= 15: ap.error("fps 必须为 4..15")
    frames = sorted(args.input.glob("*.png"), key=natural)
    if not 1 <= len(frames) <= 24: ap.error("帧数必须为 1..24")
    x, y, w, h = args.crop
    payloads, decoded_crc = [], []
    for path in frames:
        image = Image.open(path)
        if image.size != (360, 360): raise SystemExit(f"{path}: 必须是 360x360")
        pixels = rgb565(image.crop((x, y, x + w, y + h)))
        raw = struct.pack(f"<{len(pixels)}H", *pixels)
        payloads.append(rle(pixels)); decoded_crc.append(binascii.crc32(raw) & 0xffffffff)
    index_bytes = len(frames) * 12
    data_offset = HEADER.size + index_bytes
    offsets, cursor = [], data_offset
    for data in payloads: offsets.append(cursor); cursor += len(data)
    flags = 1 if args.contains_blink else 0
    header0 = HEADER.pack(MAGIC, 1, len(frames), args.fps, MODE[args.loop], flags,
                          w, h, x, y, data_offset, 0)
    index = b"".join(struct.pack("<III", off, len(data), crc)
                     for off, data, crc in zip(offsets, payloads, decoded_crc))
    container_crc = binascii.crc32(header0[:-4] + index + b"".join(payloads)) & 0xffffffff
    header = header0[:-4] + struct.pack("<I", container_crc)
    args.output.mkdir(parents=True, exist_ok=True)
    clip = args.output / f"{args.name}.clip"
    container = header + index + b"".join(payloads)
    clip.write_bytes(container)
    manifest = {"version": 1, "name": args.name, "fps": args.fps,
                "loop": args.loop, "contains_blink": args.contains_blink,
                "frame_count": len(frames), "crop": args.crop,
                "container_crc32": f"{container_crc:08x}",
                "file_crc32": f"{binascii.crc32(container) & 0xffffffff:08x}",
                "frames": [{"offset": o, "size": len(d), "crc32": f"{c:08x}"}
                           for o, d, c in zip(offsets, payloads, decoded_crc)]}
    (args.output / f"{args.name}.jsn").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    raw_size = len(frames) * w * h * 2
    packed_size = clip.stat().st_size
    print(f"{args.name}: {len(frames)} frames @{args.fps}fps {args.loop}")
    print(f"raw={raw_size} packed={packed_size} ratio={raw_size/packed_size:.2f}x "
          f"saving={(1-packed_size/raw_size)*100:.1f}% PSRAM={packed_size} bytes")
    for index, data in enumerate(payloads):
        print(f"frame[{index:02d}]={len(data)} bytes crc32={decoded_crc[index]:08x}")
    print(f"SD: /sdcard/julia/clips/<state>/{args.name}.clip")


if __name__ == "__main__":
    main()
