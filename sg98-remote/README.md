# SG98 Remote

This is the separate PlatformIO project for SG98 handheld remotes that can talk
to the controller board over `ESP-NOW` or over `WebSocket` when both devices
are on the same home Wi-Fi.

It is intentionally separate from the main grinder controller firmware:

- different board target
- different UI stack
- different power and sleep behavior
- no RS-485 / motor-control code on the remote

Do not flash this image onto the SG98 controller box. The controller firmware
still lives in the repo root and uses the `adafruit_feather_esp32s3`
environment.

## Supported boards

- `remote-universal-s3` = one universal remote image for these supported
  `ESP32-S3` handheld boards:
  - Elecrow CrowPanel 1.28 rotary/touch knob display
  - Elecrow CrowPanel 2.1 rotary/touch knob display
  - Waveshare 1.75 AMOLED round touch display

The remote app is shared across boards through a small board HAL:

- display and touch/encoder input are board-specific
- `ESP-NOW`, `WebSocket`, SG98 protocol parsing, menus, and screen flow are shared
- touch-only boards can simulate the rotary workflow with circular drag gestures
- the runtime picks the board driver by probing the known peripheral set at boot

Common board services are exposed in `include/remote_board.h` so future boards
can add reusable interfaces without pushing pin maps into app code:

- `RemoteBoardInfo` for identity, size, and capability flags
- `RemoteBoardServices` for optional shared buses like `I2C`, `SPI`, `UART`,
  and the active debug console
- `RemoteBoardPins` for semantic GPIO-role mapping inside reusable low-level code
- `RemoteBoardFeatureMap` for per-board support levels:
  - `Present` = hardware exists on the board
  - `Mapped` = bus/pin mapping is known in the HAL
  - `Implemented` = firmware support is live in the current build
- `remote_board_set_backlight()` and `remote_board_battery_status()` for
  optional board-level power hooks

Vendor hardware references for the supported remote boards are cached under:

- `../Documentation/Hardware/`

## Current behavior

- bi-directional SG98 remote transport over:
  - `ESP-NOW` when the controller radio mode is `ESP-NOW`
  - `WebSocket` when the controller radio mode is `WEB` and both devices are on the same LAN
- live status, mode, RPM, weight, drive, comm, torque, and error display
- live battery / USB / charging status on supported boards
- screen switching for main control, purge settings, GBW settings, and system
  settings
- controller menu sync through `GET_STATUS`, `GET_MENU`, `SET_MENU`, and
  `ACTION` frames
- encoder-driven navigation on CrowPanel
- touch-driven dial, taps, and menu scrolling on Waveshare AMOLED
- native USB-C serial debug console exposed through the board HAL
- idle deep-sleep with RTC state restore on boards whose HAL advertises deep-sleep wake support
- remote-side Wi-Fi onboarding portal with captive AP, saved STA credentials,
  LAN reconnect, and mDNS controller discovery

## Configuration

Edit [include/remote_config.h](include/remote_config.h) before first use:

- set `kTargetMac` to the SG98 controller board's `STA` MAC address
- set `kTargetMacConfigured = true`
- keep `kEspNowChannel = 1` unless the SG98 controller firmware changes too

For `WebSocket` mode:

- the remote brings up its own setup AP when it needs Wi-Fi onboarding
- connect a phone or laptop to the remote AP and browse to `http://192.168.4.1/`
- save the same home Wi-Fi SSID/password used by the controller
- once both devices are on the same LAN, the remote discovers the controller
  with mDNS and switches to `WebSocket` automatically

## Build

```bash
~/.platformio/penv/bin/platformio run -d sg98-remote -e remote-universal-s3
```

## Upload

```bash
~/.platformio/penv/bin/platformio run -d sg98-remote -e remote-universal-s3 -t upload
```

## Notes

- The controller-side receiver lives in the repo root under
  `src/espnow_control.cpp`, `src/remote_link.cpp`, and `src/remote_ws.cpp`.
- Set the target controller MAC before first upload or the remote will stay in
  `ESP-NOW` pairing/config mode.
- The Waveshare board is touch-only, so circular drag gestures replace the
  physical encoder.
- This universal build is currently intended for the supported `ESP32-S3R8N16`
  remote boards above, not arbitrary ESP32 boards.
