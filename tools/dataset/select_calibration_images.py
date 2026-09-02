#!/usr/bin/env python3
"""Select a deterministic, stratified INT8 calibration image set."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from collections import defaultdict
from pathlib import Path


def distance_bucket(value: object) -> str:
    if value is None:
        return "unknown"
    distance = float(value)
    for boundary in (7.5, 12.5, 17.5, 22.5):
        if distance < boundary:
            return f"lt_{boundary:g}"
    return "ge_22.5"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--dataset-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--seed", default="dart-int8-v0.2")
    args = parser.parse_args()
    rows = [json.loads(line) for line in
            args.manifest.read_text(encoding="utf-8").splitlines() if line.strip()]
    if args.count <= 0 or len(rows) < args.count:
        raise SystemExit(f"manifest has {len(rows)} rows; cannot select {args.count}")
    buckets: dict[tuple[str, ...], list[dict]] = defaultdict(list)
    for row in rows:
        key = (distance_bucket(row.get("distance_m")),
               str(row.get("color", "unknown")),
               str(row.get("blur", "unknown")),
               str(row.get("venue", "unknown")))
        buckets[key].append(row)
    for key, items in buckets.items():
        items.sort(key=lambda item: hashlib.sha256(
            f"{args.seed}:{key}:{item.get('image')}".encode()).digest())
    chosen: list[dict] = []
    ordered_keys = sorted(buckets)
    while len(chosen) < args.count:
        progressed = False
        for key in ordered_keys:
            if buckets[key] and len(chosen) < args.count:
                chosen.append(buckets[key].pop())
                progressed = True
        if not progressed:
            break
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = args.output / "calibration_manifest.jsonl"
    with manifest.open("w", encoding="utf-8") as stream:
        for index, row in enumerate(chosen):
            source = args.dataset_root / row["image"]
            destination = args.output / f"{index:03d}_{source.name}"
            shutil.copy2(source, destination)
            item = dict(row)
            item["calibration_image"] = destination.name
            stream.write(json.dumps(item, ensure_ascii=False) + "\n")
    print(f"selected {len(chosen)} images in {args.output}")


if __name__ == "__main__":
    main()
