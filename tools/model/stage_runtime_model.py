#!/usr/bin/env python3
"""Validate and stage a converted MUD/AXMODEL bundle for MaixCDK release."""

from __future__ import annotations

import argparse
import configparser
import shutil
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mud", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("models/runtime"))
    args = parser.parse_args()
    metadata = configparser.ConfigParser()
    metadata.read(args.mud, encoding="utf-8")
    if metadata.get("extra", "model_type", fallback="") != "yolo11" or \
            metadata.get("extra", "type", fallback="") != "pose":
        raise SystemExit("MUD must declare model_type=yolo11 and type=pose")
    source_dir = args.mud.parent
    references = [metadata.get("basic", key, fallback="")
                  for key in ("model_npu", "model_vnpu")]
    if any(not value for value in references):
        raise SystemExit("MUD must reference model_npu and model_vnpu")
    sources = [source_dir / value for value in references]
    missing = [str(path) for path in sources if not path.is_file()]
    if missing:
        raise SystemExit("missing AXMODEL file(s): " + ", ".join(missing))
    args.output.mkdir(parents=True, exist_ok=True)
    target_mud = args.output / "dart_target_pose.mud"
    mud_text = args.mud.read_text(encoding="utf-8")
    for key, source in zip(("model_npu", "model_vnpu"), sources):
        target_name = f"dart_target_pose_{'npu' if key == 'model_npu' else 'vnpu'}.axmodel"
        shutil.copy2(source, args.output / target_name)
        old_name = metadata.get("basic", key)
        mud_text = mud_text.replace(old_name, target_name)
    target_mud.write_text(mud_text, encoding="utf-8")
    print(f"staged {target_mud} and two AXMODEL files")


if __name__ == "__main__":
    main()
