# SG98 Desktop Updater

This folder builds self-contained updater executables for SG98.

The updater bundles:

- the SG98 application firmware image
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

This updater currently flashes the application image only:

- `firmware.bin` -> `0x10000`

That is the correct path for field upgrades on boxes that already have the proper bootloader and partition table.

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
python3 tools/windows_updater/prepare_assets.py
python3 tools/windows_updater/build_updater.py --rid osx-arm64
```

Or through PlatformIO as a single custom target on an Apple Silicon Mac:

```bash
~/.platformio/penv/bin/pio run -e adafruit_feather_esp32s3 -t bundle_macos_updater
```

Output:

```text
tools/windows_updater/publish/osx-arm64/SG98-Updater
tools/windows_updater/publish/SG98-Updater-osx-arm64.zip
```

Share the `.zip`, not just the bare binary. The archive includes the updater plus
any companion dylibs needed at runtime and preserves executable permissions when
unpacked on macOS.

## Build on Windows

From PowerShell in the repo root:

```powershell
py tools/windows_updater/fetch_esptool.py
py tools/windows_updater/prepare_assets.py
py tools/windows_updater/build_updater.py
```

Or through PlatformIO:

```powershell
pio run -e adafruit_feather_esp32s3 -t bundle_windows_updater
```

Output:

```text
tools/windows_updater/publish/win-x64/SG98-Updater.exe
tools/windows_updater/publish/SG98-Updater-win-x64.zip
```

## Notes

- The Windows updater embeds the flasher and firmware into one `.exe`.
- The macOS updater embeds the firmware and shells out to local `python3 -m esptool`.
- The build now also emits a `.zip` archive per runtime under `tools/windows_updater/publish/`.
- `assets/esptool.exe` and `assets/sg98-firmware.bin` are intentionally not committed.
