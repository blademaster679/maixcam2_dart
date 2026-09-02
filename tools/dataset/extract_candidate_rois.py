#!/usr/bin/env python3
"""Export detector candidate ROIs for annotation and hard-negative mining."""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path

import cv2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", action="append", type=Path, required=True)
    parser.add_argument("--jsonl", action="append", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("all", "selected", "hard-negative"),
                        default="all")
    parser.add_argument("--annotations", type=Path,
                        help="optional interval JSON with target_present=false regions")
    parser.add_argument("--roi-factor", type=float, default=8.0)
    parser.add_argument("--min-side", type=int, default=64)
    parser.add_argument("--max-side", type=int, default=384)
    parser.add_argument("--max-candidates", type=int, default=3)
    parser.add_argument("--stride", type=int, default=1)
    parser.add_argument("--max-per-video", type=int, default=0)
    parser.add_argument("--processing-width", type=int, default=640,
                        help="coordinate width used by replay JSON")
    parser.add_argument("--processing-height", type=int, default=480,
                        help="coordinate height used by replay JSON")
    return parser.parse_args()


def video_intervals(annotations: dict, video: Path) -> list[dict]:
    videos = annotations.get("videos", {})
    for key in (str(video), video.name, video.stem):
        if key in videos:
            return videos[key]
    return []


def interval_at(intervals: list[dict], timestamp_s: float) -> dict | None:
    for interval in intervals:
        if float(interval.get("start_s", 0.0)) <= timestamp_s < float(
                interval.get("end_s", math.inf)):
            return interval
    return None


def crop_bounds(candidate: dict, width: int, height: int,
                factor: float, minimum: int, maximum: int) -> tuple[int, int, int]:
    side = round(factor * max(1.0, float(candidate["apparent_size"])))
    side = min(maximum, max(minimum, side), width, height)
    x = round(float(candidate["center_x"]) - side / 2)
    y = round(float(candidate["center_y"]) - side / 2)
    x = max(0, min(width - side, x))
    y = max(0, min(height - side, y))
    return x, y, side


def safe_name(path: Path) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", path.stem)


def export_pair(video: Path, jsonl_path: Path, output: Path, args: argparse.Namespace,
                annotations: dict, manifest) -> int:
    capture = cv2.VideoCapture(str(video))
    if not capture.isOpened():
        raise RuntimeError(f"cannot open video: {video}")
    fps = float(capture.get(cv2.CAP_PROP_FPS))
    if not math.isfinite(fps) or fps <= 0:
        fps = 30.0
    intervals = video_intervals(annotations, video)
    exported = 0
    with jsonl_path.open("r", encoding="utf-8") as detections:
        for line_index, line in enumerate(detections):
            ok, frame = capture.read()
            if not ok:
                break
            record = json.loads(line)
            frame_index = int(record.get("frame", line_index))
            if frame_index != line_index:
                capture.set(cv2.CAP_PROP_POS_FRAMES, frame_index)
                ok, frame = capture.read()
                if not ok:
                    break
            if (frame.shape[1], frame.shape[0]) != (args.processing_width,
                                                     args.processing_height):
                frame = cv2.resize(frame,
                                   (args.processing_width, args.processing_height),
                                   interpolation=cv2.INTER_AREA)
            if frame_index % args.stride:
                continue
            timestamp_s = float(record.get("timestamp_us", 0)) / 1_000_000.0
            annotation = interval_at(intervals, timestamp_s)
            candidates = list(record.get("candidates", []))
            if args.mode == "selected":
                candidates = [item for item in candidates if item.get("selected")]
            negative_source = None
            if args.mode == "hard-negative":
                if annotation is not None and annotation.get("target_present") is False:
                    negative_source = "annotated_negative_interval"
                elif record.get("safe_for_control"):
                    green = record.get("green", record)
                    green_x = float(green.get("center_x", 0))
                    green_y = float(green.get("center_y", 0))
                    green_size = float(green.get("apparent_size", 1))
                    candidates = [item for item in candidates
                                  if not item.get("selected") and
                                  math.hypot(float(item["center_x"]) - green_x,
                                             float(item["center_y"]) - green_y) >
                                  max(32.0, 4.0 * green_size,
                                      2.0 * float(item["apparent_size"]))]
                    negative_source = "far_from_confirmed_target"
                else:
                    continue
            candidates.sort(key=lambda item: float(item.get("score", 0)), reverse=True)
            for candidate_index, candidate in enumerate(candidates[:args.max_candidates]):
                x, y, side = crop_bounds(candidate, frame.shape[1], frame.shape[0],
                                          args.roi_factor, args.min_side, args.max_side)
                image_name = (f"{safe_name(video)}_f{frame_index:07d}_"
                              f"c{candidate_index}_{side}px.jpg")
                relative_path = Path("images") / image_name
                if not cv2.imwrite(str(output / relative_path), frame[y:y + side, x:x + side]):
                    raise RuntimeError(f"failed to write ROI: {relative_path}")
                item = {
                    "image": relative_path.as_posix(),
                    "video": str(video),
                    "sequence": video.stem,
                    "frame": frame_index,
                    "timestamp_us": int(record.get("timestamp_us", 0)),
                    "candidate_index": candidate_index,
                    "roi_xywh": [x, y, side, side],
                    "candidate": candidate,
                    "target_present": False if negative_source else (
                        None if annotation is None else
                        annotation.get("target_present")),
                }
                if negative_source:
                    item["negative_source"] = negative_source
                if annotation:
                    for key in ("venue", "distance_m", "color", "roll_deg",
                                "blur", "tags"):
                        if key in annotation:
                            item[key] = annotation[key]
                manifest.write(json.dumps(item, ensure_ascii=False) + "\n")
                exported += 1
                if args.max_per_video and exported >= args.max_per_video:
                    capture.release()
                    return exported
    capture.release()
    return exported


def main() -> None:
    args = parse_args()
    if len(args.video) != len(args.jsonl):
        raise SystemExit("--video and --jsonl must be supplied in matching pairs")
    if args.stride <= 0 or args.max_candidates <= 0 or args.min_side <= 0 or \
            args.max_side < args.min_side or args.processing_width <= 0 or \
            args.processing_height <= 0:
        raise SystemExit("invalid ROI/stride settings")
    annotations = {}
    if args.annotations:
        annotations = json.loads(args.annotations.read_text(encoding="utf-8"))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "images").mkdir(exist_ok=True)
    manifest_path = args.output / "manifest.jsonl"
    if manifest_path.exists():
        raise SystemExit(f"refusing to overwrite existing manifest: {manifest_path}")
    total = 0
    with manifest_path.open("w", encoding="utf-8") as manifest:
        for video, jsonl_path in zip(args.video, args.jsonl):
            count = export_pair(video, jsonl_path, args.output, args,
                                annotations, manifest)
            total += count
            print(f"{video}: exported {count}")
    print(f"total: {total} ROI(s); manifest: {manifest_path}")


if __name__ == "__main__":
    main()
