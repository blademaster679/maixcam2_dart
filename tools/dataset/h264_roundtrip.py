#!/usr/bin/env python3
"""Apply an exact H.264 encode/decode augmentation while preserving labels."""

from __future__ import annotations

import argparse
import random
import shutil
import subprocess
import tempfile
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", type=Path, required=True)
    parser.add_argument("--labels", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size", type=int, default=256)
    parser.add_argument("--crf-min", type=int, default=18)
    parser.add_argument("--crf-max", type=int, default=35)
    parser.add_argument("--seed", type=int, default=2026)
    args = parser.parse_args()
    if shutil.which("ffmpeg") is None:
        raise SystemExit("ffmpeg is required")
    if args.size <= 0 or not 0 <= args.crf_min <= args.crf_max <= 51:
        raise SystemExit("invalid size or CRF range")
    output_images = args.output / "images"
    output_labels = args.output / "labels"
    output_images.mkdir(parents=True, exist_ok=True)
    output_labels.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    count = 0
    with tempfile.TemporaryDirectory(prefix="dart_h264_") as temporary:
        encoded = Path(temporary) / "frame.mp4"
        for image in sorted(args.images.rglob("*")):
            if image.suffix.lower() not in {".jpg", ".jpeg", ".png"}:
                continue
            relative = image.relative_to(args.images)
            label = args.labels / relative.with_suffix(".txt")
            if not label.is_file():
                raise RuntimeError(f"missing label: {label}")
            destination = output_images / relative.with_suffix(".jpg")
            destination.parent.mkdir(parents=True, exist_ok=True)
            encoded.unlink(missing_ok=True)
            subprocess.run([
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                "-i", str(image), "-vf", f"scale={args.size}:{args.size}",
                "-frames:v", "1", "-c:v", "libx264", "-preset", "veryfast",
                "-crf", str(rng.randint(args.crf_min, args.crf_max)),
                "-pix_fmt", "yuv420p", str(encoded)], check=True)
            subprocess.run([
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                "-i", str(encoded), "-frames:v", "1", str(destination)], check=True)
            destination_label = output_labels / relative.with_suffix(".txt")
            destination_label.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(label, destination_label)
            count += 1
    print(f"wrote {count} H.264 round-trip samples to {args.output}")


if __name__ == "__main__":
    main()
