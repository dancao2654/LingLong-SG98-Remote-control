#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
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
    return parser.parse_args()


def output_name_for_rid(rid: str) -> str:
    return "SG98-Updater.exe" if rid.startswith("win") else "SG98-Updater"


def archive_name_for_rid(rid: str) -> str:
    return f"SG98-Updater-{rid}.zip"


def vendor_homebrew_brotli(binary_path: Path, cwd: Path) -> None:
    if binary_path.suffix == ".exe":
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


def create_release_archive(publish_dir: Path, rid: str, cwd: Path) -> Path:
    archive_path = publish_dir.parent / archive_name_for_rid(rid)
    if archive_path.exists():
        archive_path.unlink()

    folder_name = f"SG98-Updater-{rid}"
    with tempfile.TemporaryDirectory(prefix="sg98-updater-archive-") as temp_dir_str:
        temp_dir = Path(temp_dir_str)
        stage_dir = temp_dir / folder_name
        shutil.copytree(publish_dir, stage_dir, symlinks=False, copy_function=shutil.copy)

        if os.name == "posix":
            run(["zip", "-qryX", str(archive_path), stage_dir.name], cwd=temp_dir)
        else:
            shutil.make_archive(str(archive_path.with_suffix("")), "zip", root_dir=temp_dir, base_dir=stage_dir.name)

    return archive_path


def main() -> int:
    args = parse_args()
    rid = args.rid

    root = Path(__file__).resolve().parent
    assets = root / "assets"
    required_assets = ["sg98-firmware.bin"]
    if rid.startswith("win"):
        required_assets.append("esptool.exe")

    missing = [name for name in required_assets if not (assets / name).exists()]
    if missing:
        raise FileNotFoundError(f"Missing updater assets: {', '.join(missing)}")

    project = root / "SG98Updater" / "SG98Updater.csproj"
    publish_dir = root / "publish" / rid
    publish_dir.mkdir(parents=True, exist_ok=True)

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
    output_path = publish_dir / output_name_for_rid(rid)
    vendor_homebrew_brotli(output_path, cwd=root)
    archive_path = create_release_archive(publish_dir, rid, cwd=root)
    print(f"[ok] {output_path}")
    print(f"[ok] {archive_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
