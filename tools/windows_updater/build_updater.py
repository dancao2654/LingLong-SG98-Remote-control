#!/usr/bin/env python3
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
import sys


def run(cmd: list[str], cwd: Path) -> None:
    print("[run]", " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def read_stdout(cmd: list[str], cwd: Path) -> str:
    print("[run]", " ".join(cmd))
    result = subprocess.run(cmd, cwd=cwd, check=True, text=True, capture_output=True)
    return result.stdout


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Publish the SG98 updater for a specific runtime.")
    parser.add_argument(
        "--rid",
        default="win-x64",
        help="dotnet runtime identifier to publish for, for example win-x64 or osx-arm64",
    )
    parser.add_argument(
        "--output-base",
        default="SG98-Updater",
        help="Base filename for the generated updater, without runtime suffix.",
    )
    parser.add_argument(
        "--publish-subdir",
        default="controller",
        help="Subdirectory under publish/ where unpacked runtime outputs should be written.",
    )
    parser.add_argument(
        "--bundle-subdir",
        default="bundles",
        help="Subdirectory under publish/ where zip bundles should be written.",
    )
    return parser.parse_args()


def output_name_for_rid(output_base: str, rid: str) -> str:
    return f"{output_base}.exe" if rid.startswith("win") else output_base


def archive_name_for_rid(output_base: str, rid: str) -> str:
    return f"{output_base}-{rid}.zip"


def rename_published_outputs(publish_dir: Path, rid: str, output_base: str) -> Path:
    default_base = "SG98-Updater"
    default_output = publish_dir / output_name_for_rid(default_base, rid)
    desired_output = publish_dir / output_name_for_rid(output_base, rid)

    if default_output != desired_output and default_output.exists():
        if desired_output.exists():
            desired_output.unlink()
        default_output.rename(desired_output)

    default_pdb = publish_dir / f"{default_base}.pdb"
    desired_pdb = publish_dir / f"{output_base}.pdb"
    if default_pdb != desired_pdb and default_pdb.exists():
        if desired_pdb.exists():
            desired_pdb.unlink()
        default_pdb.rename(desired_pdb)

    return desired_output


def vendor_homebrew_brotli(binary_path: Path, cwd: Path) -> None:
    if binary_path.suffix == ".exe" or sys.platform != "darwin":
        return

    otool_output = read_stdout(["otool", "-L", str(binary_path)], cwd=cwd)
    required = {
        "/opt/homebrew/opt/brotli/lib/libbrotlidec.1.dylib": "libbrotlidec.1.dylib",
        "/opt/homebrew/opt/brotli/lib/libbrotlienc.1.dylib": "libbrotlienc.1.dylib",
    }

    linked = {src: name for src, name in required.items() if src in otool_output}
    if not linked:
        return

    common_src = Path("/opt/homebrew/opt/brotli/lib/libbrotlicommon.1.dylib")
    if not common_src.exists():
        raise FileNotFoundError("Expected Homebrew brotli common library was not found")

    copied: list[Path] = []
    for src, name in linked.items():
        src_path = Path(src)
        if not src_path.exists():
            raise FileNotFoundError(f"Expected Homebrew brotli library was not found: {src}")
        dest = binary_path.parent / name
        if dest.exists():
            dest.unlink()
        shutil.copy2(src_path.resolve(), dest)
        copied.append(dest)

    common_dest = binary_path.parent / common_src.name
    if common_dest.exists():
        common_dest.unlink()
    shutil.copy2(common_src.resolve(), common_dest)
    copied.append(common_dest)

    for dylib in copied:
        run(["install_name_tool", "-id", f"@executable_path/{dylib.name}", str(dylib)], cwd=cwd)

    for dylib_name in ("libbrotlidec.1.dylib", "libbrotlienc.1.dylib"):
        dylib_path = binary_path.parent / dylib_name
        run(
            [
                "install_name_tool",
                "-change",
                "@rpath/libbrotlicommon.1.dylib",
                "@executable_path/libbrotlicommon.1.dylib",
                str(dylib_path),
            ],
            cwd=cwd,
        )

    for src, name in linked.items():
        run(
            [
                "install_name_tool",
                "-change",
                src,
                f"@executable_path/{name}",
                str(binary_path),
            ],
            cwd=cwd,
        )

    for target in [*copied, binary_path]:
        run(["codesign", "--force", "--sign", "-", str(target)], cwd=cwd)


def create_release_archive(bundle_root: Path, publish_dir: Path, output_base: str, rid: str, cwd: Path) -> Path:
    archive_path = bundle_root / archive_name_for_rid(output_base, rid)
    if archive_path.exists():
        archive_path.unlink()

    folder_name = f"{output_base}-{rid}"
    with tempfile.TemporaryDirectory(prefix="sg98-updater-archive-") as temp_dir_str:
        temp_dir = Path(temp_dir_str)
        stage_dir = temp_dir / folder_name
        shutil.copytree(publish_dir, stage_dir, symlinks=False, copy_function=shutil.copy)

        if os.name == "posix":
            run(["zip", "-qryX", str(archive_path), stage_dir.name], cwd=temp_dir)
        else:
            shutil.make_archive(str(archive_path.with_suffix("")), "zip", root_dir=temp_dir, base_dir=stage_dir.name)

    return archive_path


def write_md5_file(archive_path: Path) -> Path:
    digest = hashlib.md5(archive_path.read_bytes()).hexdigest()
    md5_path = archive_path.with_name(f"{archive_path.name}.md5")
    md5_path.write_text(f"{digest}  {archive_path.name}\n", encoding="ascii")
    return md5_path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def update_release_manifest(bundle_root: Path, archive_path: Path, md5_path: Path, rid: str, manifest: dict) -> Path:
    release_manifest_path = bundle_root / "sg98-release-manifest.json"
    if release_manifest_path.exists():
        release_manifest = json.loads(release_manifest_path.read_text(encoding="utf-8"))
    else:
        release_manifest = {
            "schema": "sg98-public-release-manifest-v1",
            "artifacts": [],
        }

    artifact = {
        "fileName": archive_path.name,
        "md5FileName": md5_path.name,
        "runtime": rid,
        "role": manifest.get("role"),
        "envName": manifest.get("envName"),
        "boardName": manifest.get("boardName"),
        "version": manifest.get("version"),
        "gitSha": manifest.get("gitSha"),
        "gitDirty": manifest.get("gitDirty", False),
        "size": archive_path.stat().st_size,
        "sha256": sha256_file(archive_path),
        "md5": md5_path.read_text(encoding="ascii").split()[0],
        "application": manifest.get("application"),
        "updatePolicy": manifest.get("updatePolicy"),
    }

    artifacts = [
        item for item in release_manifest.get("artifacts", [])
        if item.get("fileName") != archive_path.name
    ]
    artifacts.append(artifact)
    artifacts.sort(key=lambda item: (str(item.get("role") or ""), str(item.get("runtime") or ""), str(item.get("fileName") or "")))

    release_manifest.update(
        {
            "schema": "sg98-public-release-manifest-v1",
            "generatedAtUtc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "version": manifest.get("version"),
            "gitSha": manifest.get("gitSha"),
            "artifacts": artifacts,
        }
    )
    release_manifest_path.write_text(json.dumps(release_manifest, indent=2) + "\n", encoding="utf-8")
    return release_manifest_path


def load_manifest(assets: Path) -> dict:
    manifest_path = assets / "sg98-flash-manifest.json"
    if not manifest_path.exists():
        raise FileNotFoundError(f"Missing updater asset: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    segments = manifest.get("segments") or []
    if not segments:
        raise ValueError("Flash manifest does not list any segments")
    return manifest


def main() -> int:
    args = parse_args()
    rid = args.rid
    output_base = args.output_base.strip() or "SG98-Updater"
    publish_subdir = args.publish_subdir.strip().strip("/\\")
    bundle_subdir = args.bundle_subdir.strip().strip("/\\")

    root = Path(__file__).resolve().parent
    assets = root / "assets"
    manifest = load_manifest(assets)
    required_assets = [
        "sg98-flash-manifest.json",
        "sg98-ota-manifest.json",
        *[segment["fileName"] for segment in manifest["segments"]],
    ]
    if rid.startswith("win"):
        required_assets.append("esptool.exe")

    missing = [name for name in required_assets if not (assets / name).exists()]
    if missing:
        raise FileNotFoundError(f"Missing updater assets: {', '.join(missing)}")

    project = root / "SG98Updater" / "SG98Updater.csproj"

    publish_root = root / "publish"
    if publish_subdir:
        publish_root /= publish_subdir
    publish_dir = publish_root / rid
    if publish_dir.exists():
        shutil.rmtree(publish_dir)
    publish_dir.mkdir(parents=True, exist_ok=True)

    bundle_root = root / "publish"
    if bundle_subdir:
        bundle_root /= bundle_subdir
    bundle_root.mkdir(parents=True, exist_ok=True)

    run(
        [
            "dotnet",
            "publish",
            str(project),
            "-c",
            "Release",
            "-r",
            rid,
            "--self-contained",
            "true",
            "-p:PublishSingleFile=true",
            "-p:PublishTrimmed=false",
            "-p:IncludeNativeLibrariesForSelfExtract=true",
            "-o",
            str(publish_dir),
        ],
        cwd=root,
    )
    output_path = rename_published_outputs(publish_dir, rid, output_base)
    vendor_homebrew_brotli(output_path, cwd=root)
    archive_path = create_release_archive(bundle_root, publish_dir, output_base, rid, cwd=root)
    md5_path = write_md5_file(archive_path)
    release_manifest_path = update_release_manifest(bundle_root, archive_path, md5_path, rid, manifest)
    print(f"[ok] {output_path}")
    print(f"[ok] {archive_path}")
    print(f"[ok] {md5_path}")
    print(f"[ok] {release_manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
