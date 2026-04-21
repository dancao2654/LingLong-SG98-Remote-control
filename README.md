# LingLong SG98 Firmware Workspace
Controller and handheld-remote firmware for the LingLong SG98 grinder platform,
covering both the main SG98 controller board and the standalone ESP32-S3 remote.

#### Developer documentation

For a full project walkthrough, architecture guide, workflow explanation, and motor protocol notes, start with:

- `Documentation/Developer-Guide.md`
- `Documentation/IO-Reference.md`

#### Firmware targets

This repo contains two different ESP32-S3 firmware projects:

- repo root `.` = the SG98 controller box firmware
- `sg98-remote/` = the standalone SG98 hand-remote firmware project

For PlatformIO in the repo root `Project Tasks`:

- `adafruit_feather_esp32s3` = controller box
- `remote-universal-s3` = universal handheld remote firmware for supported
  ESP32-S3 remote boards

The remote code still lives as a separate project in `sg98-remote/`, but the
root `platformio.ini` exposes thin remote build aliases so you can build both
controller and remote firmware from one PlatformIO task tree.

The current universal remote image auto-detects these boards at boot:

- Elecrow CrowPanel 1.28 rotary/touch remote
- Waveshare 1.75 AMOLED touch remote

The supported remote boards now share:

- a common remote HAL for board detect, input, display, power, and debug
- on-screen battery status for the Waveshare `AXP2101` PMIC path
- idle light-sleep with wake by touch/button on supported boards

Do not flash `sg98-remote` onto the SG98 controller box.

If the box boots with serial or screen text such as `SG98 remote`, `Set target MAC`, `MANUAL`, or `ESP-NOW ch1`, the remote firmware is on the wrong device.

To recover the box, flash the controller environment from the repo root:

```bash
~/.platformio/penv/bin/platformio run -e adafruit_feather_esp32s3 -t upload
```

#### How to use
Aquire a PCB (order through JLCPCB with the provided files or buy a readymade one). and wire it up correctly.
Install Visual Studio code, install the PlatformIO extension. Clone this repo.

Make sure to (un)comment the proper drive type in definitions.
Uploading to ESP32 through USB-C. Make sure +24v is not present. If necessary short press EN while holding BOOT0 button for uploading.
If flash error in final install, plugging in the USB C shortly after pressing "Upload" will make it work.

#### BLE error console

The firmware now exposes a lightweight BLE error-history dump so you can keep `24V` connected and still inspect faults without USB-C. The board keeps a 1024-entry rolling log of motor and scale warning/error activity, then dumps it over BLE later instead of keeping a live BLE monitor session open during scale use.

Install the macOS Terminal client dependency:

```bash
python3 -m pip install --user -r tools/requirements-ble-console.txt
```

Use the Terminal client:

```bash
python3 tools/ble_console.py
```

It connects, downloads the buffered event history, prints it, and exits. If there is no history, it prints `no logged errors`.

If the log is very full, give the client more time:

```bash
python3 tools/ble_console.py --dump-timeout 30
```

To avoid disturbing the scale connection, BLE console advertising pauses while the board is actively in scale mode. The safe bench-verified setup is:

- let the board run normally with the scale
- return to idle
- run `python3 tools/ble_console.py` to fetch any recorded errors

The ring buffer stores the latest 1024 recorded events only. Fetching the dump does not clear the history; it is a rolling log.

#### USB-C web console

The firmware now also exposes a structured USB-C console API so a local browser page can render a real control UI instead of plain serial text.

The browser app lives in:

- `tools/web_console`

Run it locally:

```bash
python3 -m http.server 8080 -d tools/web_console
```

Then open in Chrome or Edge:

```text
http://localhost:8080
```

What it gives you:

- live status over USB-C
- quick start/stop and drive-selection controls
- set RPM / target weight controls
- Menu 1 to Menu 4 rendered as a browser UI, including the JMC baud submenu

Notes:

- This path uses Web Serial over USB-C, so use Chrome or Edge on desktop.
- Bluefy is still relevant for the BLE path, not the USB-C path.
- The browser UI talks to `src/diag_console.cpp` through `api ...` commands.

#### Onboard WiFi web console

The board now also hosts its own phone-friendly web UI over the ESP32-S3 WiFi radio.

How it works:

- the board always starts its own access point:
  - `SG98-XXXXXX`
- connect your phone to that WiFi
- open:
  - `http://192.168.4.1/`

What the onboard page gives you:

- live grinding screen graphics
- quick start/stop and drive controls
- full Menu 1 to Menu 4 browser control
- WiFi settings page to save home SSID/password

Home-network mode:

- the board keeps its own AP up
- you can also save home WiFi credentials in the web UI
- after that, the board will also join your LAN and show its home-network IP in the page

USB-C serial boot log now prints the AP details too:

```text
[wifi] ap_ssid=SG98-XXXXXX ap_ip=192.168.4.1
```

#### BOM
- ESP32 PCB board + 90 degree 2.54 pins
- 1.69" round corner SPI color display + 2.54 female header
- 19mm ultra short throw momentary button with 3-5V LED.
- M12 4 pin female connector
- KCD1 SPST switch
- 3D printed box, including 4xM3x5Wx4L heat inserts and 2x m3*8 countersunk screw and m3*6 non-countersunk screw.
- A knob for the rotary encoder to your choosing.

- 
