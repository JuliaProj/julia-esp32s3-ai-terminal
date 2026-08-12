#!/usr/bin/env python3
from pathlib import Path
from PIL import Image
import sys

bad = []
for path in Path(sys.argv[1]).glob("*_frames/*.png"):
    try:
        with Image.open(path) as image:
            image.verify()
    except Exception as error:
        bad.append((path, error))
for path, error in bad:
    print(f"{path}: {error}")
raise SystemExit(1 if bad else 0)
