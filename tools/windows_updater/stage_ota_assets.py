#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Stage SG98 OTA assets for a PlatformIO environment.")
    parser.add_argument(
        "--env",
        required=True,
        help="PlatformIO environment name, for example controller-universal-s3 or remote-universal-s3.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    env_name = args.env.strip()
    if not env_name:
        raise ValueError("--env cannot be empty")

    root = Path(__file__).resolve().parent
    assets = root / "assets"
    manifest_path = assets / "sg98-ota-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    app_name = manifest.get("application", {}).get("fileName")
    if not app_name:
        raise RuntimeError("OTA manifest is missing application.fileName")

    ota_dir = root / "publish" / "ota" / env_name
    if ota_dir.exists():
        shutil.rmtree(ota_dir)
    ota_dir.mkdir(parents=True, exist_ok=True)

    for name in ("sg98-ota-manifest.json", "sg98-flash-manifest.json", app_name):
        shutil.copy2(assets / name, ota_dir / name)
        print(f"[copied] {assets / name} -> {ota_dir / name}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
