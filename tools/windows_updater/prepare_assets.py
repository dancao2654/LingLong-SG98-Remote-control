#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

SEGMENTS = [
    {
        "offset": "0x0000",
        "source_name": "bootloader.bin",
        "file_name": "flash-00-0x0000-bootloader.bin",
        "kind": "bootloader",
    },
    {
        "offset": "0x8000",
        "source_name": "partitions.bin",
        "file_name": "flash-01-0x8000-partitions.bin",
        "kind": "partitions",
    },
    {
        "offset": "0xe000",
        "source_name": "boot_app0.bin",
        "file_name": "flash-02-0xe000-boot_app0.bin",
        "kind": "boot_app0",
    },
    {
        "offset": "0x10000",
        "source_name": "firmware.bin",
        "file_name": "flash-03-0x10000-firmware.bin",
        "kind": "application",
    },
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Copy a full ESP32 flash set into updater assets.")
    parser.add_argument(
        "--env",
        help="PlatformIO environment to package. Defaults to adafruit_feather_esp32s3 when no source is given.",
    )
    parser.add_argument(
        "--source",
        help="Explicit firmware.bin path to package instead of resolving it from a PlatformIO environment.",
    )
    return parser.parse_args()


def resolve_build_dir(repo: Path, args: argparse.Namespace) -> tuple[str, Path, Path]:
    if args.source:
        source = Path(args.source)
        if not source.is_absolute():
            source = repo / source
        env_name = args.env or source.parent.name
        return (env_name, source.parent, source)

    env_name = args.env or "adafruit_feather_esp32s3"
    build_dir = repo / ".pio" / "build" / env_name
    return (env_name, build_dir, build_dir / "firmware.bin")


def load_idedata(build_dir: Path) -> dict:
    idedata_path = build_dir / "idedata.json"
    if not idedata_path.exists():
        return {}
    return json.loads(idedata_path.read_text(encoding="utf-8"))


def normalize_offset(value: str) -> str:
    normalized = value.strip().lower()
    if not normalized.startswith("0x"):
        normalized = f"0x{normalized}"
    return normalized


def resolve_segment_sources(build_dir: Path, firmware_path: Path) -> list[dict]:
    idedata = load_idedata(build_dir)
    flash_images = {}
    for entry in idedata.get("extra", {}).get("flash_images", []):
        offset = entry.get("offset")
        path = entry.get("path")
        if offset and path:
            flash_images[normalize_offset(offset)] = Path(path)

    resolved = []
    for segment in SEGMENTS:
        offset = segment["offset"]
        if segment["kind"] == "application":
            source = firmware_path
        else:
            source = flash_images.get(offset, build_dir / segment["source_name"])

        resolved.append({**segment, "source_path": source})
    return resolved


def board_name_for_manifest(env_name: str, idedata: dict) -> str:
    return idedata.get("board_name") or idedata.get("env_name") or env_name


def write_manifest(assets: Path, env_name: str, board_name: str, segments: list[dict]) -> Path:
    manifest = {
        "envName": env_name,
        "boardName": board_name,
        "applicationOffset": "0x10000",
        "segments": [
            {
                "offset": segment["offset"],
                "fileName": segment["file_name"],
                "sourceName": segment["source_name"],
                "kind": segment["kind"],
            }
            for segment in segments
        ],
    }
    manifest_path = assets / "sg98-flash-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest_path


def clear_staged_assets(assets: Path) -> None:
    for stale in assets.glob("flash-*.bin"):
        stale.unlink()
    legacy_firmware = assets / "sg98-firmware.bin"
    if legacy_firmware.exists():
        legacy_firmware.unlink()
    manifest_path = assets / "sg98-flash-manifest.json"
    if manifest_path.exists():
        manifest_path.unlink()


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    args = parse_args()
    env_name, build_dir, firmware_path = resolve_build_dir(repo, args)

    assets = Path(__file__).resolve().parent / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    clear_staged_assets(assets)

    idedata = load_idedata(build_dir)
    board_name = board_name_for_manifest(env_name, idedata)
    segments = resolve_segment_sources(build_dir, firmware_path)

    for segment in segments:
        source = segment["source_path"]
        if not source.exists():
            raise FileNotFoundError(f"Missing flash segment: {source}")
        target = assets / segment["file_name"]
        shutil.copy2(source, target)
        print(f"[copied] {source} -> {target}")

    manifest_path = write_manifest(assets, env_name, board_name, segments)
    print(f"[wrote] {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
