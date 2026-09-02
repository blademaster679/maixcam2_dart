#!/usr/bin/env python3
"""Replay a video set at deployment resolution and build a v0.2 report."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--replay", type=Path,
                        default=Path("build/video-replay/dart_video_replay"))
    parser.add_argument("--config", type=Path, default=Path("config/green_detector.conf"))
    parser.add_argument("--annotations", type=Path)
    parser.add_argument("--ids", default="0,1,2,3,4,5,6,7,9")
    parser.add_argument("--max-frames", type=int, default=0)
    args = parser.parse_args()
    if not args.replay.is_file():
        raise SystemExit(f"replay executable not found: {args.replay}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    ids = [item.strip() for item in args.ids.split(",") if item.strip()]
    jsonl_paths = []
    names = []
    summaries = []
    for video_id in ids:
        video = args.input_dir / f"{video_id}.mp4"
        if not video.is_file():
            raise SystemExit(f"missing input video: {video}")
        output_video = args.output_dir / f"{video_id}_detected_v0.2.mp4"
        output_jsonl = args.output_dir / f"{video_id}_detected_v0.2.jsonl"
        command = [str(args.replay), "--input", str(video), "--output",
                   str(output_video), "--jsonl", str(output_jsonl),
                   "--config", str(args.config)]
        if args.max_frames:
            command += ["--max-frames", str(args.max_frames)]
        completed = subprocess.run(command, check=True, text=True,
                                   stdout=subprocess.PIPE)
        summary = json.loads(completed.stdout.strip().splitlines()[-1])
        summaries.append(summary)
        jsonl_paths.append(output_jsonl)
        names.append(video.name)
        print(f"{video.name}: {summary['frames']} frames, "
              f"safe={summary['safe_rate']:.2%}, "
              f"host={summary['average_detector_fps']:.1f} FPS")
    (args.output_dir / "replay_summaries.json").write_text(
        json.dumps(summaries, indent=2) + "\n", encoding="utf-8")
    evaluator = Path(__file__).with_name("evaluate_replay.py")
    command = [sys.executable, str(evaluator), *map(str, jsonl_paths)]
    for name in names:
        command += ["--video-name", name]
    command += ["--output-json", str(args.output_dir / "report_v0.2.json"),
                "--output-markdown", str(args.output_dir / "REPORT_v0.2.md")]
    if args.annotations:
        command += ["--annotations", str(args.annotations)]
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
