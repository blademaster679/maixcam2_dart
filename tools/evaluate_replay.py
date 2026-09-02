#!/usr/bin/env python3
"""Aggregate v0.2 replay metrics, optionally against interval ground truth."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from pathlib import Path


def percentile(values: list[float], percentage: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * percentage / 100.0
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] * (upper - position) + ordered[upper] * (position - lower)


def intervals_for(annotations: dict, name: str) -> list[dict]:
    videos = annotations.get("videos", {})
    for key in (name, Path(name).name, Path(name).stem):
        if key in videos:
            return videos[key]
    return []


def interval_at(intervals: list[dict], timestamp_s: float) -> dict | None:
    for interval in intervals:
        if float(interval.get("start_s", 0.0)) <= timestamp_s < float(
                interval.get("end_s", math.inf)):
            return interval
    return None


def longest_false_run(rows: list[dict], predicate) -> float:
    longest_us = 0
    start_us: int | None = None
    last_us = 0
    nominal_step = 0
    timestamps = [int(row.get("timestamp_us", 0)) for row in rows]
    steps = [b - a for a, b in zip(timestamps, timestamps[1:]) if b > a]
    if steps:
        nominal_step = int(statistics.median(steps))
    for row in rows:
        timestamp = int(row.get("timestamp_us", 0))
        if predicate(row):
            if start_us is None:
                start_us = timestamp
            last_us = timestamp
        elif start_us is not None:
            longest_us = max(longest_us, last_us - start_us + nominal_step)
            start_us = None
    if start_us is not None:
        longest_us = max(longest_us, last_us - start_us + nominal_step)
    return longest_us / 1000.0


def evaluate_one(path: Path, video_name: str, annotations: dict,
                 jump_threshold_deg: float, ground_truth_path: Path | None,
                 candidate_match_px: float, switch_error_px: float) -> dict:
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()]
    if not rows:
        raise RuntimeError(f"empty JSONL: {path}")
    ground_truth = {}
    if ground_truth_path:
        ground_truth = {int(item["frame"]): item for item in
                        (json.loads(line) for line in ground_truth_path.read_text(
                            encoding="utf-8").splitlines() if line.strip())}
    intervals = intervals_for(annotations, video_name)
    positive_rows = []
    negative_rows = []
    for row in rows:
        truth = ground_truth.get(int(row.get("frame", -1)))
        interval = interval_at(intervals, int(row.get("timestamp_us", 0)) / 1e6)
        target_present = truth.get("target_present") if truth is not None else (
            None if interval is None else interval.get("target_present"))
        if target_present is True:
            positive_rows.append(row)
        elif target_present is False:
            negative_rows.append(row)
    has_ground_truth = bool(intervals) or bool(ground_truth)
    measured_rows = positive_rows if has_ground_truth else rows
    observed = [row for row in measured_rows
                if row.get("green", row).get("valid") and
                not row.get("green", row).get("predicted")]
    safe = [row for row in measured_rows if row.get("safe_for_control")]
    armor_rows = [row for row in measured_rows
                  if row.get("armor", {}).get("valid")]
    pose_rows = [row for row in measured_rows if row.get("pose", {}).get("valid")]
    false_safe = (sum(bool(row.get("safe_for_control")) for row in negative_rows)
                  if has_ground_truth else None)
    suspicious_jumps = 0
    previous = None
    threshold_rad = math.radians(jump_threshold_deg)
    for row in rows:
        green = row.get("green", row)
        if green.get("valid") and not green.get("predicted"):
            if previous is not None:
                dt = int(row.get("timestamp_us", 0)) - int(
                    previous.get("timestamp_us", 0))
                delta = math.hypot(float(row.get("yaw_rad", 0)) -
                                   float(previous.get("yaw_rad", 0)),
                                   float(row.get("pitch_rad", 0)) -
                                   float(previous.get("pitch_rad", 0)))
                if 0 < dt <= 100_000 and delta > threshold_rad:
                    suspicious_jumps += 1
            previous = row
    capture_delays_ms = []
    for interval in intervals:
        if interval.get("target_present") is not True:
            continue
        start_us = round(float(interval["start_s"]) * 1e6)
        end_us = round(float(interval["end_s"]) * 1e6)
        first = next((row for row in rows
                      if start_us <= int(row.get("timestamp_us", 0)) < end_us and
                      row.get("safe_for_control")), None)
        capture_delays_ms.append(None if first is None else
                                 (int(first["timestamp_us"]) - start_us) / 1000.0)
    finite_captures = [value for value in capture_delays_ms if value is not None]
    green_errors = []
    keypoint_errors = []
    angle_errors_deg = []
    candidate_hits = 0
    candidate_trials = 0
    wrong_track_switches = 0
    wrong_track_active = False
    for row in rows:
        truth = ground_truth.get(int(row.get("frame", -1)))
        if not truth or truth.get("target_present") is not True:
            wrong_track_active = False
            continue
        gt_green = truth.get("green_center")
        if gt_green:
            candidate_trials += 1
            if any(math.hypot(float(item["center_x"]) - float(gt_green[0]),
                              float(item["center_y"]) - float(gt_green[1])) <=
                   candidate_match_px for item in row.get("candidates", [])):
                candidate_hits += 1
            green = row.get("green", row)
            if green.get("valid") and not green.get("predicted"):
                error = math.hypot(float(green["center_x"]) - float(gt_green[0]),
                                   float(green["center_y"]) - float(gt_green[1]))
                green_errors.append(error)
                keypoint_errors.append(error)
                is_wrong = bool(row.get("safe_for_control")) and error > switch_error_px
                if is_wrong and not wrong_track_active:
                    wrong_track_switches += 1
                wrong_track_active = is_wrong
            else:
                wrong_track_active = False
        gt_keypoints = truth.get("keypoints", [])
        frame_armor = row.get("armor", {})
        detected_points = [
            [row.get("green", row).get("center_x", 0),
             row.get("green", row).get("center_y", 0),
             row.get("green", row).get("valid", False)],
            [frame_armor.get("left_bar", {}).get("top", {}).get("x", 0),
             frame_armor.get("left_bar", {}).get("top", {}).get("y", 0),
             frame_armor.get("left_bar", {}).get("top", {}).get("valid", False)],
            [frame_armor.get("left_bar", {}).get("bottom", {}).get("x", 0),
             frame_armor.get("left_bar", {}).get("bottom", {}).get("y", 0),
             frame_armor.get("left_bar", {}).get("bottom", {}).get("valid", False)],
            [frame_armor.get("right_bar", {}).get("top", {}).get("x", 0),
             frame_armor.get("right_bar", {}).get("top", {}).get("y", 0),
             frame_armor.get("right_bar", {}).get("top", {}).get("valid", False)],
            [frame_armor.get("right_bar", {}).get("bottom", {}).get("x", 0),
             frame_armor.get("right_bar", {}).get("bottom", {}).get("y", 0),
             frame_armor.get("right_bar", {}).get("bottom", {}).get("valid", False)],
        ]
        for detected, expected in zip(detected_points[1:], gt_keypoints[1:]):
            expected_visible = len(expected) < 3 or float(expected[2]) > 0
            if detected[2] and expected_visible:
                keypoint_errors.append(math.hypot(
                    float(detected[0]) - float(expected[0]),
                    float(detected[1]) - float(expected[1])))
        if "yaw_rad" in truth and "pitch_rad" in truth and row.get("valid"):
            angle_errors_deg.append(math.degrees(math.hypot(
                float(row.get("yaw_rad", 0)) - float(truth["yaw_rad"]),
                float(row.get("pitch_rad", 0)) - float(truth["pitch_rad"]))))
    denominator = max(1, len(measured_rows))
    report = {
        "video": video_name,
        "jsonl": str(path),
        "frames": len(rows),
        "ground_truth_intervals": bool(intervals),
        "frame_ground_truth": bool(ground_truth),
        "positive_frames": len(positive_rows),
        "negative_frames": len(negative_rows),
        "observed_rate": len(observed) / denominator,
        "safe_rate": len(safe) / denominator,
        "fused_control_rate": len(safe) / denominator,
        "armor_rate": len(armor_rows) / denominator,
        "pose_rate": len(pose_rows) / denominator,
        "longest_unsafe_ms": longest_false_run(
            measured_rows, lambda row: not row.get("safe_for_control")),
        "false_safe_outputs": false_safe,
        "suspicious_track_jumps": suspicious_jumps,
        "wrong_track_switches": wrong_track_switches if ground_truth else None,
        "candidate_recall": None if not candidate_trials else
            candidate_hits / candidate_trials,
        "green_center_p95_px": percentile(green_errors, 95),
        "keypoint_p95_px": percentile(keypoint_errors, 95),
        "angle_p95_deg": percentile(angle_errors_deg, 95),
        "capture_delays_ms": capture_delays_ms,
        "capture_within_100ms_rate": None if not capture_delays_ms else
            sum(value is not None and value <= 100.0 for value in capture_delays_ms) /
            len(capture_delays_ms),
        "capture_p95_ms": percentile(finite_captures, 95),
        "processing_p50_ms": percentile(
            [float(row.get("processing_ms", 0)) for row in rows], 50),
        "processing_p95_ms": percentile(
            [float(row.get("processing_ms", 0)) for row in rows], 95),
        "pose_reprojection_p95_px": percentile(
            [float(row["pose"]["reprojection_error_px"])
             for row in pose_rows], 95),
    }
    return report


def markdown(reports: list[dict]) -> str:
    lines = ["# v0.2 replay report", "",
             "> Rates are recall-like only when interval or frame ground truth is present. "
             "Host processing time is not a MaixCAM2 latency measurement.", "",
             "| video | GT | candidate | observed | safe | armor | longest unsafe | false safe | switches | KP P95 | angle P95 | host P95 |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for item in reports:
        gt_state = "frame" if item["frame_ground_truth"] else (
            "interval" if item["ground_truth_intervals"] else "no")
        switches = (item["wrong_track_switches"] if item["wrong_track_switches"] is not None
                    else item["suspicious_track_jumps"])
        false_safe = ("-" if item["false_safe_outputs"] is None else
                      str(item["false_safe_outputs"]))
        keypoint = "-" if item["keypoint_p95_px"] is None else f"{item['keypoint_p95_px']:.2f}px"
        angle = "-" if item["angle_p95_deg"] is None else f"{item['angle_p95_deg']:.3f}°"
        candidate = ("-" if item["candidate_recall"] is None else
                     f"{item['candidate_recall']:.2%}")
        lines.append(
            f"| {item['video']} | {gt_state} | {candidate} | "
            f"{item['observed_rate']:.2%} | {item['safe_rate']:.2%} | "
            f"{item['armor_rate']:.2%} | {item['longest_unsafe_ms']:.1f} ms | "
            f"{false_safe} | {switches} | {keypoint} | {angle} | "
            f"{item['processing_p95_ms']:.2f} ms |")
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("jsonl", nargs="+", type=Path)
    parser.add_argument("--video-name", action="append",
                        help="repeat once per JSONL; defaults to JSONL stem")
    parser.add_argument("--ground-truth-jsonl", action="append", type=Path,
                        help="repeat per replay JSONL; rows contain frame/target_present/centers")
    parser.add_argument("--annotations", type=Path)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-markdown", type=Path, required=True)
    parser.add_argument("--jump-threshold-deg", type=float, default=1.0)
    parser.add_argument("--candidate-match-px", type=float, default=5.0)
    parser.add_argument("--switch-error-px", type=float, default=12.0)
    parser.add_argument("--enforce", action="store_true",
                        help="fail annotated acceptance metrics below plan thresholds")
    args = parser.parse_args()
    if args.video_name and len(args.video_name) != len(args.jsonl):
        raise SystemExit("--video-name count must match JSONL count")
    if args.ground_truth_jsonl and len(args.ground_truth_jsonl) != len(args.jsonl):
        raise SystemExit("--ground-truth-jsonl count must match JSONL count")
    annotations = {} if not args.annotations else json.loads(
        args.annotations.read_text(encoding="utf-8"))
    names = args.video_name or [path.stem for path in args.jsonl]
    truths = args.ground_truth_jsonl or [None] * len(args.jsonl)
    reports = [evaluate_one(path, name, annotations, args.jump_threshold_deg,
                            truth, args.candidate_match_px, args.switch_error_px)
               for path, name, truth in zip(args.jsonl, names, truths)]
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_markdown.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps({"schema_version": 1, "videos": reports},
                                           indent=2, ensure_ascii=False) + "\n",
                                encoding="utf-8")
    args.output_markdown.write_text(markdown(reports), encoding="utf-8")
    print(markdown(reports), end="")
    if args.enforce:
        failures = []
        annotated = [item for item in reports
                     if item["ground_truth_intervals"] or item["frame_ground_truth"]]
        if not annotated:
            failures.append("no annotated videos; acceptance cannot be enforced")
        for item in annotated:
            if item["positive_frames"] and item["observed_rate"] < 0.97:
                failures.append(f"{item['video']}: observed rate below 97%")
            if item["positive_frames"] and item["safe_rate"] < 0.995:
                failures.append(f"{item['video']}: safe rate below 99.5%")
            if item["positive_frames"] and item["longest_unsafe_ms"] > 50.0:
                failures.append(f"{item['video']}: unsafe gap exceeds 50ms")
            if item["false_safe_outputs"]:
                failures.append(f"{item['video']}: false control-safe output")
            if item["candidate_recall"] is not None and item["candidate_recall"] < 0.995:
                failures.append(f"{item['video']}: candidate recall below 99.5%")
            if item["keypoint_p95_px"] is not None and item["keypoint_p95_px"] > 2.0:
                failures.append(f"{item['video']}: keypoint P95 exceeds 2px")
            if item["angle_p95_deg"] is not None and item["angle_p95_deg"] > 0.1:
                failures.append(f"{item['video']}: angular P95 exceeds 0.1deg")
            if item["wrong_track_switches"]:
                failures.append(f"{item['video']}: wrong track switch")
            capture_rate = item["capture_within_100ms_rate"]
            if capture_rate is not None and capture_rate < 0.99:
                failures.append(f"{item['video']}: capture success below 99%")
        if failures:
            raise SystemExit("acceptance failed:\n" + "\n".join(failures))


if __name__ == "__main__":
    main()
