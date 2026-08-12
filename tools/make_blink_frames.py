from pathlib import Path

import cv2
import numpy as np


BASE = Path(r"D:\ComfyUI\ComfyUI_windows_portable_nvidia\ComfyUI_windows_portable\ComfyUI\output\Julia")
DEVICE_OUTPUT = Path(__file__).resolve().parents[1] / "file" / "ui_animation" / "standby"
OPEN_PATH = BASE / "neck_outfit_final_00001_.png"
CLOSED_PATH = BASE / "v2_blink_closed_00001_.png"
MOUTH_PATHS = {
    "mouth_00_closed.png": OPEN_PATH,
    "mouth_02_medium.png": BASE / "v3_mouth_medium_00001_.png",
    "mouth_03_wide.png": BASE / "v3_mouth_wide_00001_.png",
}


def feathered_eye_mask(height: int, width: int) -> np.ndarray:
    mask = np.zeros((height, width), np.float32)
    y = int(height * 0.43)
    for x in (int(width * 0.39), int(width * 0.61)):
        cv2.ellipse(mask, (x, y), (int(width * 0.09), int(height * 0.075)),
                    0, 0, 360, 1.0, -1)
    return cv2.GaussianBlur(mask, (0, 0), sigmaX=10, sigmaY=10)[..., None]


def main() -> None:
    opened = cv2.imread(str(OPEN_PATH), cv2.IMREAD_COLOR)
    closed = cv2.imread(str(CLOSED_PATH), cv2.IMREAD_COLOR)
    if opened is None or closed is None:
        raise FileNotFoundError("Missing generated blink source image")

    gray_open = cv2.cvtColor(opened, cv2.COLOR_BGR2GRAY).astype(np.float32) / 255.0
    gray_closed = cv2.cvtColor(closed, cv2.COLOR_BGR2GRAY).astype(np.float32) / 255.0
    warp = np.eye(2, 3, dtype=np.float32)
    criteria = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 150, 1e-6)
    cv2.findTransformECC(gray_open, gray_closed, warp, cv2.MOTION_AFFINE, criteria)
    aligned = cv2.warpAffine(closed, warp, (opened.shape[1], opened.shape[0]),
                             flags=cv2.INTER_CUBIC | cv2.WARP_INVERSE_MAP,
                             borderMode=cv2.BORDER_REFLECT)

    mask = feathered_eye_mask(opened.shape[0], opened.shape[1])
    opened_f = opened.astype(np.float32)
    aligned_f = aligned.astype(np.float32)
    closed_frame = opened_f * (1.0 - mask) + aligned_f * mask
    half_source = opened_f * 0.48 + aligned_f * 0.52
    half_frame = opened_f * (1.0 - mask) + half_source * mask

    cv2.imwrite(str(BASE / "blink_stable_open.png"), opened)
    cv2.imwrite(str(BASE / "blink_stable_half.png"), np.clip(half_frame, 0, 255).astype(np.uint8))
    cv2.imwrite(str(BASE / "blink_stable_closed.png"), np.clip(closed_frame, 0, 255).astype(np.uint8))

    DEVICE_OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, frame in (("blink_00_open.png", opened),
                        ("blink_01_closed.png", closed_frame),
                        ("blink_02_open.png", opened)):
        resized = cv2.resize(np.clip(frame, 0, 255).astype(np.uint8),
                             (360, 360), interpolation=cv2.INTER_AREA)
        cv2.imwrite(str(DEVICE_OUTPUT / name), resized)

    background = np.median(np.concatenate((opened[:24, :24], opened[:24, -24:]), axis=1),
                           axis=(0, 1)).astype(np.uint8)
    distance = np.linalg.norm(opened.astype(np.float32) - background.astype(np.float32), axis=2)
    foreground = cv2.GaussianBlur((distance > 18).astype(np.float32), (0, 0), 1.2)
    height, width = opened.shape[:2]
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    strength = np.clip((yy - height * 0.43) / (height * 0.57), 0.0, 1.0)
    map_y = yy + strength * 3.0
    moved = cv2.remap(opened, xx, map_y, cv2.INTER_CUBIC, borderMode=cv2.BORDER_REFLECT)
    moved_mask = cv2.remap(foreground, xx, map_y, cv2.INTER_LINEAR,
                           borderMode=cv2.BORDER_CONSTANT)[..., None]
    breath = opened_f * (1.0 - moved_mask) + moved.astype(np.float32) * moved_mask
    breath = cv2.resize(np.clip(breath, 0, 255).astype(np.uint8),
                        (360, 360), interpolation=cv2.INTER_AREA)
    cv2.imwrite(str(DEVICE_OUTPUT / "breath_inhale.png"), breath)

    mouth_mask = np.zeros((height, width), np.float32)
    cv2.ellipse(mouth_mask, (int(width * 0.50), int(height * 0.565)),
                (int(width * 0.075), int(height * 0.07)), 0, 0, 360, 1.0, -1)
    mouth_mask = cv2.GaussianBlur(mouth_mask, (0, 0), 7)[..., None]
    for name, path in MOUTH_PATHS.items():
        source = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if source is None:
            raise FileNotFoundError(path)
        source_gray = cv2.cvtColor(source, cv2.COLOR_BGR2GRAY).astype(np.float32) / 255.0
        mouth_warp = np.eye(2, 3, dtype=np.float32)
        cv2.findTransformECC(gray_open, source_gray, mouth_warp, cv2.MOTION_AFFINE, criteria)
        mouth_aligned = cv2.warpAffine(source, mouth_warp, (width, height),
                                       flags=cv2.INTER_CUBIC | cv2.WARP_INVERSE_MAP,
                                       borderMode=cv2.BORDER_REFLECT)
        frame = opened_f * (1.0 - mouth_mask) + mouth_aligned.astype(np.float32) * mouth_mask
        frame = cv2.resize(np.clip(frame, 0, 255).astype(np.uint8),
                           (360, 360), interpolation=cv2.INTER_AREA)
        cv2.imwrite(str(DEVICE_OUTPUT / name), frame)
    cv2.imwrite(str(DEVICE_OUTPUT / "mouth_01_small.png"),
                cv2.resize(opened, (360, 360), interpolation=cv2.INTER_AREA))


if __name__ == "__main__":
    main()
