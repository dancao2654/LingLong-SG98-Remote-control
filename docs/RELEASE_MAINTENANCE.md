# SG98 Release Maintenance

This document describes the normal maintenance flow for building SG98 controller
and remote firmware, preparing updater bundles, and publishing the public files
that devices can use for OTA checks.

## Repositories

- Private source repo:
  `https://github.com/dancao2654/LingLong-SG98-Remote-control`
- Public firmware release repo:
  `https://github.com/dancao2654/SG98-firmware-releases`

The firmware source can stay private. Device-downloadable OTA files must be in
the public release repo because ESP32 devices cannot authenticate to private
GitHub raw URLs.

## Public OTA URLs

The firmware uses these public manifest locations:

```text
https://raw.githubusercontent.com/dancao2654/SG98-firmware-releases/main/ota/controller-universal-s3/sg98-ota-manifest.json
https://raw.githubusercontent.com/dancao2654/SG98-firmware-releases/main/ota/remote-universal-s3/sg98-ota-manifest.json
```

Each manifest points to the matching application binary in the same public
folder. The device downloads only the application image for OTA. The full
desktop updater bundles remain available for first-time flash, recovery, or
partition/bootloader changes.

## Version Scheme

The release identity is:

```text
v<VERSION>-<git short sha>
```

For example:

```text
v2026.04.28-c9dcd6a3
```

`VERSION` supplies the date-style release version. The short SHA proves exactly
which source commit produced the firmware. The About page and USB diagnostic
APIs display the same release, SHA, role, and build environment.

Build environments used for public releases:

- Controller: `controller-universal-s3`
- Remote: `remote-universal-s3`

## Prerequisites

Install or verify:

```bash
git --version
gh --version
dotnet --version
$HOME/.platformio/penv/bin/platformio --version
```

Authenticate GitHub CLI for release publishing:

```bash
gh auth login
gh auth status
```

## Source Build Flow

Start from a clean private repo:

```bash
cd "/Users/dancao/Library/CloudStorage/OneDrive-Cisco/LingLong-SG98-Remote-control"
git status --short
```

Commit source changes before generating release artifacts so the firmware embeds
a clean SHA:

```bash
git add include src sg98-remote tools docs VERSION platformio.ini
git commit -m "Describe the firmware change"
```

Build both public firmware targets:

```bash
$HOME/.platformio/penv/bin/platformio run \
  -e controller-universal-s3 \
  -e remote-universal-s3
```

Prepare desktop updater bundles:

```bash
$HOME/.platformio/penv/bin/platformio run -t bundle_all_updaters
```

Prepare OTA files:

```bash
$HOME/.platformio/penv/bin/platformio run -t stage_all_ota_assets
```

Generated files are written to:

```text
tools/windows_updater/publish/bundles/
tools/windows_updater/publish/ota/controller-universal-s3/
tools/windows_updater/publish/ota/remote-universal-s3/
```

The bundle and OTA commands are intentionally separate. Run and publish only the
artifact family that changed.

## Verify Generated Metadata

Check that both OTA manifests show the clean source SHA and `gitDirty: false`:

```bash
python3 - <<'PY'
import json
from pathlib import Path
for path in sorted(Path("tools/windows_updater/publish/ota").glob("*/*.json")):
    data = json.loads(path.read_text())
    if path.name == "sg98-ota-manifest.json":
        print(path, data["version"], data["gitSha"], data["gitDirty"], data["envName"])
PY
```

Check that the raw OTA manifests resolve their application image by putting both
the manifest and `flash-03-0x10000-firmware.bin` in the same public folder.

## Commit Private Artifacts

Commit the generated private artifacts after verification:

```bash
git add tools/windows_updater/publish
git commit -m "Refresh SG98 release artifacts"
git push origin main
```

Do not commit `tools/windows_updater/assets/`; it is a temporary staging folder.

## Publish Public Release Repo

Refresh the public release repo clone:

```bash
cd /tmp/sg98-firmware-releases
git pull --ff-only origin main
```

Copy the public files from the private repo. For a full release, copy both
families:

```bash
SRC="/Users/dancao/Library/CloudStorage/OneDrive-Cisco/LingLong-SG98-Remote-control/tools/windows_updater/publish"
rsync -a --delete "$SRC/bundles/" bundles/
rsync -a --delete "$SRC/ota/" ota/
```

For an OTA-only release, copy only `ota/`. For a full-bundle-only release, copy
only `bundles/`.

Update `CHANGELOG.md` with the new tag and a short summary. Then commit and
push:

```bash
git add bundles ota CHANGELOG.md README.md
git commit -m "Publish SG98 firmware release <tag>"
git tag <tag>
git push origin main --tags
```

Create the GitHub release and upload the assets for the artifact family being
published. A full release can include both bundles and OTA files:

```bash
mkdir -p /tmp/sg98-release-upload
rm -f /tmp/sg98-release-upload/*
cp bundles/* /tmp/sg98-release-upload/
cp ota/controller-universal-s3/flash-03-0x10000-firmware.bin \
  /tmp/sg98-release-upload/controller-universal-s3-flash-03-0x10000-firmware.bin
cp ota/controller-universal-s3/sg98-ota-manifest.json \
  /tmp/sg98-release-upload/controller-universal-s3-sg98-ota-manifest.json
cp ota/controller-universal-s3/sg98-flash-manifest.json \
  /tmp/sg98-release-upload/controller-universal-s3-sg98-flash-manifest.json
cp ota/remote-universal-s3/flash-03-0x10000-firmware.bin \
  /tmp/sg98-release-upload/remote-universal-s3-flash-03-0x10000-firmware.bin
cp ota/remote-universal-s3/sg98-ota-manifest.json \
  /tmp/sg98-release-upload/remote-universal-s3-sg98-ota-manifest.json
cp ota/remote-universal-s3/sg98-flash-manifest.json \
  /tmp/sg98-release-upload/remote-universal-s3-sg98-flash-manifest.json

gh release create <tag> /tmp/sg98-release-upload/* \
  -R dancao2654/SG98-firmware-releases \
  --title "SG98 firmware <tag>" \
  --notes-file CHANGELOG.md
```

The raw `main` OTA files are what devices use for daily OTA checks. The GitHub
release assets are the archived human-downloadable copy of the files published
for that tag.

## First-Time Flash and Recovery

Use the full bundles for a new device, a recovery flash, or any release that
changes partitions or bootloader:

- `SG98-Updater-win-x64.zip`
- `SG98-Updater-osx-arm64.zip`
- `SG98-Remote-Updater-win-x64.zip`
- `SG98-Remote-Updater-osx-arm64.zip`

Normal future updates should be application-only OTA when the device reports:

- same role
- same build environment
- `supportsAppOnlyUpdate=true`
- current firmware version at or above `appOnlySince`

If any of those checks fail, use the full updater bundle.

## Release Checklist

1. Commit source changes in the private repo.
2. Build `controller-universal-s3` and `remote-universal-s3`.
3. Run `bundle_all_updaters` for full updater bundles when needed.
4. Run `stage_all_ota_assets` for OTA files when needed.
5. Verify OTA manifests are clean and match the source SHA when OTA files changed.
6. Commit and push private generated artifacts for the changed family.
7. Copy only the changed `bundles/` and/or `ota/` family to the public release repo.
8. Update changelog, commit, tag, and push public repo.
9. Create the public GitHub release and upload the changed assets.
10. Verify raw OTA manifest URLs with `curl` when OTA files changed.
11. On device, use About or USB diagnostics to confirm the expected version.
