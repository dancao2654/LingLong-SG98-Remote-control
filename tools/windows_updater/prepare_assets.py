#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Copy the exact flash image set for a built PlatformIO environment into updater assets."
    )
    parser.add_argument(
        "--env",
        help="PlatformIO environment name, for example adafruit_feather_esp32s3 or waveshare_esp32_s3_rs485_can",
    )
    parser.add_argument(
        "--build-dir",
        help="Explicit .pio build directory to read instead of resolving from --env",
    )
    args = parser.parse_args()
    if not args.env and not args.build_dir:
        parser.error("pass --env or --build-dir so the updater knows which controller image set to bundle")
    return args


def _candidate_build_dirs(build_dir: Path, repo: Path) -> list[Path]:
    candidates = [build_dir.resolve()]
    for candidate in (
        repo / ".pio" / "build" / build_dir.name,
        repo / "sg98-remote" / ".pio" / "build" / build_dir.name,
    ):
        resolved = candidate.resolve()
        if resolved not in candidates:
            candidates.append(resolved)
    return candidates


def _load_idedata(build_dir: Path, repo: Path) -> dict:
    searched: list[Path] = []
    for candidate_build in _candidate_build_dirs(build_dir, repo):
        idedata_path = candidate_build / "idedata.json"
        searched.append(idedata_path)
        if idedata_path.exists():
            return json.loads(idedata_path.read_text(encoding="utf-8"))
    searched_text = ", ".join(str(path) for path in searched)
    raise FileNotFoundError(f"Missing PlatformIO metadata. Searched: {searched_text}")


def _parse_offset(value: str) -> int:
    return int(value, 16)


def _safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("._") or "image"


def _kind_for_path(path: Path, is_application: bool) -> str:
    if is_application:
        return "application"

    name = path.name.lower()
    if "bootloader" in name:
        return "bootloader"
    if "partition" in name:
        return "partitions"
    if "boot_app0" in name:
        return "boot_app0"
    return "extra"


def _resolve_source_path(source: Path, build_dir: Path, repo: Path) -> Path:
    candidates = [source]
    for candidate_build in _candidate_build_dirs(build_dir, repo):
        remapped = candidate_build / source.name
        if remapped not in candidates:
            candidates.append(remapped)

    for candidate in candidates:
        if candidate.exists():
            return candidate

    tried = ", ".join(str(path) for path in candidates)
    raise FileNotFoundError(f"Missing source image. Searched: {tried}")


def main() -> int:
    args = parse_args()
    repo = Path(__file__).resolve().parents[2]
    build = Path(args.build_dir).resolve() if args.build_dir else repo / ".pio" / "build" / args.env
    idedata = _load_idedata(build, repo)

    try:
        firmware = _resolve_source_path(build / "firmware.bin", build, repo)
    except FileNotFoundError:
        prog_path = Path(idedata["prog_path"])
        firmware = _resolve_source_path(prog_path.with_suffix(".bin"), build, repo)

    flash_images = list(idedata.get("extra", {}).get("flash_images", []))
    application_offset = idedata.get("extra", {}).get("application_offset")
    if not application_offset:
        raise KeyError(f"Missing application_offset in {build / 'idedata.json'}")

    segments: list[dict[str, str]] = []
    for image in flash_images:
        source = _resolve_source_path(Path(image["path"]), build, repo)
        segments.append(
            {
                "offset": image["offset"],
                "source": str(source),
                "kind": _kind_for_path(source, is_application=False),
            }
        )

    segments.append(
        {
            "offset": application_offset,
            "source": str(firmware),
            "kind": "application",
        }
    )
    segments.sort(key=lambda item: _parse_offset(item["offset"]))

    assets = Path(__file__).resolve().parent / "assets"
    assets.mkdir(parents=True, exist_ok=True)

    for stale in assets.glob("flash-*.bin"):
        stale.unlink()

    legacy_app = assets / "sg98-firmware.bin"
    if legacy_app.exists():
        legacy_app.unlink()

    manifest_path = assets / "sg98-flash-manifest.json"
    if manifest_path.exists():
        manifest_path.unlink()

    manifest_segments: list[dict[str, str]] = []
    for index, segment in enumerate(segments):
        source = Path(segment["source"])
        file_name = f"flash-{index:02d}-{segment['offset'].lower()}-{_safe_name(source.name)}"
        target = assets / file_name
        shutil.copy2(source, target)
        manifest_segments.append(
            {
                "offset": segment["offset"],
                "fileName": file_name,
                "sourceName": source.name,
                "kind": segment["kind"],
            }
        )
        print(f"[copied] {source} -> {target}")

    defines = idedata.get("defines", [])
    board_name = next(
        (define.split("=", 1)[1].strip('"') for define in defines if define.startswith("ARDUINO_BOARD=")),
        idedata.get("env_name", "unknown"),
    )
    manifest = {
        "envName": idedata.get("env_name", "unknown"),
        "boardName": board_name,
        "applicationOffset": application_offset,
        "segments": manifest_segments,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"[wrote] {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
