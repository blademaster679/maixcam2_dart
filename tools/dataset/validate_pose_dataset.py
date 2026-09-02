#!/usr/bin/env python3
"""Validate one-class YOLO pose labels with five ordered keypoints."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp"}


def validate_label(path: Path) -> list[str]:
    errors: list[str] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 20:
            errors.append(f"{path}:{line_number}: expected 20 values, got {len(fields)}")
            continue
        try:
            values = [float(value) for value in fields]
        except ValueError:
            errors.append(f"{path}:{line_number}: non-numeric value")
            continue
        if values[0] != 0:
            errors.append(f"{path}:{line_number}: only class 0 dart_target is allowed")
        for index in range(1, 5):
            if not 0.0 <= values[index] <= 1.0:
                errors.append(f"{path}:{line_number}: bbox value outside [0,1]")
        for keypoint in range(5):
            x, y, visibility = values[5 + 3 * keypoint:8 + 3 * keypoint]
            if visibility not in (0.0, 1.0, 2.0):
                errors.append(f"{path}:{line_number}: visibility must be 0, 1 or 2")
            if visibility > 0 and not (0.0 <= x <= 1.0 and 0.0 <= y <= 1.0):
                errors.append(f"{path}:{line_number}: visible keypoint outside image")
        if values[7] == 0:
            errors.append(f"{path}:{line_number}: green keypoint must be visible")
    return errors


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--split-manifest", type=Path,
                        help="combined JSONL containing split and sequence")
    args = parser.parse_args()
    errors: list[str] = []
    image_count = 0
    label_count = 0
    for split in ("train", "val", "test"):
        images = args.root / "images" / split
        labels = args.root / "labels" / split
        if not images.is_dir() or not labels.is_dir():
            errors.append(f"missing images/{split} or labels/{split}")
            continue
        for image in images.rglob("*"):
            if image.suffix.lower() not in IMAGE_SUFFIXES:
                continue
            image_count += 1
            relative = image.relative_to(images).with_suffix(".txt")
            label = labels / relative
            if not label.exists():
                errors.append(f"missing label for {image}")
                continue
            label_count += 1
            errors.extend(validate_label(label))
    if args.split_manifest:
        owners: dict[str, str] = {}
        for line in args.split_manifest.read_text(encoding="utf-8").splitlines():
            row = json.loads(line)
            sequence, split = str(row["sequence"]), str(row["split"])
            if sequence in owners and owners[sequence] != split:
                errors.append(f"sequence leakage: {sequence} in {owners[sequence]} and {split}")
            owners[sequence] = split
    if errors:
        print("\n".join(errors[:100]))
        raise SystemExit(f"validation failed with {len(errors)} error(s)")
    print(f"valid: {image_count} images, {label_count} labels, 1 class, 5 keypoints")


if __name__ == "__main__":
    main()
