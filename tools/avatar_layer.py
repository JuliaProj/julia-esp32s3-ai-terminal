"""Build small, independently redrawable LVGL avatar layers.

The tool is intentionally configuration-driven. It runs on the PC during the
build/release workflow; firmware only embeds the resulting RGB565(A) blobs.
"""
from __future__ import annotations

import argparse
import configparser
import json
import struct
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]


def rect(value: str) -> tuple[int, int, int, int]:
    values = tuple(int(part.strip()) for part in value.split(","))
    if len(values) != 4 or values[2] <= 0 or values[3] <= 0:
        raise ValueError(f"invalid rectangle: {value}")
    return values


def rgb565_bytes(image: Image.Image, alpha: bool = False) -> bytes:
    image = image.convert("RGBA")
    result = bytearray()
    for r, g, b, a in image.getdata():
        raw = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        result += struct.pack("<H", raw)
        if alpha:
            result.append(a)
    return bytes(result)


def resolve(value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else ROOT / path


def load_optional(cfg: configparser.ConfigParser, key: str, fallback: Image.Image) -> Image.Image:
    path = resolve(cfg["source"].get(key, ""))
    return Image.open(path).convert("RGBA") if path.is_file() else fallback.copy()


def ellipse_layer(base: Image.Image, box: tuple[int, int, int, int], inset: int) -> Image.Image:
    x, y, w, h = box
    layer = base.crop((x, y, x + w, y + h)).convert("RGBA")
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).ellipse((inset, inset, w - inset - 1, h - inset - 1), fill=255)
    layer.putalpha(mask)
    return layer


def emit_descriptor(output: Path, specs: list[dict[str, object]]) -> None:
    header = """#pragma once
#include \"lvgl.h\"
typedef enum { AVATAR_MOUTH_CLOSED, AVATAR_MOUTH_HALF, AVATAR_MOUTH_OPEN } avatar_mouth_frame_t;
const lv_img_dsc_t *avatar_layer_eye(bool left, uint8_t frame);
const lv_img_dsc_t *avatar_layer_pupil(bool left);
const lv_img_dsc_t *avatar_layer_mouth(avatar_mouth_frame_t frame);
const lv_img_dsc_t *avatar_layer_hair_tip(void);
typedef struct {
    const char *name;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    lv_img_cf_t color_format;
    uint32_t data_size;
    const uint8_t *data_start;
    const uint8_t *data_end;
} avatar_layer_asset_info_t;
const avatar_layer_asset_info_t *avatar_layer_asset_info(const lv_img_dsc_t *descriptor);
bool avatar_layer_asset_valid(const lv_img_dsc_t *descriptor);
"""
    (output / "avatar_layer_assets.h").write_text(header, encoding="ascii")
    lines = ['#include "avatar_layer_assets.h"', ""]
    for item in specs:
        name, width, height, alpha = item["name"], item["width"], item["height"], item["alpha"]
        bpp = 3 if alpha else 2
        lines += [
            f'extern const uint8_t {name}_bin_start[] asm("_binary_{name}_bin_start");',
            f'extern const uint8_t {name}_bin_end[] asm("_binary_{name}_bin_end");',
            f"static const lv_img_dsc_t {name}_dsc = {{",
            f"  .header.always_zero=0, .header.w={width}, .header.h={height},",
            f"  .header.cf={'LV_IMG_CF_TRUE_COLOR_ALPHA' if alpha else 'LV_IMG_CF_TRUE_COLOR'},",
            f"  .data_size={width}*{height}*{3 if alpha else 2}, .data={name}_bin_start,",
            "};", "",
            f'static const avatar_layer_asset_info_t {name}_info = {{"{name}", {width}, {height}, {width * bpp},',
            f"  {'LV_IMG_CF_TRUE_COLOR_ALPHA' if alpha else 'LV_IMG_CF_TRUE_COLOR'}, {width * height * bpp}, {name}_bin_start, {name}_bin_end}};", "",
        ]
    lines += [
        "const lv_img_dsc_t *avatar_layer_eye(bool left, uint8_t frame) {",
        "  static const lv_img_dsc_t *e[2][3]={{&eye_left_open_dsc,&eye_left_half_dsc,&eye_left_closed_dsc},{&eye_right_open_dsc,&eye_right_half_dsc,&eye_right_closed_dsc}};",
        "  return e[left?0:1][frame<3?frame:0];", "}",
        "const lv_img_dsc_t *avatar_layer_pupil(bool left) { return left?&avatar_pupil_left_dsc:&avatar_pupil_right_dsc; }",
        "const lv_img_dsc_t *avatar_layer_mouth(avatar_mouth_frame_t frame) {",
        "  static const lv_img_dsc_t *m[3]={&mouth_closed_dsc,&mouth_half_dsc,&mouth_open_dsc}; return m[frame<3?frame:2];", "}",
        "const lv_img_dsc_t *avatar_layer_hair_tip(void) { return &hair_tip_dsc; }", "",
        "const avatar_layer_asset_info_t *avatar_layer_asset_info(const lv_img_dsc_t *descriptor) {",
        "  static const struct { const lv_img_dsc_t *d; const avatar_layer_asset_info_t *i; } assets[] = {",
    ]
    for item in specs:
        lines.append(f"    {{&{item['name']}_dsc, &{item['name']}_info}},")
    lines += [
        "  };",
        "  for (unsigned i=0; i<sizeof(assets)/sizeof(assets[0]); ++i) if (assets[i].d==descriptor) return assets[i].i;",
        "  return NULL;", "}",
        "bool avatar_layer_asset_valid(const lv_img_dsc_t *descriptor) {",
        "  const avatar_layer_asset_info_t *i=avatar_layer_asset_info(descriptor);",
        "  return i && descriptor->data && descriptor->data==i->data_start &&",
        "         descriptor->header.w==i->width && descriptor->header.h==i->height &&",
        "         descriptor->header.cf==i->color_format && descriptor->data_size==i->data_size &&",
        "         (uint32_t)(i->data_end-i->data_start)==i->data_size;", "}", "",
    ]
    (output / "avatar_layer_assets.c").write_text("\n".join(lines), encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=str(ROOT / "tools/avatar_layers.conf"))
    args = parser.parse_args()
    cfg = configparser.ConfigParser()
    if not cfg.read(args.config, encoding="utf-8"):
        raise FileNotFoundError(args.config)
    base = Image.open(resolve(cfg["source"]["base"])).convert("RGBA")
    half = load_optional(cfg, "blink_half", base)
    closed = load_optional(cfg, "blink_closed", base)
    mouth_sources = [load_optional(cfg, key, base) for key in ("mouth_closed", "mouth_half", "mouth_open")]
    output = resolve(cfg["source"]["output"])
    output.mkdir(parents=True, exist_ok=True)
    boxes = {key: rect(value) for key, value in cfg["layers"].items()}
    layers: list[tuple[str, Image.Image, bool, tuple[int, int, int, int]]] = []
    for side in ("left", "right"):
        box = boxes[f"eye_{side}"]
        for frame_name, source in (("open", base), ("half", half), ("closed", closed)):
            x, y, w, h = box
            layers.append((f"eye_{side}_{frame_name}", source.crop((x, y, x+w, y+h)), False, box))
        pbox = boxes[f"pupil_{side}"]
        layers.append((f"avatar_pupil_{side}", ellipse_layer(base, pbox, cfg.getint("pupil", "alpha_inset")), True, pbox))
    mbox = boxes["mouth"]
    for name, source in zip(("closed", "half", "open"), mouth_sources):
        x, y, w, h = mbox
        layers.append((f"mouth_{name}", source.crop((x, y, x+w, y+h)), False, mbox))
    hbox = boxes["hair_tip"]
    x, y, w, h = hbox
    hair = base.crop((x, y, x+w, y+h))
    if not cfg.getboolean("options", "hair_tip"):
        hair.putalpha(0)
    layers.append(("hair_tip", hair, True, hbox))
    specs = []
    manifest = {"source": str(resolve(cfg["source"]["base"])), "layers": []}
    for name, image, alpha, box in layers:
        payload = rgb565_bytes(image, alpha)
        (output / f"{name}.bin").write_bytes(payload)
        image.save(output / f"{name}.png")
        spec = {"name": name, "width": image.width, "height": image.height, "alpha": alpha, "bytes": len(payload), "rect": box}
        specs.append(spec)
        manifest["layers"].append(spec)
        if len(payload) >= 10 * 1024:
            raise RuntimeError(f"{name} is {len(payload)} bytes, exceeds the 10KB layer limit")
    emit_descriptor(output, specs)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="ascii")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
