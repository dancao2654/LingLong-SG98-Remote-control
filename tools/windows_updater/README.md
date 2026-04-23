# SG98 Desktop Updater

This folder builds self-contained desktop updater bundles for the SG98
controller and the SG98 handheld remote.

Each updater bundle embeds a full ESP32-S3 flash set:

- bootloader
- partitions
- `boot_app0`
- application firmware

## PlatformIO Tasks

In the PlatformIO sidebar, open the `Custom` group under these environments:

- `adafruit_feather_esp32s3`
- `waveshare_esp32_s3_rs485_can`
- `remote-universal-s3`

Available updater tasks:

- `Bundle Windows Updater`
- `Bundle macOS ARM64 Updater`
- `Bundle All Windows Updaters`
- `Bundle All macOS ARM64 Updaters`
- `Bundle All Updaters`

The aggregate tasks build the currently shipped updater set:

- controller updater from `adafruit_feather_esp32s3`
- remote updater from `remote-universal-s3`

Bundle generation is pinned to the release runtimes:

- Windows bundles -> `win-x64`
- macOS bundles -> `osx-arm64`

That means the same PlatformIO task can be run on both Windows and macOS
machines and will still emit the same release bundle set.

## Output Layout

Unpacked runtime outputs are written under:

- `tools/windows_updater/publish/controller/`
- `tools/windows_updater/publish/remote/`
- `tools/windows_updater/publish/controller-waveshare/`

Distributable zip bundles and checksums are written under:

- `tools/windows_updater/publish/bundles/`

Examples:

- `SG98-Updater-win-x64.zip`
- `SG98-Updater-win-x64.zip.md5`
- `SG98-Remote-Updater-osx-arm64.zip`
- `SG98-Remote-Updater-osx-arm64.zip.md5`
- `SG98-Updater-osx-arm64.zip`
- `SG98-Remote-Updater-win-x64.zip`

## Direct Script Usage

Build a controller updater from the repo root:

```bash
python3 tools/windows_updater/prepare_assets.py --env adafruit_feather_esp32s3
python3 tools/windows_updater/build_updater.py --rid win-x64 --output-base SG98-Updater --publish-subdir controller
python3 tools/windows_updater/build_updater.py --rid osx-arm64 --output-base SG98-Updater --publish-subdir controller
```

Build a remote updater from the repo root:

```bash
python3 tools/windows_updater/prepare_assets.py --env remote-universal-s3
python3 tools/windows_updater/build_updater.py --rid win-x64 --output-base SG98-Remote-Updater --publish-subdir remote
python3 tools/windows_updater/build_updater.py --rid osx-arm64 --output-base SG98-Remote-Updater --publish-subdir remote
```

## Notes

- Windows bundles embed `esptool.exe`.
- macOS bundles use the local `python3 -m esptool` path at runtime.
- Bundles flash the full image set instead of only replacing the app at `0x10000`.
- Each generated zip also gets a matching `.md5` file.
- `tools/windows_updater/assets/` is a transient staging area.
