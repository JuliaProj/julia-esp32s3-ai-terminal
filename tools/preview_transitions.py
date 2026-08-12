#!/usr/bin/env python3
"""Render Julia .trn assets to animated GIFs and an HTML gallery."""

from __future__ import annotations

import argparse
import html
import struct
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct("<4sBHHHBBII11s")


def decode_rle(data: bytes, pixels: int) -> bytes:
    output = bytearray()
    cursor = 0
    while cursor < len(data):
        control = struct.unpack_from("<H", data, cursor)[0]
        cursor += 2
        count = (control & 0x7FFF) + 1
        if control & 0x8000:
            value = data[cursor:cursor + 2]
            cursor += 2
            output += value * count
        else:
            length = count * 2
            output += data[cursor:cursor + length]
            cursor += length
    if len(output) != pixels * 2:
        raise ValueError(f"RLE decoded {len(output)} bytes, expected {pixels * 2}")
    return bytes(output)


def rgb565_image(frame: bytes, width: int, height: int, swapped: bool) -> Image.Image:
    rgb = bytearray(width * height * 3)
    for index in range(width * height):
        first, second = frame[index * 2:index * 2 + 2]
        value = (first << 8 | second) if swapped else (second << 8 | first)
        rgb[index * 3] = ((value >> 11) & 0x1F) * 255 // 31
        rgb[index * 3 + 1] = ((value >> 5) & 0x3F) * 255 // 63
        rgb[index * 3 + 2] = (value & 0x1F) * 255 // 31
    image = Image.frombytes("RGB", (width, height), bytes(rgb))
    if width != 360 or height != 360:
        image = image.resize((360, 360), Image.Resampling.NEAREST)
    return image


def render(source: Path, target: Path, swapped: bool) -> tuple[int, int]:
    data = source.read_bytes()
    magic, version, width, height, count, fps, fmt, frame_bytes, total, _ = HEADER.unpack_from(data)
    if magic != b"JTRN" or version != 1 or total != len(data) or fmt not in (0, 1):
        raise ValueError(f"invalid or unsupported TRN: {source}")
    offsets = struct.unpack_from(f"<{count}I", data, HEADER.size)
    base = HEADER.size + count * 4
    frames = []
    for index, offset in enumerate(offsets):
        end = offsets[index + 1] if index + 1 < count else len(data) - base
        packed = data[base + offset:base + end]
        raw = decode_rle(packed, width * height) if fmt == 1 else packed
        if len(raw) != frame_bytes:
            raise ValueError(f"frame {index} has {len(raw)} bytes, expected {frame_bytes}")
        frames.append(rgb565_image(raw, width, height, swapped))
    target.parent.mkdir(parents=True, exist_ok=True)
    duration = max(20, round(1000 / fps))
    frames[0].save(target, save_all=True, append_images=frames[1:], duration=duration,
                   loop=0, disposal=2, optimize=False)
    return count, fps


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=ROOT / "assets/transitions")
    parser.add_argument("--output", type=Path, default=ROOT / "tmp/transition_preview")
    parser.add_argument("--swap", type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    cards = []
    for source in sorted(args.input.rglob("*.trn")):
        relative = source.relative_to(args.input)
        route = str(relative.parent).replace("\\", "/")
        safe_route = route.replace("/", "_")
        target = args.output / f"{safe_route}_{source.stem}.gif"
        count, fps = render(source, target, bool(args.swap))
        cards.append((route, source.stem, target.name, count, fps))
        print(f"PREVIEW route={route} clip={source.stem} frames={count} fps={fps} output={target}")
    if not cards:
        raise SystemExit(f"no transition files found under {args.input}")
    sections = "\n".join(
        f'<figure><img src="{html.escape(filename)}" alt="{html.escape(route)}">'
        f'<figcaption><strong>{html.escape(route)}</strong><span>{html.escape(name)}</span>'
        f'<small>{count} 帧 · {fps} fps · {count / fps:.2f}s</small></figcaption></figure>'
        for route, name, filename, count, fps in cards
    )
    page = f"""<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Julia 动画预览</title>
<style>body{{margin:0;background:#151719;color:#f4f5f6;font-family:Segoe UI,Microsoft YaHei,sans-serif}}
header{{padding:24px 32px;border-bottom:1px solid #34383c}}h1{{font-size:22px;margin:0 0 6px}}p{{margin:0;color:#aeb5bb}}
main{{display:grid;grid-template-columns:repeat(auto-fit,minmax(360px,1fr));gap:1px;background:#34383c}}
figure{{margin:0;padding:24px;background:#1d2023}}img{{display:block;width:100%;max-width:480px;aspect-ratio:1;object-fit:contain;margin:auto;background:#000}}
figcaption{{max-width:480px;margin:14px auto 0;display:grid;grid-template-columns:auto 1fr auto;gap:10px;align-items:baseline}}
figcaption span,small{{color:#aeb5bb}}small{{text-align:right}}</style></head><body>
<header><h1>Julia 全部动画</h1><p>设备 RGB565 原帧解码预览，循环播放</p></header><main>{sections}</main></body></html>"""
    (args.output / "index.html").write_text(page, encoding="utf-8")
    print(f"GALLERY output={args.output / 'index.html'}")


if __name__ == "__main__":
    main()
