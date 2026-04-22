#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


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
        help="Subdirectory under publish/ where artifacts should be written.",
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


def vendor_homebrew_brotli(binary_path: Path, rid: str, cwd: Path) -> None:
    if not rid.startswith("osx"):
        return

    if sys.platform != "darwin":
        print(f"[info] skipping Homebrew dylib patch for {rid} on non-macOS host")
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

    # Ad-hoc sign the patched binaries so Gatekeeper and dyld accept the edits.
    for target in [*copied, binary_path]:
        run(["codesign", "--force", "--sign", "-", str(target)], cwd=cwd)


def _zip_mode_for_path(path: Path, rid: str, output_base: str) -> int:
    if path.is_dir():
        return 0o755

    mode = stat.S_IMODE(path.stat().st_mode) or 0o644
    executable_name = output_name_for_rid(output_base, rid)
    if rid.startswith("osx") and (path.name == executable_name or path.suffix == ".dylib"):
        return 0o755
    return mode


def _write_zip_entry(archive: zipfile.ZipFile, path: Path, arcname: str, rid: str, output_base: str) -> None:
    if path.is_dir():
        directory_name = arcname if arcname.endswith("/") else arcname + "/"
        info = zipfile.ZipInfo.from_file(path, directory_name)
        info.create_system = 3
        info.external_attr = ((_zip_mode_for_path(path, rid, output_base) | stat.S_IFDIR) << 16) | 0x10
        info.compress_type = zipfile.ZIP_STORED
        archive.writestr(info, b"")
        return

    info = zipfile.ZipInfo.from_file(path, arcname)
    info.create_system = 3
    info.external_attr = (_zip_mode_for_path(path, rid, output_base) | stat.S_IFREG) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    with path.open("rb") as src, archive.open(info, "w") as dest:
        shutil.copyfileobj(src, dest)


def write_md5_file(target_path: Path) -> Path:
    digest = hashlib.md5()
    with target_path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)

    md5_path = target_path.parent / f"{target_path.name}.md5"
    md5_path.write_text(f"{digest.hexdigest()}  {target_path.name}\n", encoding="utf-8")
    return md5_path


def create_release_archive(bundle_root: Path, publish_dir: Path, output_base: str, rid: str, cwd: Path) -> Path:
    archive_path = bundle_root / archive_name_for_rid(output_base, rid)
    if archive_path.exists():
        archive_path.unlink()

    folder_name = f"{output_base}-{rid}"
    with tempfile.TemporaryDirectory(prefix="sg98-updater-archive-") as temp_dir_str:
        temp_dir = Path(temp_dir_str)
        stage_dir = temp_dir / folder_name
        shutil.copytree(publish_dir, stage_dir, symlinks=False, copy_function=shutil.copy)
        paths = [stage_dir, *sorted(stage_dir.rglob("*"))]
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in paths:
                arcname = path.relative_to(temp_dir).as_posix()
                _write_zip_entry(archive, path, arcname, rid, output_base)

    return archive_path


def main() -> int:
    args = parse_args()
    rid = args.rid
    output_base = args.output_base
    publish_subdir = args.publish_subdir.strip().strip("/").strip("\\")
    bundle_subdir = args.bundle_subdir.strip().strip("/").strip("\\")

    root = Path(__file__).resolve().parent
    assets = root / "assets"
    required_assets = ["sg98-flash-manifest.json"]
    if rid.startswith("win"):
        required_assets.append("esptool.exe")

    missing = [name for name in required_assets if not (assets / name).exists()]
    if missing:
        raise FileNotFoundError(f"Missing updater assets: {', '.join(missing)}")
    if not list(assets.glob("flash-*.bin")):
        raise FileNotFoundError(f"No flash image assets found in {assets}")

    project = root / "SG98Updater" / "SG98Updater.csproj"
    publish_root = root / "publish"
    if publish_subdir:
        publish_root = publish_root / publish_subdir
    publish_dir = publish_root / rid
    if publish_dir.exists():
        shutil.rmtree(publish_dir)
    publish_dir.mkdir(parents=True, exist_ok=True)

    bundle_root = root / "publish"
    if bundle_subdir:
        bundle_root = bundle_root / bundle_subdir
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
    vendor_homebrew_brotli(output_path, rid=rid, cwd=root)
    archive_path = create_release_archive(bundle_root, publish_dir, output_base, rid, cwd=root)
    md5_path = write_md5_file(archive_path)
    print(f"[ok] {output_path}")
    print(f"[ok] {archive_path}")
    print(f"[ok] {md5_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
