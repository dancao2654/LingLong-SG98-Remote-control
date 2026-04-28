#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from sg98_version import APP_ONLY_UPDATE_SINCE, metadata  # noqa: E402

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
        help="PlatformIO environment to package. Defaults to controller-universal-s3 when no source is given.",
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

    env_name = args.env or "controller-universal-s3"
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


def platformio_package_dirs() -> list[Path]:
    roots = []
    packages_dir = os.environ.get("PLATFORMIO_PACKAGES_DIR")
    if packages_dir:
        roots.append(Path(packages_dir))
    roots.append(Path.home() / ".platformio" / "packages")

    resolved = []
    for root in roots:
        if root.exists() and root not in resolved:
            resolved.append(root)
    return resolved


def resolve_boot_app0_path() -> Path | None:
    for packages_dir in platformio_package_dirs():
        direct = packages_dir / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"
        if direct.exists():
            return direct

        for framework_dir in sorted(packages_dir.glob("framework-arduinoespressif32*")):
            candidate = framework_dir / "tools" / "partitions" / "boot_app0.bin"
            if candidate.exists():
                return candidate

    return None


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
        elif segment["kind"] == "boot_app0":
            source = flash_images.get(offset, build_dir / segment["source_name"])
            if not source.exists():
                source = resolve_boot_app0_path() or source
        else:
            source = flash_images.get(offset, build_dir / segment["source_name"])

        resolved.append({**segment, "source_path": source})
    return resolved


def board_name_for_manifest(env_name: str, idedata: dict) -> str:
    return idedata.get("board_name") or idedata.get("env_name") or env_name


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def update_default_mode() -> str:
    mode = os.environ.get("SG98_UPDATE_DEFAULT_MODE", "application").strip().lower()
    if mode not in ("application", "full"):
        raise ValueError("SG98_UPDATE_DEFAULT_MODE must be 'application' or 'full'")
    return mode


def app_only_since_version() -> str:
    return os.environ.get("SG98_APP_ONLY_SINCE", APP_ONLY_UPDATE_SINCE).strip() or APP_ONLY_UPDATE_SINCE


def write_manifest(assets: Path, build_info: dict, env_name: str, board_name: str, segments: list[dict]) -> Path:
    application = next((segment for segment in segments if segment["kind"] == "application"), None)
    default_mode = update_default_mode()
    manifest = {
        "schema": "sg98-flash-manifest-v2",
        "version": build_info["version"],
        "gitSha": build_info["gitSha"],
        "gitDirty": build_info["gitDirty"],
        "role": build_info["role"],
        "envName": env_name,
        "boardName": board_name,
        "applicationOffset": "0x10000",
        "generatedAtUtc": build_info["generatedAtUtc"],
        "updatePolicy": {
            "schema": "sg98-update-policy-v1",
            "defaultMode": default_mode,
            "appOnlySince": app_only_since_version(),
            "applicationOffset": "0x10000",
            "requiresDeviceCapability": "supportsAppOnlyUpdate",
            "fallbackMode": "full",
            "fallbackReason": "Use full flash when the running firmware cannot report SG98 app-only capability, the role/env is incompatible, or the board is in recovery.",
        },
        "application": {
            "offset": application["offset"] if application else "0x10000",
            "fileName": application["file_name"] if application else "flash-03-0x10000-firmware.bin",
            "sha256": sha256_file(assets / application["file_name"]) if application else "",
            "size": (assets / application["file_name"]).stat().st_size if application else 0,
        },
        "segments": [
            {
                "offset": segment["offset"],
                "fileName": segment["file_name"],
                "sourceName": segment["source_name"],
                "kind": segment["kind"],
                "sha256": sha256_file(assets / segment["file_name"]),
                "size": (assets / segment["file_name"]).stat().st_size,
            }
            for segment in segments
        ],
    }
    manifest_path = assets / "sg98-flash-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest_path


def write_ota_manifest(assets: Path, build_info: dict, env_name: str, board_name: str, segments: list[dict]) -> Path:
    application = next((segment for segment in segments if segment["kind"] == "application"), None)
    if application is None:
        raise ValueError("Cannot write OTA manifest without an application segment")

    app_path = assets / application["file_name"]
    manifest = {
        "schema": "sg98-ota-manifest-v1",
        "version": build_info["version"],
        "gitSha": build_info["gitSha"],
        "gitDirty": build_info["gitDirty"],
        "role": build_info["role"],
        "envName": env_name,
        "boardName": board_name,
        "generatedAtUtc": build_info["generatedAtUtc"],
        "application": {
            "offset": application["offset"],
            "fileName": application["file_name"],
            "sha256": sha256_file(app_path),
            "size": app_path.stat().st_size,
        },
    }
    manifest_path = assets / "sg98-ota-manifest.json"
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
    ota_manifest_path = assets / "sg98-ota-manifest.json"
    if ota_manifest_path.exists():
        ota_manifest_path.unlink()


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
    build_info = metadata(repo, env_name)

    for segment in segments:
        source = segment["source_path"]
        if not source.exists():
            raise FileNotFoundError(f"Missing flash segment: {source}")
        target = assets / segment["file_name"]
        shutil.copy2(source, target)
        print(f"[copied] {source} -> {target}")

    manifest_path = write_manifest(assets, build_info, env_name, board_name, segments)
    print(f"[wrote] {manifest_path}")
    ota_manifest_path = write_ota_manifest(assets, build_info, env_name, board_name, segments)
    print(f"[wrote] {ota_manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
