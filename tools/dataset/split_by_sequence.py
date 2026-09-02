#!/usr/bin/env python3
"""Split an ROI manifest by complete sequence, locking one venue as test."""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import defaultdict
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--test-venue", required=True)
    parser.add_argument("--validation-fraction", type=float, default=0.2)
    parser.add_argument("--seed", default="dart-v0.2")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not 0.0 < args.validation_fraction < 1.0:
        raise SystemExit("--validation-fraction must be between zero and one")
    records = [json.loads(line) for line in
               args.manifest.read_text(encoding="utf-8").splitlines() if line.strip()]
    groups: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for record in records:
        venue = str(record.get("venue", ""))
        sequence = str(record.get("sequence", record.get("video", "")))
        if not venue or not sequence:
            raise SystemExit("every manifest row must contain venue and sequence/video")
        groups[(venue, sequence)].append(record)

    result: dict[str, list[dict]] = {"train": [], "val": [], "test": []}
    for (venue, sequence), items in groups.items():
        if venue == args.test_venue:
            split = "test"
        else:
            digest = hashlib.sha256(f"{args.seed}:{venue}:{sequence}".encode()).digest()
            fraction = int.from_bytes(digest[:8], "big") / float(1 << 64)
            split = "val" if fraction < args.validation_fraction else "train"
        for item in items:
            item = dict(item)
            item["split"] = split
            result[split].append(item)

    if not result["test"]:
        raise SystemExit(f"locked test venue {args.test_venue!r} has no samples")
    args.output.mkdir(parents=True, exist_ok=True)
    sequence_sets: dict[str, set[str]] = {}
    for split, items in result.items():
        path = args.output / f"{split}.jsonl"
        path.write_text("".join(json.dumps(item, ensure_ascii=False) + "\n"
                                for item in items), encoding="utf-8")
        sequence_sets[split] = {str(item["sequence"]) for item in items}
        print(f"{split}: {len(items)} samples, {len(sequence_sets[split])} sequences")
    if any(sequence_sets[a] & sequence_sets[b]
           for a, b in (("train", "val"), ("train", "test"), ("val", "test"))):
        raise RuntimeError("sequence leakage detected after split")


if __name__ == "__main__":
    main()
