#!/usr/bin/env python3
"""Convert GIF, MP4, or a PNG sequence to Julia raw RGB565 .trn format."""

from __future__ import annotations

import argparse
import binascii
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageSequence

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "assets" / "transitions"
WIDTH = HEIGHT = 360
FRAME_BYTES = WIDTH * HEIGHT * 2
HEADER = struct.Struct("<4sBHHHBBII11s")
CLIP_HEADER = struct.Struct("<4sHHHBBHHIIII")


def color_swap_enabled(sdkconfig: Path) -> bool:
    if not sdkconfig.exists():
        raise SystemExit(f"sdkconfig not found: {sdkconfig}")
    settings = sdkconfig.read_text(encoding="utf-8", errors="replace").splitlines()
    return "CONFIG_LV_COLOR_16_SWAP=y" in settings


def fit_frame(image: Image.Image, size: int = WIDTH) -> Image.Image:
    image = image.convert("RGB")
    scale = max(size / image.width, size / image.height)
    scaled_size = (round(image.width * scale), round(image.height * scale))
    image = image.resize(scaled_size, Image.Resampling.LANCZOS)
    left = (image.width - size) // 2
    top = (image.height - size) // 2
    return image.crop((left, top, left + size, top + size))


def lock_green_pupils(image: Image.Image) -> Image.Image:
    image = image.copy()
    pixels = image.load()
    scale = image.width / 360.0
    for x0, y0, x1, y1 in ((120, 125, 175, 180), (185, 125, 240, 180)):
        for y in range(round(y0 * scale), round(y1 * scale)):
            for x in range(round(x0 * scale), round(x1 * scale)):
                red, green, blue = pixels[x, y]
                if green >= 45 and green > red * 1.25 and green > blue * 1.25:
                    pixels[x, y] = (76, 175, 80)
    return image


def encode_rgb565(image: Image.Image, swap: bool, size: int = WIDTH) -> bytes:
    output = bytearray(size * size * 2)
    cursor = 0
    fitted = lock_green_pupils(fit_frame(image, size))
    for red, green, blue in fitted.getdata():
        value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        if swap:
            output[cursor] = value >> 8
            output[cursor + 1] = value & 0xFF
        else:
            output[cursor] = value & 0xFF
            output[cursor + 1] = value >> 8
        cursor += 2
    return bytes(output)


def rle_frame(frame: bytes) -> bytes:
    values = struct.unpack(f"<{len(frame) // 2}H", frame)
    output = bytearray()
    cursor = 0
    while cursor < len(values):
        run = 1
        while cursor + run < len(values) and values[cursor + run] == values[cursor] and run < 32768:
            run += 1
        if run >= 3:
            output += struct.pack("<HH", 0x8000 | (run - 1), values[cursor])
            cursor += run
            continue
        start = cursor
        cursor += run
        while cursor < len(values) and cursor - start < 32768:
            look = 1
            while cursor + look < len(values) and values[cursor + look] == values[cursor] and look < 3:
                look += 1
            if look >= 3:
                break
            cursor += look
        literal = values[start:cursor]
        output += struct.pack("<H", len(literal) - 1)
        output += struct.pack(f"<{len(literal)}H", *literal)
    return bytes(output)


def image_frames(source: Path):
    if source.is_dir():
        files = sorted(source.glob("*.png"))
        if not files:
            raise SystemExit(f"no PNG frames in {source}")
        for path in files:
            with Image.open(path) as image:
                yield image.copy()
        return
    if source.suffix.lower() == ".gif":
        with Image.open(source) as image:
            for frame in ImageSequence.Iterator(image):
                yield frame.convert("RGB")
        return
    if source.suffix.lower() == ".png":
        with Image.open(source) as image:
            yield image.copy()
        return
    raise ValueError("not an image input")


def extract_mp4(source: Path, fps: int, directory: Path):
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg:
        output = directory / "%06d.png"
        subprocess.run(
            [ffmpeg, "-v", "error", "-i", str(source), "-vf", f"fps={fps}", str(output)],
            check=True,
        )
        yield from image_frames(directory)
        return

    try:
        import cv2
    except ImportError as error:
        raise SystemExit("MP4 input requires ffmpeg or the opencv-python package") from error

    capture = cv2.VideoCapture(str(source))
    source_fps = capture.get(cv2.CAP_PROP_FPS)
    if not capture.isOpened() or source_fps <= 0:
        raise SystemExit(f"OpenCV could not decode MP4 input: {source}")
    next_output_time = 0.0
    output_interval = 1.0 / fps
    frame_index = 0
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        frame_time = frame_index / source_fps
        if frame_time + (0.5 / source_fps) >= next_output_time:
            yield Image.fromarray(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
            next_output_time += output_interval
        frame_index += 1
    capture.release()


def decode_clip(source: Path, swap: bool) -> tuple[list[bytes], int]:
    data = source.read_bytes()
    if len(data) < CLIP_HEADER.size:
        raise SystemExit(f"clip header truncated: {source}")
    magic, version, count, fps, _mode, _flags, width, height, _x, _y, _offset, _crc = (
        CLIP_HEADER.unpack_from(data)
    )
    if magic != b"JCLP" or version != 1 or width != WIDTH or height != HEIGHT:
        raise SystemExit(f"unsupported clip: {source}")
    frames: list[bytes] = []
    for index in range(count):
        offset, length, expected_crc = struct.unpack_from("<III", data, CLIP_HEADER.size + index * 12)
        packed = memoryview(data)[offset : offset + length]
        values: list[int] = []
        cursor = 0
        while cursor < len(packed):
            control = struct.unpack_from("<H", packed, cursor)[0]
            cursor += 2
            run = (control & 0x7FFF) + 1
            if control & 0x8000:
                value = struct.unpack_from("<H", packed, cursor)[0]
                cursor += 2
                values.extend([value] * run)
            else:
                values.extend(struct.unpack_from(f"<{run}H", packed, cursor))
                cursor += run * 2
        if len(values) != WIDTH * HEIGHT:
            raise SystemExit(f"clip frame {index} decoded pixels={len(values)}")
        raw_le = struct.pack(f"<{len(values)}H", *values)
        if binascii.crc32(raw_le) & 0xFFFFFFFF != expected_crc:
            raise SystemExit(f"clip frame {index} CRC mismatch")
        frame = bytearray(FRAME_BYTES)
        for pixel_index, value in enumerate(values):
            byte_index = pixel_index * 2
            frame[byte_index] = value >> 8 if swap else value & 0xFF
            frame[byte_index + 1] = value & 0xFF if swap else value >> 8
        frames.append(bytes(frame))
    return frames, fps


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--fps", type=int, default=15, choices=range(1, 61))
    parser.add_argument("--sdkconfig", type=Path, default=ROOT / "sdkconfig")
    parser.add_argument("--rle", action="store_true", help="RLE-compress each RGB565 frame")
    parser.add_argument("--resolution", type=int, choices=(180, 360), default=360)
    parser.add_argument("--frame-count", type=int, choices=range(2, 121),
                        help="uniformly resample the source to this many frames")
    args = parser.parse_args()

    source = args.input.resolve()
    if not source.exists():
        raise SystemExit(f"input not found: {source}")
    output = args.output
    if not output.is_absolute():
        output = DEFAULT_OUTPUT / output
    output.parent.mkdir(parents=True, exist_ok=True)
    swap = color_swap_enabled(args.sdkconfig)

    if source.suffix.lower() == ".clip":
        encoded, source_fps = decode_clip(source, swap)
        if args.fps == 15:
            args.fps = source_fps
    else:
        with tempfile.TemporaryDirectory(prefix="julia_trn_") as temp:
            if source.suffix.lower() in {".mp4", ".mov", ".m4v"}:
                frames = list(extract_mp4(source, args.fps, Path(temp)))
            else:
                frames = list(image_frames(source))
        if not frames or len(frames) > 120:
            raise SystemExit(f"frame count must be 1-120, got {len(frames)}")
        encoded = [encode_rgb565(frame, swap, args.resolution) for frame in frames]
    if source.suffix.lower() == ".clip" and args.resolution != 360:
        raise SystemExit("--resolution 180 is only supported for image/GIF/MP4 input")
    if args.frame_count and len(encoded) != args.frame_count:
        last = len(encoded) - 1
        encoded = [encoded[round(i * last / (args.frame_count - 1))]
                   for i in range(args.frame_count)]
    stored = [rle_frame(frame) for frame in encoded] if args.rle else encoded
    frame_bytes = args.resolution * args.resolution * 2
    offsets, cursor = [], 0
    for frame in stored:
        offsets.append(cursor)
        cursor += len(frame)
    total = HEADER.size + len(offsets) * 4 + cursor
    header = HEADER.pack(
        b"JTRN", 1, args.resolution, args.resolution, len(encoded), args.fps,
        1 if args.rle else 0, frame_bytes, total, bytes(11),
    )
    with output.open("wb") as stream:
        stream.write(header)
        stream.write(struct.pack(f"<{len(offsets)}I", *offsets))
        for frame in stored:
            stream.write(frame)
    print(
        f"TRN output={output} frames={len(encoded)} fps={args.fps} "
        f"bytes={total} format={'rle' if args.rle else 'raw'} resolution={args.resolution} "
        f"LV_COLOR_16_SWAP={int(swap)} byte_order={'high-first' if swap else 'low-first'}"
    )


if __name__ == "__main__":
    main()
