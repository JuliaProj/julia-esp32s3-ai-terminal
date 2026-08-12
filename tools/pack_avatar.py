#!/usr/bin/env python3
"""将 PNG 帧序列打包为 RGB565 RLE 数据和 JSON 清单。"""

import argparse
import binascii
import json
import re
import struct
from pathlib import Path

from PIL import Image


def natural_key(path: Path):
    return [int(v) if v.isdigit() else v.lower() for v in re.split(r"(\d+)", path.name)]


def rgb565_pixels(image: Image.Image, crop, lvgl_swap):
    image = image.convert("RGB")
    x, y, w, h = crop
    if image.size != (360, 360):
        raise ValueError(f"{image}: 期望 360x360，实际 {image.size}")
    image = image.crop((x, y, x + w, y + h))
    pixels = []
    for r, g, b in image.getdata():
        value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        if lvgl_swap:
            value = ((value & 0xff) << 8) | (value >> 8)
        pixels.append(value)
    return pixels


def encode_rle(pixels):
    """控制字高位1表示重复段，低15位为长度减1；否则为原样段。"""
    out = bytearray()
    index = 0
    size = len(pixels)
    while index < size:
        run = 1
        while index + run < size and pixels[index + run] == pixels[index] and run < 32768:
            run += 1
        if run >= 3:
            out += struct.pack("<HH", 0x8000 | (run - 1), pixels[index])
            index += run
            continue
        start = index
        index += run
        while index < size and index - start < 32768:
            lookahead = 1
            while index + lookahead < size and pixels[index + lookahead] == pixels[index] and lookahead < 3:
                lookahead += 1
            if lookahead >= 3:
                break
            index += lookahead
        literal = pixels[start:index]
        out += struct.pack("<H", len(literal) - 1)
        out += struct.pack(f"<{len(literal)}H", *literal)
    return bytes(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="PNG目录")
    parser.add_argument("output", type=Path, help="输出目录")
    parser.add_argument("--name", required=True, help="序列名，建议不超过8字符")
    parser.add_argument("--fps", type=int, default=20)
    parser.add_argument("--pattern", default="*.png", help="输入匹配模式")
    parser.add_argument("--limit", type=int, default=0, help="仅打包前N帧，0表示全部")
    parser.add_argument("--lvgl-swap", action="store_true", help="输出适配LV_COLOR_16_SWAP=1的字节布局")
    parser.add_argument("--crop", type=int, nargs=4, metavar=("X", "Y", "W", "H"), default=(0, 0, 360, 360))
    args = parser.parse_args()
    frames = sorted(args.input.glob(args.pattern), key=natural_key)
    if args.limit > 0:
        frames = frames[:args.limit]
    if not frames:
        raise SystemExit("没有找到 PNG 帧")
    x, y, w, h = args.crop
    if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > 360 or y + h > 360:
        raise SystemExit("裁剪矩形超出360x360画布")
    args.output.mkdir(parents=True, exist_ok=True)
    bin_path = args.output / f"{args.name}.bin"
    manifest_path = args.output / f"{args.name}.jsn"
    offsets, sizes, compressed_crc32, decoded_crc32 = [], [], [], []
    raw_total = compressed_total = 0
    with bin_path.open("wb") as stream:
        for frame in frames:
            raw_size = w * h * 2
            pixels = rgb565_pixels(Image.open(frame), (x, y, w, h), args.lvgl_swap)
            raw = struct.pack(f"<{len(pixels)}H", *pixels)
            encoded = encode_rle(pixels)
            offsets.append(stream.tell())
            sizes.append(len(encoded))
            compressed_crc32.append(f"{binascii.crc32(encoded) & 0xffffffff:08x}")
            decoded_crc32.append(f"{binascii.crc32(raw) & 0xffffffff:08x}")
            stream.write(encoded)
            raw_total += raw_size
            compressed_total += len(encoded)
    manifest = {
        "version": 1,
        "name": args.name,
        "compression": 1,
        "pixel_format": "RGB565_LVGL_SWAP" if args.lvgl_swap else "RGB565_LE",
        "canvas": [360, 360],
        "crop": [x, y, w, h],
        "fps": args.fps,
        "frame_count": len(frames),
        "raw_frame_bytes": w * h * 2,
        "offsets": offsets,
        "sizes": sizes,
        "compressed_crc32": compressed_crc32,
        "decoded_crc32": decoded_crc32,
    }
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    ratio = raw_total / compressed_total
    print(f"frames={len(frames)} raw={raw_total} compressed={compressed_total} ratio={ratio:.2f}x saving={(1-compressed_total/raw_total)*100:.1f}%")
    print(bin_path)
    print(manifest_path)


if __name__ == "__main__":
    main()
