# SG98 Porting Guide (Plain English)

This guide is for the next person who says:

- "I need to add a new motor driver library."
- "I need to move the controller firmware to a different ESP32 board."
- "I need to move the remote firmware to a different ESP32 board."

The goal is not elegance. The goal is getting from `it does not build` to `it runs on real hardware` with the fewest surprises.

## 1. Understand The Repo First

There are really two firmware projects in this repo:

1. Controller firmware
   - main source: `src/`
   - controller PlatformIO config: `platformio.ini`
   - current controller targets:
     - `adafruit_feather_esp32s3`
     - `waveshare_esp32_s3_rs485_can`

2. Remote firmware
   - main source: `sg98-remote/src/`
   - remote PlatformIO config: `sg98-remote/platformio.ini`
   - build entry is still also exposed from root `platformio.ini`
   - current remote targets:
     - `remote-universal-s3`
     - `remote-crowpanel-128`
     - `remote-crowpanel-21`
     - `remote-waveshare-175`

If you forget which side you are editing, you will lose time.

## 2. Mental Model: What Lives Where

### Controller

- `src/main.cpp`
  - boot sequence
  - menus
  - high-level state machine
- `src/definitions.h`
  - board pin map
  - default values
  - controller board capability macros
- `src/io.cpp`
  - buttons, encoder, LED
- `src/display.cpp`
  - local square-screen UI
- `src/comm.cpp` and `src/comm.h`
  - RS485 / Modbus-ish transport details and timing
- `src/motor_hal.cpp` and `src/motor_hal.h`
  - one place that decides which motor backend is active
- `src/JMC/`
  - JMC motor backend
- `src/RT/`
  - RT motor backend
- `src/pdata.cpp`
  - controller persistent settings in NVS
- `src/diag_console.cpp`
  - USB serial debug console

### Remote

- `sg98-remote/src/remote_app.cpp`
  - main app logic
  - menus
  - transport logic
  - persistent UI snapshot
  - USB debug console
- `sg98-remote/include/remote_board.h`
  - board-facing interfaces and structs
- `sg98-remote/src/remote_board_driver.h`
  - function table every remote board driver must implement
- `sg98-remote/src/remote_board.cpp`
  - board registration and auto-detect order
- `sg98-remote/src/remote_board_*.cpp`
  - board-specific drivers
- `sg98-remote/include/sg98_remote_defs.h`
  - remote defaults

## 3. How To Add A New Motor Library On The Controller

This is the cleanest way to think about it:

1. `comm.cpp` talks bytes on the RS485 wire.
2. Your motor backend translates controller intent into that motor's protocol.
3. `motor_hal.cpp` chooses which backend is active and exposes one common API to the rest of the controller.

If your new motor can reuse the same raw RS485 helper functions, life is easier. If it needs a different framing or timing model, you may also need a new comm profile.

### Step 1: Copy An Existing Backend

Pick the closer of these two as your starting point:

- `src/JMC/`
- `src/RT/`

Create a new folder, for example:

- `src/NEWMOTOR/`

Create a header that matches the existing backend shape. The current HAL expects backend functions like:

- `init_backend`
- `do_comm_backend`
- `off_backend`
- `calibrate_backend`
- `sleep_backend`
- optionally `reset_backend`

You can see the existing expected shape in:

- `src/JMC/motorcontrol_jmc.h`
- `src/RT/motorcontrol_rt.h`

### Step 2: Add The New Drive Type To The HAL

Edit:

- `src/motor_hal.h`
- `src/motor_hal.cpp`

What to add:

1. A new `DriveType` enum value.
2. A new label in `motor_get_type_label()`.
3. New routing in:
   - `motor_init()`
   - `do_comm()`
   - `motorOff()`
   - `Calibrate()`
   - `motorSleep()`
   - any reset helper if your backend supports it
4. Max RPM / direction / ramp capability helpers if your new motor behaves differently.

If you skip any one of those routes, the build may still pass but the motor will behave half-ported.

### Step 3: Decide Whether Auto Detect Must Support It

Edit:

- `src/motor_hal.cpp`

The controller currently auto-detects JMC vs RT in the HAL.

If your new motor should be auto-detected:

1. Add a probe path for it.
2. Decide the priority order when more than one probe partly answers.
3. Make sure you restore the original comm profile after probing.

If your new motor should not be auto-detected yet:

1. Add the backend.
2. Force it manually first.
3. Add detection later after the motor is stable.

That is usually the safer order.

### Step 4: Add Menu And Console Hooks

At minimum, update:

- `src/main.h`
- `src/main.cpp`
- `src/diag_console.cpp`

What to think about:

1. Does the user need a manual selection item?
2. Does the user need motor-specific config like baud, torque mode, special calibration, or safety limits?
3. Should the debug console expose raw or friendly commands for the new backend?

If the remote must also select this motor manually, you will also need to update:

- `sg98-remote/src/remote_app.cpp`

### Step 5: Save Settings If You Added New Ones

If you add a new persistent setting on the controller, update:

- `src/definitions.h` for default values
- `src/main.cpp` for menu entries
- `src/pdata.cpp` for read/write/normalize behavior

If the setting is missing from `pdata.cpp`, it will look like it works until the next reboot.

### Step 6: Build And Test In This Order

1. Build only:
   ```bash
   ~/.platformio/penv/bin/platformio run -e adafruit_feather_esp32s3
   ```
2. Flash hardware.
3. Use the USB console in `src/diag_console.cpp` to prove:
   - probe result
   - current drive type
   - read/write success
   - start/stop behavior
4. Only then add remote-side UI niceties.

### Common Motor-Porting Mistakes

- You added the backend files but forgot to route them in `motor_hal.cpp`.
- You reused JMC timings for a non-JMC device and reads randomly fail.
- You made a new menu item but forgot to persist it in `pdata.cpp`.
- You added auto-detect too early and now detection is flaky.
- You changed max RPM in one file but not in UI clamping logic.

## 4. How To Port The Controller To A New ESP32 Board

There are two controller port types:

1. Easy port
   - same controller features
   - only pins and board definition change
2. Real port
   - display/no display changes
   - encoder or button layout changes
   - RS485 control pins differ
   - flash/PSRAM/partition settings differ

### Step 1: Add Or Reuse A PlatformIO Environment

Edit:

- `platformio.ini`

If PlatformIO already knows your board, add a new environment using that board.

If PlatformIO does not know your board, you may need a custom board JSON. The repo already does that on the remote side under:

- `sg98-remote/boards/`

You can use the same idea for controller boards if needed.

### Step 2: Add A Board Flag And Pin Map

Edit:

- `src/definitions.h`

The current controller already has two board personalities:

- default controller box
- Waveshare RS485/CAN board

Use that pattern again:

1. Add a new build flag in `platformio.ini`.
2. Add a matching `#if defined(...)` block in `src/definitions.h`.
3. Define:
   - target name
   - feature macros like `SG98_HAS_DISPLAY`
   - RS485 pins
   - button pins
   - encoder pins
   - display pins
   - sleep/wake pins
   - any expansion pins you care about

### Step 3: Let Existing Modules Tell You What Broke

The controller side is already partly abstracted by feature macros:

- `SG98_HAS_DISPLAY`
- `SG98_HAS_START_BUTTON`
- `SG98_HAS_START_LED`
- `SG98_HAS_ENCODER`
- `SG98_HAS_ENCODER_BUTTON`

Those guards are already used in:

- `src/main.cpp`
- `src/io.cpp`
- `src/display.cpp`

So after you add your board flag in `definitions.h`, build once and see what still assumes hardware exists when it does not.

### Step 4: Verify RS485 First

Before caring about the screen, prove these work:

1. `Serial2`
2. `DE` / `RE`
3. motor comm

Files involved:

- `src/comm.cpp`
- `src/definitions.h`

If RS485 is wrong, the rest of the controller port is fake progress.

### Step 5: Verify Local UI Only If Your Board Has It

If your new controller board has:

- no screen
- no encoder
- no start button

then copy the approach used by the Waveshare controller target and do not waste time trying to force the square-screen UI onto a headless board.

### Step 6: Test In This Order

1. Board boots.
2. USB serial console works.
3. RS485 works.
4. Motor can auto-detect.
5. Start/stop works.
6. Sleep/wake works.
7. Local display/input works if the board has them.

### Common Controller-Board Porting Mistakes

- Wrong `DE` / `RE` polarity.
- Wrong wake pins.
- Forgot to set flash size or partitions.
- Left display pins enabled on a headless board.
- Chose a board without enough flash and the build almost fits until one more feature lands.

## 5. How To Port The Remote To A New ESP32 Board

The remote side is cleaner than the controller side. Treat the board as a plugin.

### Step 1: Add A Board Environment

Edit:

- `sg98-remote/platformio.ini`

Usually you want two environments while bringing up a new board:

1. forced board env
   - easiest for bring-up
2. universal env support
   - only after probe logic is stable

This is exactly how the current remote targets are organized.

### Step 2: Add A Driver File

Create a new driver file like:

- `sg98-remote/src/remote_board_my_new_board.cpp`

Use one of these as your template:

- `sg98-remote/src/remote_board_crowpanel.cpp`
- `sg98-remote/src/remote_board_crowpanel_rotary21.cpp`
- `sg98-remote/src/remote_board_waveshare_amoled175.cpp`

### Step 3: Implement The `RemoteBoardDriver` Contract

The driver interface is declared in:

- `sg98-remote/src/remote_board_driver.h`

You need to provide:

1. `probe`
2. `init`
3. `poll_input`
4. `canvas`
5. `info`
6. `services`
7. `pins`
8. `feature_map`
9. `apply_indicator`
10. `set_backlight`
11. `battery_status` or `battery_millivolts`
12. `input_pending`

If one of those does not apply, return something harmless. Do not leave garbage pointers.

### Step 4: Fill In The Board Metadata

The board-facing structs live in:

- `sg98-remote/include/remote_board.h`

At minimum define:

1. `RemoteBoardPins`
2. `RemoteBoardInfo`
3. `RemoteBoardServices`
4. `RemoteBoardFeatureMap`

These are what the generic app uses to decide:

- screen size
- touch presence
- encoder presence
- battery support
- transport/debug capabilities

### Step 5: Register The Board

Edit:

- `sg98-remote/src/remote_board_driver.h`
- `sg98-remote/src/remote_board.cpp`

You must:

1. declare the driver factory
2. add it to the driver list
3. choose the probe order carefully

Probe order matters. The universal build only works if the more specific boards probe before looser fallback boards.

### Step 6: Be Careful With Shared Buses In Probe Logic

This one matters because we already hit it.

When the universal build probes multiple boards, one probe can leave shared hardware state behind for the next probe. On the Waveshare, the fix was to explicitly reinitialize the I2C bus inside the Waveshare probe/init before talking to the touch controller.

So for new remote boards:

1. keep `probe()` cheap
2. keep `probe()` deterministic
3. if a probe touches shared `Wire` or `SPI`, reset/reconfigure it for that board inside the probe
4. do not assume your board probes first forever

### Step 7: Only Change `remote_app.cpp` If You Really Need To

Most of the time, the app should stay generic and your new board driver should adapt to the app.

Only edit `sg98-remote/src/remote_app.cpp` if the new hardware truly changes:

- screen shape
- touch model
- encoder model
- battery UX
- transport UX

If all you need is a different display or touch driver, keep the change inside the board driver.

### Step 8: Persistence Lives In Different Places On The Remote

The remote does not use controller `pdata.cpp`.

Remote persistence mainly lives in:

- `sg98-remote/src/remote_app.cpp`

Look for:

- `seedMenuDefaults()`
- `restorePersistentSnapshot()`
- `savePersistentSnapshotNow()`
- `markPersistentStateDirty()`

If you add a remote-only setting and forget to touch the snapshot path, it will not survive reboot.

### Step 9: Bring-Up Order For A New Remote Board

Do it in this order:

1. USB serial prints boot logs.
2. `probe()` correctly identifies the board.
3. display works.
4. input works.
5. battery / PMIC works if present.
6. Wi-Fi and ESP-NOW work.
7. only then enable universal auto-detect.

Do not start with the universal build if the forced build still fails.

### Common Remote-Board Porting Mistakes

- Probe tries to do too much.
- Shared I2C bus is left configured for the previous board probe.
- Touch works only in forced build, not in universal build.
- Screen works but `RemoteBoardInfo.width` / `height` are wrong, so the UI feels broken.
- Driver compiles but `input_pending()` never wakes the app efficiently.

## 6. If You Add New Menu Items Or Settings

This is the boring part people forget.

### Controller

If you add a new controller setting, check all of these:

1. `src/definitions.h`
2. `src/main.h`
3. `src/main.cpp`
4. `src/pdata.cpp`
5. `src/diag_console.cpp`
6. remote mirror logic in `sg98-remote/src/remote_app.cpp` if the remote should expose it

### Remote

If you add a new remote-only setting, check:

1. `sg98-remote/include/sg98_remote_defs.h`
2. `sg98-remote/src/remote_app.cpp`
3. remote persistent snapshot code in the same file

## 7. Build Commands You Will Actually Use

### Controller

```bash
~/.platformio/penv/bin/platformio run -e adafruit_feather_esp32s3
~/.platformio/penv/bin/platformio run -e waveshare_esp32_s3_rs485_can
```

### Remote

```bash
~/.platformio/penv/bin/platformio run -d sg98-remote -e remote-universal-s3
~/.platformio/penv/bin/platformio run -d sg98-remote -e remote-crowpanel-21
~/.platformio/penv/bin/platformio run -d sg98-remote -e remote-crowpanel-128
~/.platformio/penv/bin/platformio run -d sg98-remote -e remote-waveshare-175
```

### Upload Example

```bash
~/.platformio/penv/bin/platformio run -d sg98-remote -e remote-universal-s3 -t upload --upload-port /dev/cu.usbmodem21101
```

## 8. Recommended Order Of Work

If you want the least painful path:

1. Make it compile.
2. Make USB console work.
3. Make the real hardware transport work.
4. Make persistence work.
5. Make menus nice.
6. Make universal auto-detect nice.
7. Package updater bundles last.

If you reverse that order, you will spend time shipping builds you do not trust yet.

## 9. Sanity Checklist Before You Say "Done"

- Correct PlatformIO env exists.
- Firmware builds cleanly.
- Firmware flashes.
- USB serial logs are readable.
- Required hardware is detected.
- Start/stop works.
- Settings survive reboot if they are supposed to.
- Universal remote build probes correctly if you touched probe logic.
- Updater bundles are generated only after the firmware itself is proven.

That is the practical version.
