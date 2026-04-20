#!/usr/bin/env python3
from __future__ import annotations

import io
import sys
import zipfile
from pathlib import Path
from urllib.request import urlopen


ESPTOOL_VERSION = "5.2.0"
ESPTOOL_URL = f"https://github.com/espressif/esptool/releases/download/v{ESPTOOL_VERSION}/esptool-v{ESPTOOL_VERSION}-windows-amd64.zip"


def main() -> int:
    root = Path(__file__).resolve().parent
    assets = root / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    target = assets / "esptool.exe"

    if target.exists():
        print(f"[ok] {target}")
        return 0

    print(f"[download] {ESPTOOL_URL}")
    with urlopen(ESPTOOL_URL) as response:
        data = response.read()

    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        member = next((name for name in archive.namelist() if name.endswith("/esptool.exe") or name == "esptool.exe"), None)
        if member is None:
            raise RuntimeError("esptool.exe not found in downloaded release archive")
        with archive.open(member) as src, target.open("wb") as dst:
            dst.write(src.read())

    print(f"[saved] {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
