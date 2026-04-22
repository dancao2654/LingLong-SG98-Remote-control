# SG98 Desktop Updater

This folder builds self-contained updater executables for SG98.

The updater bundles:

- the exact SG98 flash image set for the selected controller build
- the updater app itself
- and, on Windows, Espressif's official `esptool.exe`

The end-user flow is:

1. Connect the box to the computer over USB-C.
2. Keep the main `+24V` supply disconnected during flashing.
3. Run the updater.
4. Pick the serial port if needed.
5. Let the updater flash the board.

On macOS, the updater now prefers `/dev/cu.usbmodem*` and `/dev/cu.usbserial*` devices and auto-selects the board when there is only one likely USB candidate.

## What it flashes

This updater now flashes the same image set PlatformIO expects for the selected
controller environment:

- bootloader image at its PlatformIO offset
- partition table image at its PlatformIO offset
- `boot_app0.bin` at its PlatformIO offset
- the application image at its PlatformIO offset
- any additional board-specific flash images exported by PlatformIO

This avoids bundling a controller update that only writes `firmware.bin` while
silently assuming the box already has a matching bootloader and partition table.

## Platform notes

### Windows

Most Windows 10/11 systems should expose the ESP32-S3 as a `COM` port without extra setup.

If no COM port appears, install Espressif's ESP32-C3/S3 USB driver support:

- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/jtag-debugging/configure-builtin-jtag.html

### macOS

The macOS updater is published as a native `osx-arm64` or `osx-x64` console binary depending on the machine that builds it.

At runtime, the macOS updater uses the local Python `esptool` package instead of an embedded `.exe`.

Install that once with:

```bash
python3 -m pip install --user esptool
```

If upload still fails after choosing the USB port, hold `BOOT0`, short-press `EN`, then retry.

## Build on macOS/Linux

From the repo root:

```bash
python3 tools/windows_updater/prepare_assets.py --env adafruit_feather_esp32s3
python3 tools/windows_updater/build_updater.py --rid osx-arm64
```

Or through PlatformIO as a single custom target on an Apple Silicon Mac:

```bash
~/.platformio/penv/bin/pio run -e adafruit_feather_esp32s3 -t bundle_macos_updater
```

The same target also works for remote environments such as `remote-universal-s3`.
Those builds automatically emit `SG98-Remote-Updater-*` artifacts under
`publish/remote/` and `publish/bundles/`.

Output:

```text
tools/windows_updater/publish/controller/osx-arm64/SG98-Updater
tools/windows_updater/publish/bundles/SG98-Updater-osx-arm64.zip
```

Share the `.zip`, not just the bare binary. The archive includes the updater plus
any companion dylibs needed at runtime and preserves executable permissions when
unpacked on macOS.

## Build on Windows

From PowerShell in the repo root:

```powershell
py tools/windows_updater/fetch_esptool.py
py tools/windows_updater/prepare_assets.py --env adafruit_feather_esp32s3
py tools/windows_updater/build_updater.py --rid win-x64
```

Or through PlatformIO:

```powershell
pio run -e adafruit_feather_esp32s3 -t bundle_windows_updater
```

Remote environments use the same target name and automatically switch to the
`SG98-Remote-Updater-*` output names.

Output:

```text
tools/windows_updater/publish/controller/win-x64/SG98-Updater.exe
tools/windows_updater/publish/bundles/SG98-Updater-win-x64.zip
```

## Notes

- The Windows updater embeds the flasher and firmware into one `.exe`.
- The macOS updater embeds the flash manifest and images, then shells out to local `python3 -m esptool`.
- The build emits all distributable `.zip` bundles and matching `.md5` files under `tools/windows_updater/publish/bundles/`.
- `assets/esptool.exe`, `assets/flash-*.bin`, and `assets/sg98-flash-manifest.json` are intentionally not committed.
