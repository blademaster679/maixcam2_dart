#!/usr/bin/env python3
"""Evaluate a 30-minute MaixCAM2 JSONL log against runtime thresholds."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] if low == high else (
        ordered[low] * (high - position) + ordered[high] * (position - low))


def linear_slope(xs: list[float], ys: list[float]) -> float:
    x_mean, y_mean = statistics.mean(xs), statistics.mean(ys)
    denominator = sum((x - x_mean) ** 2 for x in xs)
    if denominator <= 0:
        return 0.0
    return sum((x - x_mean) * (y - y_mean) for x, y in zip(xs, ys)) / denominator


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--fps", type=float, default=60.0)
    parser.add_argument("--min-actual-fps", type=float, default=59.0,
                        help="minimum measured frame-index rate")
    parser.add_argument("--min-duration-min", type=float, default=30.0)
    parser.add_argument("--max-processing-p95-ms", type=float, default=30.0)
    parser.add_argument("--max-model-p95-ms", type=float, default=20.0)
    parser.add_argument("--max-drop-rate", type=float, default=0.001)
    parser.add_argument("--max-rss-growth-kb-per-min", type=float, default=64.0)
    parser.add_argument("--allow-no-model", action="store_true",
                        help="do not fail a classical-only benchmark")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    # MaixCAM2 drivers write initialization/shutdown diagnostics to the same
    # stream as the JSONL telemetry. Ignore those plain-text lines so a raw
    # board log can be evaluated without a manual cleanup step.
    rows = []
    for line in args.log.read_text(encoding="utf-8").splitlines():
        if not line.lstrip().startswith("{"):
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            # SIGINT can arrive while the final telemetry line is being
            # written. Earlier complete frames remain valid for a soak test.
            continue
    if len(rows) < 2:
        raise SystemExit("device log has fewer than two frames")
    duration_s = (int(rows[-1]["timestamp_us"]) - int(rows[0]["timestamp_us"])) / 1e6
    processing = [float(row["processing_ms"]) for row in rows]
    classical = [float(row["classical_detection_ms"]) for row in rows
                 if row.get("classical_detection_ran")]
    visual_motion = [float(row["visual_motion_ms"]) for row in rows
                     if row.get("visual_motion_ran")]
    capture_intervals = [float(row.get("capture_interval_us", 0)) / 1000.0
                         for row in rows[1:]
                         if float(row.get("capture_interval_us", 0)) > 0]
    model = [float(row["model_inference_ms"]) for row in rows if row.get("model_ran")]
    expected_interval_us = 1e6 / args.fps
    estimated_drops = sum(max(0, round(float(row.get("capture_interval_us", 0)) /
                                       expected_interval_us) - 1)
                          for row in rows[1:])
    drop_rate = estimated_drops / max(1, estimated_drops + len(rows))
    rss_samples = [(int(row["timestamp_us"]) / 60e6, float(row["rss_kb"]))
                   for row in rows if float(row.get("rss_kb", -1)) >= 0]
    rss_slope = linear_slope([item[0] for item in rss_samples],
                             [item[1] for item in rss_samples]) if rss_samples else 0.0
    frame_delta = int(rows[-1].get("frame", len(rows) - 1)) - \
        int(rows[0].get("frame", 0))
    report = {
        "duration_min": duration_s / 60.0,
        "frames": len(rows),
        "frame_index_delta": frame_delta,
        "actual_fps": frame_delta / duration_s if duration_s > 0 else 0.0,
        "capture_interval_mean_ms": (statistics.mean(capture_intervals)
                                     if capture_intervals else None),
        "capture_interval_p95_ms": (percentile(capture_intervals, 0.95)
                                    if capture_intervals else None),
        "classical_p95_ms": (percentile(classical, 0.95)
                             if classical else None),
        "visual_motion_p95_ms": (percentile(visual_motion, 0.95)
                                 if visual_motion else None),
        "processing_p95_ms": percentile(processing, 0.95),
        "model_runs": len(model),
        "model_p95_ms": None if not model else percentile(model, 0.95),
        "estimated_dropped_frames": estimated_drops,
        "estimated_drop_rate": drop_rate,
        "rss_growth_kb_per_min": rss_slope,
    }
    failures = []
    if report["duration_min"] < args.min_duration_min:
        failures.append("duration is shorter than the required soak test")
    if report["actual_fps"] < args.min_actual_fps:
        failures.append("measured frame rate is below threshold")
    if report["processing_p95_ms"] > args.max_processing_p95_ms:
        failures.append("end-to-end processing P95 exceeds threshold")
    if report["model_p95_ms"] is None:
        if not args.allow_no_model:
            failures.append("no NPU model runs were recorded")
    elif report["model_p95_ms"] > args.max_model_p95_ms:
        failures.append("NPU model P95 exceeds threshold")
    if drop_rate >= args.max_drop_rate:
        failures.append("estimated dropped-frame rate exceeds threshold")
    if rss_slope > args.max_rss_growth_kb_per_min:
        failures.append("resident memory has a sustained positive slope")
    report["passed"] = not failures
    report["failures"] = failures
    rendered = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    print(rendered, end="")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
