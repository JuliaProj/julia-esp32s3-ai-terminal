#!/usr/bin/env python3
"""Generate short, once-hold state transition clips for SD deployment."""

import argparse
import binascii
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "main" / "ui" / "generated"
DEFAULT_OUTPUT = GENERATED / "transitions"

# id, source image, target image, frames, fps, legacy face x/y motion
SCRIPTS = (
    ("TR_S1_S2", "julia_s1_1_near_standby.png", "julia_s2_1_observe.png", 7, 6, 1, 2),
    ("TR_S2_S1", "julia_s2_1_observe.png", "julia_s1_1_near_standby.png", 7, 6, -1, -2),
    ("TR_S1_S3", "julia_s1_1_near_standby.png", "julia_s3_3_user_call.png", 7, 6, 0, 4),
    ("TR_S3_S4", "julia_s3_3_user_call.png", "julia_s4_1_light_dialog.png", 7, 6, 0, 1),
    ("TR_S4_S1", "julia_s4_1_light_dialog.png", "julia_s1_1_near_standby.png", 7, 6, 0, -2),
    ("TR_S4_S5", "julia_s4_1_light_dialog.png", "julia_s5_1_user_reject.png", 7, 6, -4, 1),
    ("TR_S5_S1", "julia_s5_1_user_reject.png", "julia_s1_1_near_standby.png", 7, 6, 1, 0),
    ("TR_S1_S0", "julia_s1_1_near_standby.png", "julia_s0_1_night_sleep.png", 7, 6, 0, -1),
    ("TR_S0_S1", "julia_s0_1_night_sleep.png", "julia_s1_1_near_standby.png", 7, 6, 0, 2),
    ("TR_S2_S3", "julia_s2_1_observe.png", "julia_s3_1_emotion_trigger.png", 7, 6, 0, 4),
    ("TR_S5_S2", "julia_s5_1_user_reject.png", "julia_s2_1_observe.png", 7, 6, 1, 2),
    ("TR_S5_S4", "julia_s5_1_user_reject.png", "julia_s4_1_light_dialog.png", 7, 6, 1, 3),
)

TRN_HEADER = struct.Struct("<4sBHHHBBII11s")


def decode_rle(data, pixels):
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
            size = count * 2
            output += data[cursor:cursor + size]
            cursor += size
    if len(output) != pixels * 2:
        raise ValueError("invalid TRN RLE frame")
    return bytes(output)


def trn_frames(path):
    data = path.read_bytes()
    magic, version, width, height, count, _fps, fmt, frame_bytes, total, _ = \
        TRN_HEADER.unpack_from(data)
    if magic != b"JTRN" or version != 1 or total != len(data):
        raise ValueError(f"invalid TRN: {path}")
    offsets = struct.unpack_from(f"<{count}I", data, TRN_HEADER.size)
    base = TRN_HEADER.size + count * 4
    result = []
    for index, offset in enumerate(offsets):
        end = offsets[index + 1] if index + 1 < count else len(data) - base
        packed = data[base + offset:base + end]
        raw = decode_rle(packed, width * height) if fmt == 1 else packed
        if len(raw) != frame_bytes:
            raise ValueError(f"invalid TRN frame {index}: {path}")
        # Assets are stored in the byte order used by the LCD (big-endian RGB565).
        rgb = bytearray(width * height * 3)
        for pixel in range(width * height):
            value = (raw[pixel * 2] << 8) | raw[pixel * 2 + 1]
            rgb[pixel * 3] = ((value >> 11) & 0x1F) * 255 // 31
            rgb[pixel * 3 + 1] = ((value >> 5) & 0x3F) * 255 // 63
            rgb[pixel * 3 + 2] = (value & 0x1F) * 255 // 31
        result.append(Image.frombytes("RGB", (width, height), bytes(rgb)))
    return result


def render_motion_frames(source, count, directory):
    frames = trn_frames(source)
    indices = [round(i * (len(frames) - 1) / (count - 1)) for i in range(count)]
    for output_index, source_index in enumerate(indices):
        frame = frames[source_index]
        # Preserve the strong body/expression movement while retaining the compact
        # RLE footprint required for all showcase clips to coexist in PSRAM.
        frame = frame.resize((64, 64), Image.Resampling.BOX).resize(
            (360, 360), Image.Resampling.NEAREST)
        frame = frame.quantize(colors=16, method=Image.Quantize.MEDIANCUT).convert("RGB")
        frame.save(directory / f"frame_{output_index:02d}.png", optimize=True)


def smoothstep(value):
    return value * value * (3.0 - 2.0 * value)


def render_frames(source, target, count, dx, dy, directory):
    source = Image.open(source).convert("RGB")
    target = Image.open(target).convert("RGB")
    if source.size != (360, 360) or target.size != (360, 360):
        raise SystemExit("transition sources must be 360x360")
    face_box = (72, 42, 288, 306)
    for index in range(count):
        t = smoothstep(index / (count - 1))
        frame = Image.blend(source, target, t)
        # Move only the face/upper-body region. The full canvas is never transformed.
        face = frame.crop(face_box)
        moved = Image.new("RGB", face.size, frame.getpixel((180, 180)))
        moved.paste(face, (round(dx * t), round(dy * t)))
        mask = Image.new("L", face.size, 255)
        frame.paste(moved, face_box, mask)
        # A small fixed palette is intentional: it keeps 5-8 frame RLE clips under 200 KiB.
        frame = frame.quantize(colors=12, method=Image.Quantize.MEDIANCUT).convert("RGB")
        frame = frame.resize((64, 64), Image.Resampling.BOX).resize(
            (360, 360), Image.Resampling.NEAREST)
        frame.save(directory / f"frame_{index:02d}.png", optimize=True)


def write_table(manifests, destination):
    lines = [
        "/* Generated by tools/gen_transition_clips.py. */",
        "#pragma once", "", "#include \"avatar_clip_map.h\"", "",
        "static const avatar_clip_descriptor_t g_transition_clips[] = {",
    ]
    for manifest, relpath in manifests:
        frames = manifest["frames"]
        offsets = ", ".join(str(frame["offset"]) for frame in frames)
        sizes = ", ".join(str(frame["size"]) for frame in frames)
        crcs = ", ".join("0x" + frame["crc32"] for frame in frames)
        lines += [
            "    {",
            f'        .name = "{manifest["name"]}", .path = "/sdcard/julia/clips/{relpath}",',
            f'        .compressed_crc32 = 0x{manifest["file_crc32"]}U,',
            f'        .frame_count = {manifest["frame_count"]}, .fps = {manifest["fps"]},',
            "        .transition_ms = 0, .contains_blink = true,",
            "        .mode = AVATAR_CLIP_ONCE_HOLD, .transition = AVATAR_TRANSITION_CROSS_FADE,",
            f"        .frame_offsets = {{{offsets}}},",
            f"        .frame_sizes = {{{sizes}}},",
            f"        .decoded_crc32 = {{{crcs}}},",
            "    },",
        ]
    lines += ["};", "", "#define TRANSITION_CLIP_COUNT (sizeof(g_transition_clips) / sizeof(g_transition_clips[0]))", ""]
    destination.write_text("\n".join(lines), encoding="ascii")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifests = []
    total = 0
    for clip_id, start, end, count, fps, dx, dy in SCRIPTS:
        work = args.output / (clip_id + "_frames")
        if work.exists():
            shutil.rmtree(work)
        work.mkdir()
        route = clip_id.removeprefix("TR_")
        motion_source = ROOT / "assets" / "transitions" / "consistent_v2" / \
            "transitions" / route / f"{route}.trn"
        if motion_source.exists():
            render_motion_frames(motion_source, count, work)
        else:
            render_frames(GENERATED / start, GENERATED / end, count, dx, dy, work)
        subprocess.run([sys.executable, str(ROOT / "tools" / "clip_packer.py"),
                        str(work), str(args.output), "--name", clip_id,
                        "--fps", str(fps), "--loop", "once-hold"], check=True)
        manifest_path = args.output / f"{clip_id}.jsn"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        size = (args.output / f"{clip_id}.clip").stat().st_size
        if size > 400 * 1024:
            raise SystemExit(f"{clip_id}: {size} exceeds 400 KiB")
        total += size
        manifests.append((manifest, f"{clip_id}.clip"))
    limit = 4600 * 1024
    if total > limit:
        raise SystemExit(f"transition total {total} exceeds 2.5 MiB")
    write_table(manifests, args.output / "transition_clip_table.h")
    report = {"clips": len(manifests), "packed_bytes": total,
              "crc32": f"{binascii.crc32(b''.join((args.output / (m['name'] + '.clip')).read_bytes() for m, _ in manifests)) & 0xffffffff:08x}"}
    (args.output / "size_report.json").write_text(json.dumps(report, indent=2), encoding="ascii")
    print(f"TOTAL clips={len(manifests)} packed={total} bytes limit={limit} PASS")


if __name__ == "__main__":
    main()
