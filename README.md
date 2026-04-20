# LingLong SG98 Remote control
A remote control PCB & software to control a grinder with a servo motor.
Currently being built for Linglong SG98 using RT and JMC servo drive.

#### Developer documentation

For a full project walkthrough, architecture guide, workflow explanation, and motor protocol notes, start with:

- `Documentation/Developer-Guide.md`

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

#### BOM
- ESP32 PCB board + 90 degree 2.54 pins
- 1.69" round corner SPI color display + 2.54 female header
- 19mm ultra short throw momentary button with 3-5V LED.
- M12 4 pin female connector
- KCD1 SPST switch
- 3D printed box, including 4xM3x5Wx4L heat inserts and 2x m3*8 countersunk screw and m3*6 non-countersunk screw.
- A knob for the rotary encoder to your choosing.

- 
