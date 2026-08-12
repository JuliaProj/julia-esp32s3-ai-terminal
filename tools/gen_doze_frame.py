"""Generate Julia's static 360x360 RGB565 doze frame."""
from __future__ import annotations

import argparse
import configparser
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter


ROOT = Path(__file__).resolve().parents[1]


def resolve(value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else ROOT / path


def rectangle(value: str) -> tuple[int, int, int, int]:
    parts = tuple(int(part.strip()) for part in value.split(","))
    if len(parts) != 4:
        raise ValueError(f"invalid rectangle: {value}")
    return parts


def rgb565(image: Image.Image) -> bytes:
    payload = bytearray()
    for r, g, b in image.convert("RGB").getdata():
        payload += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(payload)


def emit_c_array(path: Path, payload: bytes) -> None:
    lines = [
        '#include <stdint.h>',
        '#include "doze_frame.h"',
        '',
        'const uint8_t julia_doze_frame_rgb565[JULIA_DOZE_FRAME_BYTES] = {',
    ]
    for offset in range(0, len(payload), 16):
        lines.append("    " + ", ".join(f"0x{v:02x}" for v in payload[offset:offset + 16]) + ",")
    lines += ['};', '']
    path.write_text("\n".join(lines), encoding="ascii")
    path.with_suffix(".h").write_text(
        "#pragma once\n#include <stdint.h>\n#define JULIA_DOZE_FRAME_BYTES (360U * 360U * 2U)\n"
        "extern const uint8_t julia_doze_frame_rgb565[JULIA_DOZE_FRAME_BYTES];\n",
        encoding="ascii",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=str(ROOT / "tools/avatar_layers.conf"))
    parser.add_argument("--output", default=str(ROOT / "main/ui/generated/doze_frame.bin"))
    parser.add_argument("--preview", default=str(ROOT / "main/ui/generated/doze_frame_preview.png"))
    parser.add_argument("--c-array", help="optional .c path for a Flash fallback")
    args = parser.parse_args()

    cfg = configparser.ConfigParser()
    if not cfg.read(args.config, encoding="utf-8"):
        raise FileNotFoundError(args.config)
    base = Image.open(resolve(cfg["source"]["base"])).convert("RGB")
    opened = Image.open(resolve(cfg["source"]["blink_half"])).convert("RGB")
    closed = Image.open(resolve(cfg["source"]["blink_closed"])).convert("RGB")
    if base.size != (360, 360) or opened.size != (360, 360) or closed.size != (360, 360):
        raise ValueError("base and blink sources must all be 360x360")

    for key in ("eye_left", "eye_right"):
        x, y, width, height = rectangle(cfg["layers"][key])
        box = (x, y, x + width, y + height)
        layer_path = resolve(cfg["source"]["output"]) / f"{key}_closed.png"
        closed_crop = (Image.open(layer_path).convert("RGB") if layer_path.is_file()
                       else closed.crop(box))
        mask = Image.new("L", (width, height), 0)
        ImageDraw.Draw(mask).ellipse((3, 10, width - 4, height - 5), fill=255)
        mask = mask.filter(ImageFilter.GaussianBlur(2.0))
        base.paste(closed_crop, (x, y), mask)

    # The source blink retains a narrow iris highlight; close that last slit
    # while preserving the asset's brow and eyelash alignment.
    draw = ImageDraw.Draw(base)
    for cx in (140, 221):
        skin = closed.getpixel((cx, 145))
        draw.ellipse((cx - 20, 145, cx + 20, 174), fill=skin)
        draw.arc((cx - 17, 140, cx + 17, 164), 15, 165, fill=(72, 48, 47), width=3)
        draw.line((cx - 15, 158, cx - 20, 161), fill=(72, 48, 47), width=2)
        draw.line((cx + 15, 158, cx + 20, 161), fill=(72, 48, 47), width=2)

    # A restrained hand-drawn sleep mark, positioned away from facial layers.
    ink = (90, 108, 139)
    for x, y, scale in ((278, 72, 1.0), (305, 51, 0.78), (326, 36, 0.58)):
        width, height = int(25 * scale), int(18 * scale)
        stroke = max(2, int(3 * scale))
        draw.line((x, y, x + width, y), fill=ink, width=stroke)
        draw.line((x + width, y, x, y + height), fill=ink, width=stroke)
        draw.line((x, y + height, x + width, y + height), fill=ink, width=stroke)

    base = ImageEnhance.Brightness(base).enhance(0.85)
    pixels = []
    for r, g, b in base.getdata():
        pixels.append((max(0, int(r * 0.96)), max(0, int(g * 0.99)), min(255, int(b * 1.05))))
    base.putdata(pixels)

    output = Path(args.output)
    preview = Path(args.preview)
    output.parent.mkdir(parents=True, exist_ok=True)
    preview.parent.mkdir(parents=True, exist_ok=True)
    payload = rgb565(base)
    output.write_bytes(payload)
    base.save(preview)
    if args.c_array:
        emit_c_array(Path(args.c_array), payload)
    print(f"generated {output} ({len(payload)} bytes), preview={preview}")


if __name__ == "__main__":
    main()
