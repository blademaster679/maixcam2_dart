#!/usr/bin/env python3
"""Create deterministic pose augmentations with geometry-aware keypoints."""

from __future__ import annotations

import argparse
import random
from pathlib import Path

import cv2
import numpy as np


def parse_label(path: Path) -> list[float]:
    lines = [line for line in path.read_text(encoding="utf-8").splitlines()
             if line.strip()]
    if len(lines) != 1:
        raise RuntimeError(f"ROI labels must contain exactly one target: {path}")
    values = [float(value) for value in lines[0].split()]
    if len(values) != 20 or values[0] != 0:
        raise RuntimeError(f"expected class 0 and five x/y/visibility keypoints: {path}")
    return values


def transform_point(matrix: np.ndarray, x: float, y: float) -> tuple[float, float]:
    result = matrix @ np.array([x, y, 1.0], dtype=np.float32)
    return float(result[0]), float(result[1])


def motion_blur(image: np.ndarray, length: int, angle_deg: float) -> np.ndarray:
    if length <= 1:
        return image
    kernel = np.zeros((length, length), dtype=np.float32)
    cv2.line(kernel, (0, length // 2), (length - 1, length // 2), 1.0, 1)
    rotation = cv2.getRotationMatrix2D((length / 2 - 0.5, length / 2 - 0.5),
                                       angle_deg, 1.0)
    kernel = cv2.warpAffine(kernel, rotation, (length, length))
    kernel /= max(1.0e-6, float(kernel.sum()))
    return cv2.filter2D(image, -1, kernel)


def structure_copy(image: np.ndarray, values: list[float], background: np.ndarray,
                   rng: random.Random) -> tuple[np.ndarray, list[float]]:
    height, width = image.shape[:2]
    cx, cy, bw, bh = values[1] * width, values[2] * height, \
                     values[3] * width, values[4] * height
    padding = 0.15 * max(bw, bh)
    x0 = max(0, round(cx - bw / 2 - padding))
    y0 = max(0, round(cy - bh / 2 - padding))
    x1 = min(width, round(cx + bw / 2 + padding))
    y1 = min(height, round(cy + bh / 2 + padding))
    patch = image[y0:y1, x0:x1]
    if patch.size == 0:
        return image, values
    canvas = cv2.resize(background, (width, height), interpolation=cv2.INTER_AREA)
    destination_x = rng.randint(0, max(0, width - patch.shape[1]))
    destination_y = rng.randint(0, max(0, height - patch.shape[0]))
    mask = np.full(patch.shape[:2], 255, dtype=np.uint8)
    feather = max(1, min(patch.shape[:2]) // 12 * 2 + 1)
    mask = cv2.GaussianBlur(mask, (feather, feather), 0).astype(np.float32) / 255.0
    region = canvas[destination_y:destination_y + patch.shape[0],
                    destination_x:destination_x + patch.shape[1]]
    region[:] = (patch * mask[..., None] + region * (1.0 - mask[..., None])).astype(
        np.uint8)
    remapped = list(values)
    offset_x = destination_x - x0
    offset_y = destination_y - y0
    remapped[1] = (cx + offset_x) / width
    remapped[2] = (cy + offset_y) / height
    for keypoint in range(5):
        base = 5 + 3 * keypoint
        if remapped[base + 2] > 0:
            remapped[base] = (values[base] * width + offset_x) / width
            remapped[base + 1] = (values[base + 1] * height + offset_y) / height
    return canvas, remapped


def augment(image: np.ndarray, values: list[float], rng: random.Random,
            max_blur: int) -> tuple[np.ndarray, list[float]]:
    height, width = image.shape[:2]
    angle = rng.uniform(-180.0, 180.0)
    # Candidate ROIs place the lamp at roughly 1/8 of the input width. Most
    # samples therefore shrink to the planned 3--20 px lamp regime, while a
    # smaller branch retains close-range large targets.
    scale = rng.uniform(0.10, 0.65) if rng.random() < 0.75 else \
        rng.uniform(0.65, 1.30)
    matrix = cv2.getRotationMatrix2D((width / 2, height / 2), angle, scale)
    matrix[0, 2] += rng.uniform(-0.08, 0.08) * width
    matrix[1, 2] += rng.uniform(-0.08, 0.08) * height
    transformed = cv2.warpAffine(image, matrix, (width, height),
                                 borderMode=cv2.BORDER_REFLECT_101)

    cx, cy, bw, bh = values[1] * width, values[2] * height, \
                     values[3] * width, values[4] * height
    corners = [(cx - bw / 2, cy - bh / 2), (cx + bw / 2, cy - bh / 2),
               (cx + bw / 2, cy + bh / 2), (cx - bw / 2, cy + bh / 2)]
    mapped_corners = [transform_point(matrix, x, y) for x, y in corners]
    xs, ys = zip(*mapped_corners)
    x0, x1 = max(0.0, min(xs)), min(float(width), max(xs))
    y0, y1 = max(0.0, min(ys)), min(float(height), max(ys))
    output = list(values)
    output[1] = (x0 + x1) / (2 * width)
    output[2] = (y0 + y1) / (2 * height)
    output[3] = max(0.0, x1 - x0) / width
    output[4] = max(0.0, y1 - y0) / height
    for keypoint in range(5):
        base = 5 + 3 * keypoint
        if output[base + 2] <= 0:
            continue
        x, y = transform_point(matrix, values[base] * width,
                               values[base + 1] * height)
        output[base] = min(1.0, max(0.0, x / width))
        output[base + 1] = min(1.0, max(0.0, y / height))
        if x < 0 or x >= width or y < 0 or y >= height:
            output[base + 2] = 0.0

    temperature = rng.uniform(-0.22, 0.22)
    gains = np.array([1.0 - temperature, rng.uniform(0.85, 1.15),
                      1.0 + temperature], dtype=np.float32)
    transformed = np.clip(transformed.astype(np.float32) * gains,
                          0, 255).astype(np.uint8)
    transformed = motion_blur(transformed, rng.randint(0, max_blur),
                              rng.uniform(0, 360))
    if rng.random() < 0.35:
        ox = rng.randint(0, max(0, width - 1))
        oy = rng.randint(0, max(0, height - 1))
        ow = rng.randint(max(1, width // 25), max(2, width // 6))
        oh = rng.randint(max(1, height // 25), max(2, height // 6))
        cv2.rectangle(transformed, (ox, oy), (min(width, ox + ow), min(height, oy + oh)),
                      tuple(rng.randint(0, 80) for _ in range(3)), -1)
        for keypoint in range(5):
            base = 5 + 3 * keypoint
            px, py = output[base] * width, output[base + 1] * height
            if ox <= px <= ox + ow and oy <= py <= oy + oh:
                output[base + 2] = min(1.0, output[base + 2])
    return transformed, output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", type=Path, required=True)
    parser.add_argument("--labels", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--copies", type=int, default=2)
    parser.add_argument("--background-dir", type=Path)
    parser.add_argument("--structure-copy-probability", type=float, default=0.25)
    parser.add_argument("--max-motion-blur", type=int, default=12)
    parser.add_argument("--seed", type=int, default=2026)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    output_images = args.output / "images"
    output_labels = args.output / "labels"
    output_images.mkdir(parents=True, exist_ok=True)
    output_labels.mkdir(parents=True, exist_ok=True)
    backgrounds = [] if not args.background_dir else [
        path for path in args.background_dir.rglob("*")
        if path.suffix.lower() in {".jpg", ".jpeg", ".png"}]
    count = 0
    for image_path in sorted(args.images.rglob("*")):
        if image_path.suffix.lower() not in {".jpg", ".jpeg", ".png"}:
            continue
        label_path = args.labels / image_path.relative_to(args.images).with_suffix(".txt")
        if not label_path.exists():
            raise RuntimeError(f"missing label: {label_path}")
        original = cv2.imread(str(image_path))
        values = parse_label(label_path)
        for copy_index in range(args.copies):
            image = original.copy()
            label = list(values)
            if backgrounds and rng.random() < args.structure_copy_probability:
                background = cv2.imread(str(rng.choice(backgrounds)))
                if background is not None:
                    image, label = structure_copy(image, label, background, rng)
            image, label = augment(image, label, rng, args.max_motion_blur)
            stem = f"{image_path.stem}_aug{copy_index:02d}"
            quality = rng.randint(45, 96)  # Compression robustness augmentation.
            cv2.imwrite(str(output_images / f"{stem}.jpg"), image,
                        [cv2.IMWRITE_JPEG_QUALITY, quality])
            (output_labels / f"{stem}.txt").write_text(
                " ".join(f"{value:.7g}" for value in label) + "\n", encoding="utf-8")
            count += 1
    print(f"wrote {count} augmented image/label pairs to {args.output}")


if __name__ == "__main__":
    main()
