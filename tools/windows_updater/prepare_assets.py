#!/usr/bin/env python3
from __future__ import annotations

import shutil
from pathlib import Path


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    build = repo / ".pio" / "build" / "adafruit_feather_esp32s3"
    source = build / "firmware.bin"
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
