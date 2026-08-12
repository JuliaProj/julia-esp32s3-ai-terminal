#!/usr/bin/env python3
"""Generate Julia offline prompts as signed 16-bit, 16 kHz, mono raw PCM."""

import argparse
import asyncio
import json
import subprocess
import tempfile
from pathlib import Path

import edge_tts
import imageio_ffmpeg

ROOT = Path(__file__).resolve().parents[1]


async def synthesize(item: dict, output: Path, ffmpeg: str) -> None:
    with tempfile.TemporaryDirectory(prefix="julia_tts_") as temporary:
        mp3 = Path(temporary) / f"{item['name']}.mp3"
        await edge_tts.Communicate(item["text"], item.get("voice", "zh-CN-XiaoxiaoNeural")).save(mp3)
        subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(mp3),
                        "-f", "s16le", "-acodec", "pcm_s16le", "-ar", "16000",
                        "-ac", "1", str(output)], check=True)
    if output.stat().st_size < 3200 or output.stat().st_size % 2:
        raise RuntimeError(f"invalid PCM output: {output}")


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=ROOT / "tools/offline_tts_prompts.json")
    parser.add_argument("--output", type=Path, default=ROOT / "assets/offline_audio")
    args = parser.parse_args()
    prompts = json.loads(args.manifest.read_text(encoding="utf-8"))
    args.output.mkdir(parents=True, exist_ok=True)
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    for item in prompts:
        destination = args.output / f"{item['name']}.pcm"
        await synthesize(item, destination, ffmpeg)
        print(f"generated {destination.name}: {destination.stat().st_size} bytes")


if __name__ == "__main__":
    asyncio.run(main())
