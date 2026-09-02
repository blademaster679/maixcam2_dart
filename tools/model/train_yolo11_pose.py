#!/usr/bin/env python3
"""Train/evaluate/export the one-class five-keypoint YOLO11n-Pose model."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load_ultralytics():
    try:
        from ultralytics import YOLO
    except ImportError as exc:
        raise SystemExit(
            "ultralytics is not installed; use a dedicated venv and run "
            "pip install -r tools/model/requirements.txt") from exc
    return YOLO


def parse_sizes(value: str) -> list[int]:
    sizes = [int(item) for item in value.split(",")]
    if any(size not in (192, 256, 320) for size in sizes):
        raise argparse.ArgumentTypeError("sizes must be selected from 192,256,320")
    return sizes


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    train = subparsers.add_parser("train")
    train.add_argument("--data", type=Path, required=True)
    train.add_argument("--base-model", default="yolo11n-pose.pt")
    train.add_argument("--imgsz", type=int, default=256, choices=(192, 256, 320))
    train.add_argument("--epochs", type=int, default=200)
    train.add_argument("--batch", type=int, default=32)
    train.add_argument("--device", default="0")
    train.add_argument("--project", default="runs/dart_pose")
    train.add_argument("--name", default="yolo11n_pose_256")
    evaluate = subparsers.add_parser("evaluate")
    evaluate.add_argument("--weights", type=Path, required=True)
    evaluate.add_argument("--data", type=Path, required=True)
    evaluate.add_argument("--sizes", type=parse_sizes, default=[192, 256, 320])
    evaluate.add_argument("--output", type=Path, required=True)
    evaluate.add_argument("--device", default="0")
    export = subparsers.add_parser("export")
    export.add_argument("--weights", type=Path, required=True)
    export.add_argument("--imgsz", type=int, default=256, choices=(192, 256, 320))
    export.add_argument("--opset", type=int, default=17)
    args = parser.parse_args()
    YOLO = load_ultralytics()
    if args.command == "train":
        model = YOLO(args.base_model)
        model.train(data=str(args.data), imgsz=args.imgsz, epochs=args.epochs,
                    batch=args.batch, device=args.device, project=args.project,
                    name=args.name, degrees=180.0, translate=0.10, scale=0.50,
                    hsv_h=0.08, hsv_s=0.55, hsv_v=0.45, fliplr=0.5,
                    mosaic=0.35, close_mosaic=20, plots=True)
    elif args.command == "evaluate":
        model = YOLO(str(args.weights))
        report = {}
        for size in args.sizes:
            metrics = model.val(data=str(args.data), imgsz=size,
                                device=args.device, plots=False)
            report[str(size)] = getattr(metrics, "results_dict", {})
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(args.output)
    else:
        model = YOLO(str(args.weights))
        path = model.export(format="onnx", imgsz=args.imgsz, dynamic=False,
                            simplify=True, opset=args.opset)
        print(path)


if __name__ == "__main__":
    main()
