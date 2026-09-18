#!/usr/bin/env python3
"""Pull completed offline clips without deleting the board copies."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path,
                        default=Path("recordings/maixcam2/full180_offline"))
    parser.add_argument("--clip", action="append",
                        help="clip directory name; default pulls every completed clip")
    parser.add_argument("--skill", type=Path,
                        default=Path("/home/blade_master/pnx/maixpy-skill/maixpy"))
    args = parser.parse_args()

    sys.path.insert(0, str(args.skill.resolve() / "scripts"))
    from maixpy_skill.config import get_device
    from maixpy_skill.ssh import command_env, run_ssh, scp_from_command

    device = get_device()
    result = run_ssh(device,
        "for d in /root/dart_recordings/clip_*; do "
        "test -d \"$d\" && test -f \"$d/.complete\" && basename \"$d\"; done",
        timeout=20)
    if result.returncode:
        print(result.stderr, file=sys.stderr, end="")
        return result.returncode
    available = sorted(line.strip() for line in result.stdout.splitlines() if line.strip())
    selected = args.clip or available
    for name in selected:
        if not re.fullmatch(r"clip_[0-9]{6}_[A-Za-z0-9_-]+", name) or name not in available:
            raise SystemExit(f"clip is absent, incomplete, or invalid: {name}")

    args.output.mkdir(parents=True, exist_ok=True)
    for name in selected:
        destination = args.output / name
        if destination.exists():
            print(f"skip existing {destination}")
            continue
        remote_dir = f"/root/dart_recordings/{name}"
        hashes = run_ssh(
            device,
            f"cd {shlex.quote(remote_dir)} && "
            "find . -maxdepth 1 -type f -exec sha256sum '{}' ';' | sort",
            timeout=30,
        )
        if hashes.returncode:
            print(hashes.stderr, file=sys.stderr, end="")
            return hashes.returncode
        remote_hashes = {}
        for line in hashes.stdout.splitlines():
            digest, relative = line.split(maxsplit=1)
            relative = relative.removeprefix("./")
            if not re.fullmatch(r"[A-Za-z0-9_.-]+", relative):
                raise SystemExit(f"unexpected remote filename: {relative}")
            remote_hashes[relative] = digest
        command = scp_from_command(device, f"/root/dart_recordings/{name}",
                                   str(args.output.resolve()))
        command.insert(command.index("scp") + 1, "-r")
        copied = subprocess.run(command, env=command_env(device), check=False)
        if copied.returncode:
            return copied.returncode
        required = [destination / "record.h264", destination / "frames.csv",
                    destination / "capture.json", destination / ".complete"]
        if any(not path.is_file() for path in required):
            raise SystemExit(f"incomplete local transfer: {destination}")
        capture = json.loads((destination / "capture.json").read_text())
        if capture.get("exit_code") != 0 or capture.get("encoded_packets") != capture.get("frames"):
            raise SystemExit(f"capture integrity failed: {destination}")
        missing = int(capture.get("sequence_missing", 0))
        mipi_errors = int(capture.get("mipi_errors_max", 0))
        if missing or mipi_errors:
            print(f"warning: {name} has {missing} missing source frames and "
                  f"{mipi_errors} recovered MIPI errors; "
                  "keep for training, exclude from lossless performance acceptance",
                  file=sys.stderr)
        local_hashes = {path.name: file_hash(path) for path in destination.iterdir()
                        if path.is_file()}
        if local_hashes != remote_hashes:
            raise SystemExit(f"remote/local hash mismatch: {destination}")
        manifest = {
            "clip": name,
            "files": remote_hashes,
        }
        (destination / "pull_manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        print(destination)
    if not selected:
        print("no completed clips found")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
