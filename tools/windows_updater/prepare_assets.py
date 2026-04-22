#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Copy a firmware image into updater assets.")
    parser.add_argument(
        "--env",
        help="PlatformIO environment to package. Defaults to adafruit_feather_esp32s3 when no source is given.",
    )
    parser.add_argument(
        "--source",
        help="Explicit firmware image to package instead of resolving from a PlatformIO environment.",
    )
    return parser.parse_args()


def resolve_source(repo: Path, args: argparse.Namespace) -> Path:
    if args.source:
        source = Path(args.source)
        if not source.is_absolute():
            source = repo / source
        return source

    env_name = args.env or "adafruit_feather_esp32s3"
    return repo / ".pio" / "build" / env_name / "firmware.bin"


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    args = parse_args()
    source = resolve_source(repo, args)

    if not source.exists():
        raise FileNotFoundError(f"Missing firmware image: {source}")

    assets = Path(__file__).resolve().parent / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    target = assets / "sg98-firmware.bin"
    shutil.copy2(source, target)
    print(f"[copied] {source} -> {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
