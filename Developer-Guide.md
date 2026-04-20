# LingLong SG98 Remote Control Developer Guide

## Purpose

This document explains the design, runtime workflow, and motor-control protocol used by the LingLong SG98 remote control project.

It is written for new contributors who need to:

- build and flash the firmware
- understand how the UI and state machine work
- trace how the ESP32 talks to the servo drive
- understand how grind-by-weight works
- know where to make changes safely

This guide is based on the current source tree in this repository.

## What This Project Is

This project is an ESP32-S3 based front-panel controller for a grinder. It is not a desktop application and it does not generate hobby-servo PWM.

The controller does five main jobs:

1. Reads the front-panel controls:
   - start button
   - rotary encoder
   - encoder push button
2. Draws the local UI on a small SPI display.
3. Talks to an industrial servo drive over RS-485.
4. Optionally connects to a BLE scale for grind-by-weight mode.
5. Stores settings and calibration data in ESP32 non-volatile storage.

At a high level, the firmware takes user intent such as "grind at 1000 RPM" or "grind to 18.0 g", converts that into `motor_setRPM`, then the motor-control layer translates that into drive register writes over RS-485.

## Current Build Configuration

The active drive selection is in `src/definitions.h`.

Current source tree:

- `JMC_DRIVE` is disabled
- `RT_DRIVE` is enabled

That means the current firmware build uses the RT motor-control implementation by default.

## Protocol References Used In This Guide

The protocol explanations in this guide come from both the firmware source and the vendor manuals already stored in `Documentation/`.

The most relevant protocol references are:

- `Documentation/JAND-D 15002 Servo drive.pdf`
  - JMC RS-485 / Modbus framing overview
  - especially PDF pages 173-177
- `Documentation/JMC Manual RU.pdf`
  - JMC object dictionary and CiA402 state-machine semantics
  - especially PDF pages 434, 450, 467-478
- `Documentation/RT RS RS485 Series Servo User Manual V4.0.pdf`
  - RT RS-485 / Modbus framing
  - torque-mode parameter meanings
  - monitor parameter meanings
  - especially PDF pages 64-73 and 140-147

When this guide says a manual "defines" something, that comes from those PDFs.
When this guide says "the firmware appears to" or "inference", that comes from matching the source code to the manual terminology and register/object IDs.

## Repository Layout

### Main firmware files

- `src/main.cpp`
  - boot flow
  - top-level loop
  - purge logic
  - sleep logic
- `src/main.h`
  - global states
  - menu enums
  - shared globals
- `src/definitions.h`
  - board pins
  - timing constants
  - build defaults
  - drive selection

### UI and persistence

- `src/io.cpp`
  - button and encoder handling
  - state transitions
  - menu navigation
  - front LED behavior
- `src/display.cpp`
  - display initialization
  - screen drawing
  - error overlay
- `src/pdata.cpp`
  - persistent settings
  - calibration storage
  - saved scale identity

### Scale and motor control

- `src/gbw.cpp`
  - BLE scale management
  - grind-by-weight shot tracking
  - prediction and learning
- `src/comm.cpp`
  - RS-485 transport
  - Modbus-RTU-style framing
  - CRC handling
- `src/motorcontrol_jmc.cpp`
  - JMC drive initialization and runtime control
- `src/motorcontrol_rt.cpp`
  - RT drive initialization and runtime control

### Hardware and reference material

- `Documentation/`
  - vendor manuals
  - mechanical references
- `PCB/`
  - board production files
- `3DP parts/`
  - enclosure parts

## Hardware Overview

The controller appears to be built around an ESP32-S3 Feather-compatible board and a small SPI TFT display.

### Pin map

From `src/definitions.h`:

- RS-485
  - RX: GPIO 18
  - TX: GPIO 17
  - RE: GPIO 15
  - DE: GPIO 16
- User input
  - start button: GPIO 4
  - start button LED: GPIO 5
  - encoder button: GPIO 42
  - encoder A: GPIO 41
  - encoder B: GPIO 40
- Display
  - SCL: GPIO 10
  - SDA/MOSI: GPIO 11
  - RST: GPIO 12
  - DC: GPIO 13
  - CS: GPIO 14
  - backlight: GPIO 21

### Communication links

There are two separate serial paths in this project:

- USB serial
  - used for logs and debugging
  - `Serial.begin(115200)`
- RS-485 serial
  - used for motor drive control
  - `Serial2.begin(9600, SERIAL_8N1, RS485_RXD, RS485_TXD)`

## Build and Flash Workflow

### Tools

The project uses PlatformIO with the Arduino framework.

Relevant dependencies in `platformio.ini`:

- `JC_Button`
- `ESP32Encoder`
- `Adafruit GFX Library`
- `Adafruit-ST7735-Library` from GitHub
- `ESP32Arduino-BLEScale` from GitHub

### Typical developer workflow

1. Open the repository root in VS Code.
2. Make sure PlatformIO is installed.
3. Check `src/definitions.h` and confirm the correct drive type is enabled.
4. Build.
5. Flash over USB-C.
6. Open the serial monitor if needed.

### Safety note for flashing

The project README explicitly warns not to flash with the main `+24V` supply connected. Use USB-C only during firmware upload.

## Runtime Architecture

The firmware is organized around:

- one Arduino main loop
- one FreeRTOS task for the BLE scale
- shared global state

### Boot flow

`setup()` does the following:

1. Starts USB serial logging.
2. Creates `scaleMutex`.
3. Restores GPIO state if waking from deep sleep.
4. Initializes persistent storage.
5. Either loads saved settings or writes defaults on first boot.
6. Starts the background scale task on core 0.
7. Initializes display, inputs, comms, motor control, and watchdog.

### Main loop order

The top-level loop order is important:

1. `doVitals()`
2. `do_comm()`
3. `do_io()`
4. `sleepTimeCheck()`
5. `update_display()`
6. state-specific action

That means:

- motor comms are serviced continuously
- user input can immediately change `state` or `motor_setRPM`
- the display reflects the newest state and feedback

### Background scale task

The scale task runs `gbwVitals()` every 10 ms.

That task is responsible for:

- BLE connection management
- receiving new scale weight values
- tracking the currently connected scale name and MAC
- deciding when the firmware should attempt to connect or maintain the link

The main loop does not directly own BLE operations. It only consumes the resulting shared data such as:

- `currentWeight`
- `scaleStatus`
- `scale_name`
- `scale_mac`

## State Machine

The top-level states are:

- `SLEEPING`
- `IDLE`
- `IDLE_GBW`
- `GRINDING`
- `PURGING`
- `GRINDING_GBW`
- `MENU1`
- `MENU2`
- `MENU3`
- `CALIBRATING`

### `IDLE`

Purpose:

- manual RPM mode

Behavior:

- encoder changes `setRPM`
- start button enters `GRINDING`
- short encoder press opens `MENU1`
- long encoder press switches to `IDLE_GBW`
- last RPM is stored after inactivity

### `IDLE_GBW`

Purpose:

- grind-by-weight mode while idle

Behavior:

- encoder changes `setWeight` in mg
- start button enters `GRINDING_GBW`
- short encoder press opens `MENU1`
- long encoder press switches back to `IDLE`
- last target weight is stored after inactivity

### `GRINDING`

Purpose:

- manual motor run

Behavior:

- motor target follows `setRPM`
- encoder can still adjust RPM while grinding
- start button stops grinding
- if `Purge /w button` is enabled, that stop enters `PURGING`
- otherwise it returns to `IDLE`
- encoder button stops immediately and returns to `IDLE`

### `PURGING`

Purpose:

- clear retained grounds after grind completion or manual request

Behavior:

- optional zero-RPM delay
- forward purge phase
- optional pause
- optional reverse purge phase
- then return to `IDLE`

### `GRINDING_GBW`

Purpose:

- automatic weight-based grinding

Behavior:

- taring if needed
- delayed start
- normal grind RPM
- optional slow phase near target
- prediction-based stop
- short learning phase
- return to `IDLE_GBW`

### Menu states

Purpose:

- view and edit settings

Behavior:

- encoder scrolls menu entries
- encoder press enters or confirms a setting
- start button backs out or cancels the edit

### `CALIBRATING`

Purpose:

- measure grinder torque baseline across RPM points

Behavior:

- motor runs through many RPM steps
- torque is averaged at each step
- results are written to persistent storage

### `SLEEPING`

Purpose:

- deep sleep for inactivity reduction

Behavior:

- watchdog is disabled
- motor is stopped
- display is turned off
- drive is put into a lower-power state
- wake is configured on the start button

## Menu Design

The menu system is data-driven. Menu items are defined in `src/main.cpp` using `menuEntry` arrays.

Each menu entry contains:

- display name
- current value
- min and max
- step scalar
- preference key
- unit string

### Menu 1: general

JMC build currently exposes:

- exit menu
- purge settings
- GbW settings
- sleep time
- display brightness
- LED brightness
- invert scrolling
- max RPM
- min RPM
- motor torque
- motor ramp
- calibrate
- reset default

### Menu 2: purge

- auto purge enable
- purge on button stop
- forward purge RPM
- reverse purge RPM
- purge delay
- purge duration
- zero-RPM delay before purge
- stabilize time before torque detection
- frame thresholds
- high and low torque scalar thresholds
- auto-off time

### Menu 3: grind by weight

- grind RPM
- slow-phase RPM
- slow-phase threshold in mg before target
- start delay
- speed modifier
- stop-time offset
- save scale pairing

## Persistent Data Model

Persistent storage uses the ESP32 `Preferences` API under the namespace `settings`.

### What is stored

- build ID
- manual idle RPM
- GBW target weight
- last idle mode
- all menu values
- calibration array
- saved BLE scale name
- saved BLE scale MAC

### First-boot and migration behavior

On boot, `pdata_init()` compares the stored `build_id` with the current `BUILD_ID`.

If they differ:

- NVS is erased
- defaults are re-written
- the firmware treats the boot as a fresh configuration

This is the project's migration strategy. If you make a settings change that is not backward compatible, bump `BUILD_ID`.

### Persistence write modes

`pdata_write()` is used with numeric modes:

- `0`
  - write all defaults
- `1`
  - write calibration array
- `2`
  - write current menu item
- `3`
  - write current `setRPM` or `setWeight`
- `4`
  - write learned GBW parameters
- `5`
  - write current idle mode
- `6`
  - clear saved scale
- `7`
  - save current scale
- `8`
  - force reset on next boot by changing `build_id`

## Display Design

The UI is full-screen and state-driven. It does not rebuild the whole screen every loop. Instead it:

- tracks the last rendered screen
- tracks previously rendered values
- redraws only when data changes

### Screen types

- manual idle screen
- GBW idle screen
- grinding/purging screen
- GBW grinding screen
- calibrating screen
- menu list screen
- menu value screen
- error overlay

### Display behavior

- refresh is capped by `DISP_REFRESH_RATE`
- `disp_updateRequired` forces refreshes when data changes
- `newData` from the motor also triggers redraw
- the display dims after long inactivity

### What the display shows

Manual idle:

- target RPM
- drive connection state
- purge enabled state

GBW idle:

- target weight
- live scale weight
- drive connection state
- scale connection state

Grinding:

- actual motor RPM
- live torque gauge
- drive state
- purge state

GBW grinding:

- commanded RPM
- current weight
- drive state
- scale state

Calibration:

- live RPM
- live torque reading
- connection state
- progress counter

### Error overlay

Errors are shown as a red overlay on idle screens. Non-fatal errors above 100 auto-clear after 5 seconds.

## Input Handling

The UI uses:

- `JC_Button` for button edge handling
- `ESP32Encoder` for rotary input

Important behavior:

- actions trigger on button release, not on initial press
- long-press detection is used only on the encoder button
- encoder movement updates `lastActivity`
- button release updates `lastActivity`

The front LED is state-driven and supports:

- off
- solid on
- slow flash
- fast flash
- fastest flash
- breathing/fade
- direct PWM percentage

## Communication Stack

The motor communication stack has two layers:

1. a generic RS-485 transport in `src/comm.cpp`
2. a drive-specific protocol in `src/motorcontrol_jmc.cpp` or `src/motorcontrol_rt.cpp`

### What the manuals confirm

The serial framing used by this project matches the Modbus RTU descriptions in:

- `Documentation/JAND-D 15002 Servo drive.pdf`, PDF pages 173-177
- `Documentation/RT RS RS485 Series Servo User Manual V4.0.pdf`, PDF pages 140-147

Those manuals explicitly describe:

- master/slave communication
- function codes `0x03`, `0x06`, and `0x10`
- big-endian 16-bit register data
- CRC16 with polynomial `0xA001`
- CRC low byte first, high byte last
- an idle gap of at least 3.5 character times between frames

That aligns closely with the implementation in `src/comm.cpp`.

### Physical and link settings

Current settings:

- bus type: half-duplex RS-485
- UART: `Serial2`
- baud rate: `9600`
- format: `8N1`
- slave address: `0x01`

Important manual note:

- the RT manual lists the default RS-485 settings as axis address `1`, `115200 bps`, and `8N1`
- the JMC Modbus section also describes RS-485 operation up to `115200`
- this firmware is hard-coded to `9600 8N1`

So for this project to communicate successfully, the attached drive must already be configured to match the firmware, not the vendor default.

### Direction control

The ESP controls RS-485 direction with:

- `RS485DE`
- `RS485RE`

Transmit:

- DE high
- RE high

Receive:

- DE low
- RE low

The code inserts short `delayMicroseconds(500)` delays before and after the transmit flush.

### Frame types

The transport supports standard Modbus-RTU-style register access:

- `0x03`
  - read holding registers
- `0x06`
  - write single register
- `0x10`
  - write multiple registers

### Request formats

Read holding registers:

```text
[slave][0x03][addr_hi][addr_lo][count_hi][count_lo][crc_lo][crc_hi]
```

Write single register:

```text
[slave][0x06][addr_hi][addr_lo][value_hi][value_lo][crc_lo][crc_hi]
```

Write multiple registers:

```text
[slave][0x10][start_hi][start_lo][count_hi][count_lo][byte_count][data...][crc_lo][crc_hi]
```

The RT manual's examples on PDF pages 142-145 and the JMC Modbus manual pages 175-177 use this same structure.

### CRC

CRC is standard Modbus CRC16:

- initial value: `0xFFFF`
- polynomial: `0xA001`
- low byte transmitted first

The RT manual even prints the CRC reference function in its communication chapter on PDF page 146, and it matches the algorithm implemented in `src/comm.cpp`.

### Register numbering conventions from the manuals

Both vendor manuals describe a simple relationship between parameter numbers and Modbus register addresses:

- JMC Modbus manual example:
  - `p03-09` maps to decimal register `309`
- RT manual example:
  - `P08.02` maps to decimal register `802`

This matters because many of the register numbers used by the firmware are not random.

For the RT path especially, several code-level register addresses clearly match the manual's decimalized parameter numbering:

- `503` -> `P05.03`
- `514` -> `P05.14`
- `515` -> `P05.15`
- `1300` -> `P13.00`
- `1301` -> `P13.01`
- `1303` -> `P13.03`

### Transport timing

Key timing constants from `src/definitions.h`:

- `COMM_DELAY_SEND = 60 ms`
- `COMM_DELAY_RECEIVE = 50 ms`
- `COMMINTERVAL = 60 ms`
- `COMM_DELAY_IDLE = 1000 ms`

Behavior:

- when idle, the firmware polls more slowly
- while active grinding is in progress, it polls much faster

### Health tracking

`commCounter` is the main transport-health metric.

- incremented whenever a request is sent
- decremented whenever a valid response is parsed

If it grows too large, the firmware treats the bus as unhealthy and stops the motor.

### Error responses

If the drive returns a 5-byte exception/error frame:

- the firmware stores the drive error code in `errorCode`
- it sets application error `3`
- the display shows "Drive fault"

## Motor Control Abstraction

The rest of the application does not send raw serial frames directly. Instead it uses shared motor variables:

- `motor_setRPM`
- `motor_currentRPM`
- `motor_currentTorque`
- `commStatus`
- `currentStatus`
- `newData`

This separation is important:

- UI and application logic write desired intent into `motor_setRPM`
- the drive layer decides what register sequence is needed to realize that intent

That makes the top-level application mostly drive-agnostic.

## JMC Drive Protocol

This is the active implementation in the current source tree.

### Vendor manual basis

The JMC protocol explanation in this section uses two different JMC documents for two different layers:

- `Documentation/JAND-D 15002 Servo drive.pdf`
  - for the serial Modbus RTU transport format
- `Documentation/JMC Manual RU.pdf`
  - for the meaning of objects such as `0x6040`, `0x6041`, `0x606C`, and `0x6077`

Important inference:

- the JMC object semantics in the manual are described in the EtherCAT / CoE / CiA402 chapter
- this firmware does not speak EtherCAT
- however, the object IDs and state values line up with what the firmware reads and writes over RS-485

So the best working model is:

- transport layer
  - Modbus RTU over RS-485
- object semantics
  - CiA402-style JMC object dictionary

That inference is based on the code plus the manual, not on a single manual sentence that says exactly that.

### High-level model

The JMC path is torque-mode based.

The firmware does not simply write "run at RPM". Instead it configures:

- torque control mode
- torque command source
- speed limit source
- torque target
- speed limit
- control word

The drive then enforces the speed limit while running in torque mode.

The JMC manual's communication chapter states that its object dictionary follows CiA402 / CiA301 conventions, and the objects the firmware uses match that model:

- `0x6040`
  - control word
- `0x6041`
  - status word
- `0x606C`
  - actual speed
- `0x6077`
  - actual torque

### Initialization sequence

When disconnected, the firmware attempts to establish first contact by writing:

- `0x6040 = 1`

If that succeeds, it reads `0x1010` and checks for the magic value:

- word 0: `0x6576`
- word 1: `0x6173`

This is the string `"Save"` split across two 16-bit registers.

If that magic value is not present, the firmware treats the drive as not yet configured for this project and writes the required parameters.

This `0x1010` save-marker behavior is project-specific firmware logic. It is not one of the standard CiA402 objects discussed in the manual section used here.

### JMC register roles used by the firmware

Core state and feedback:

- `0x6040`
  - control word
- `0x6041`
  - status word
- `0x606C`
  - actual velocity
- `0x6077`
  - actual torque

Manual-backed meanings:

- `0x6040`
  - control word used to enable output, stop, start, and fault-reset state transitions
- `0x6041`
  - read-only status word reflecting the current drive state
- `0x606C`
  - actual speed in `r/min`
- `0x6077`
  - actual torque in `‰` of rated torque

Configuration and persistence:

- `0x1010`
  - save marker used by this firmware
- `0x2101`
  - mode, set to torque control
- `0x2102`
  - rigidity source selection
- `0x2103`
  - rigidity amount
- `0x2121`
  - quick-stop torque
- `0x2203`
  - velocity feed-forward gain
- `0x2204`
  - velocity feed-forward filter or related parameter

Torque-mode command path:

- `0x2500`
  - torque command source
- `0x2501`
  - speed limit source
- `0x2502`
  - speed limit setpoint
- `0x2503`
  - torque setpoint
- `0x2510`
  - forward torque limit
- `0x2511`
  - reverse torque limit

### JMC initialization values written by the firmware

- `0x2101 = 2`
  - torque control mode
- `0x2102 = 1`
  - semi-auto rigidity source
- `0x2103 = Menu1[SETMOTORRAMP]`
  - rigidity/ramp level chosen in menu
- `0x2121 = 30`
  - quick-stop torque
- `0x2203 = 0`
- `0x2204 = 2000`
- `0x2500 = 1`
  - torque command source
- `0x2501 = 1`
  - speed limit source
- `0x2502 = 0`
- `0x2503 = 0`
- `0x2510 = +torque_limit`
- `0x2511 = -torque_limit`

### Runtime command sequence

The JMC manual's control-word table shows these documented CiA402-style transition words:

- `0x0006`
  - shutdown / holding-brake state
- `0x0007`
  - output voltage / unlocked
- `0x000F`
  - power-on enable / operation enable
- `0x0080`
  - fault reset

That is useful when comparing firmware behavior to the manual.

#### Stop

When `motor_setRPM == 0`, the firmware drives the JMC controller toward:

- `0x2503 = 0`
  - zero torque
- `0x2502 = 0`
  - zero speed limit
- `0x6040 = 3`
  - servo disable / non-enabled state used by this project

Note:

- `0x0003` is a real value used by the firmware and appears to work in practice
- it is not one of the explicit state-transition examples printed in the extracted JMC manual table
- treat it as project-proven behavior rather than a value directly documented in the cited table

#### Forward rotation

When `motor_setRPM > 0`, the firmware drives the controller toward:

- `0x2502 = motor_setRPM`
- `0x6040 = 0x000F`
  - enable operation
- `0x2503 = +Menu1[SETMOTORTORQUE]`

Interpretation:

- set the speed ceiling
- enable the servo
- command positive torque

#### Reverse rotation

When `motor_setRPM < 0`, the firmware drives the controller toward:

- `0x2502 = abs(motor_setRPM)`
- `0x6040 = 0x000F`
- `0x2503 = -Menu1[SETMOTORTORQUE]`

Interpretation:

- same speed ceiling mechanism
- same enable word
- negative torque commands reverse direction

### Status polling

Once command registers are in the expected state, the firmware alternates reads of:

- `0x6041`
  - status word
- `0x606C`
  - velocity
- `0x6077`
  - torque

Manual-backed units:

- `0x606C`
  - rpm
- `0x6077`
  - per-mille of rated torque, not raw physical torque in N·m

That is why the code's "percent" style calculations are only an application-level approximation.
The display and purge logic are using the returned value as a convenient relative load metric.

### JMC status word decoding

The firmware masks the JMC status word with `0x006F` and maps the result into:

- switch on disabled
- ready to switch on
- switched on
- operation enabled
- fault
- fault reaction active
- quick stop active

Those values match the JMC manual's status-word state table:

- `0x0021`
  - ready for operation
- `0x0023`
  - can start / switched on
- `0x0027`
  - run enable / operation enabled
- `0x0007`
  - quick stop effective
- `0x0008`
  - fault state
- `0x000F`
  - fault operation / fault reaction active

Those are then converted to application-level states:

- `MOTOR_NOT_READY`
- `MOTOR_READY`
- `MOTOR_ENABLED`
- `MOTOR_FAULT`

### Example JMC frames

Enable operation:

```text
01 06 60 40 00 0F CRClo CRChi
```

Set speed limit to 1000 RPM:

```text
01 06 25 02 03 E8 CRClo CRChi
```

Set torque target to +100:

```text
01 06 25 03 00 64 CRClo CRChi
```

Read status word:

```text
01 03 60 41 00 01 CRClo CRChi
```

Read torque:

```text
01 03 60 77 00 01 CRClo CRChi
```

### Important JMC design implication

This project is using an industrial servo drive with a register protocol that looks CiA-402-like in parts. It is not sending a hobby-servo pulse train and it is not commanding a standalone BLDC driver directly.

## RT Drive Protocol

The RT implementation uses the same transport but a different register map.

### Vendor manual basis

The RT protocol explanation here combines:

- `Documentation/RT RS RS485 Series Servo User Manual V4.0.pdf`, PDF pages 140-147
  - Modbus RTU framing
- PDF pages 64-73 and 108-109
  - speed-mode / torque-mode parameter meanings
- PDF page 135
  - monitoring parameter meanings

Unlike the JMC section, the RT manual is more clearly parameter-oriented than object-dictionary-oriented.

### High-level model

The RT path uses:

- enable/disable register
- torque register
- separate forward and reverse speed limits
- jog command register

Manual-backed control concepts:

- `P01.00 = 2`
  - torque control mode
- `P05.14`
  - torque-mode forward internal speed limit in rpm
- `P05.15`
  - torque-mode reverse internal speed limit in rpm
- `P05.03`
  - digital-given torque in `0.1%`
- `P05.20`
  - communication-given torque in `0.001%`

Important design observation:

- the firmware does not appear to use the RT manual's dedicated communication-given torque parameter `P05.20`
- instead it writes register `503`, which matches `P05.03`
- so the project is effectively remote-writing the drive's torque setpoint parameter over Modbus, rather than using the manual's separate "communication given torque" path

### RT registers used by the firmware

Command path:

- `201`
  - enable or disable drive
- `503`
  - torque command
- `514`
  - forward speed limit
- `515`
  - reverse speed limit
- `525`
  - jog control
    - `0`
      - stop
    - `1`
      - forward
    - `2`
      - reverse

Status/feedback:

- `1300`
  - running state
- `1301`
  - current RPM
- `1302`
  - another feedback word, currently unused
- `1303`
  - motor torque

The following RT mappings are directly supported by the manual's parameter-number-to-register rule:

- `503`
  - `P05.03`, digital-given torque, unit `0.1%`
- `514`
  - `P05.14`, torque-mode forward internal speed limit, unit rpm
- `515`
  - `P05.15`, torque-mode reverse internal speed limit, unit rpm
- `1300`
  - `P13.00`, servo running status
- `1301`
  - `P13.01`, motor speed, unit rpm
- `1303`
  - `P13.03`, motor torque, unit `0.1%`

Registers `201` and `525` are used by the firmware as drive enable and jog-direction controls, but this documentation pass did not recover their exact manual text. Their runtime meaning is therefore taken from the code path rather than a confirmed extracted page.

### RT initialization

The project currently treats a successful write of:

- `514 = 0`
- `515 = 0`

as proof that communication has been established.

### Runtime command sequence

#### Stop

- write `514, 515 = 0, 0`
- write `525 = 0`
- write `201 = 0`

#### Forward

- write `201 = 1`
- write `503 = torque_limit * 10`
- write `514, 515 = motor_setRPM, 0`
- write `525 = 1`

Why `torque_limit * 10`:

- the UI stores torque as an integer percent
- the RT manual defines `P05.03` in units of `0.1%`
- multiplying by 10 converts firmware percent units into the drive's parameter units

#### Reverse

- write `201 = 1`
- write `503 = -(torque_limit * 10)`
- write `514, 515 = 0, abs(motor_setRPM)`
- write `525 = 2`

Why `514` and `515` are written separately:

- the RT manual defines separate forward and reverse internal speed limits in torque mode
- the firmware uses that directly instead of sending a single signed speed word

### RT polling

The firmware repeatedly reads:

- `1300`
- `1301`
- `1302`
- `1303`

and interprets them as:

- status
- RPM
- unused/ignored feedback
- torque

The RT manual's monitoring-parameter section confirms:

- `P13.00`
  - running status
- `P13.01`
  - actual motor speed in rpm
- `P13.03`
  - actual motor torque in `0.1%`, where `100%` corresponds to rated torque

That matches the code comments and the way the firmware consumes those values.

## Auto Purge Algorithm

The purge system is one of the more project-specific pieces of logic.

### Purpose

The controller wants to detect when beans are no longer passing through normally and then run a purge sequence to clear retained grounds.

### How it works

1. During calibration, the firmware records the normal unloaded or lightly loaded torque profile versus RPM.
2. During a manual grind, it compares live torque against the stored calibrated torque at the current RPM.
3. If torque exceeds a configured percentage threshold for enough consecutive frames, it marks `ready_purge = true`.
4. While grounds are still flowing, `lastActivity` keeps being refreshed.
5. Once flow appears to stop and the configured delay expires, it enters `PURGING`.

### Threshold structure

There are separate purge settings for:

- low RPM
- high RPM

Each has:

- required frame count
- torque scalar percentage

That lets the purge detector behave differently at slow and fast grind speeds.

### Purge sequence structure

The purge routine supports:

- initial delay at zero RPM
- forward purge
- optional reverse purge

The reverse purge is disabled by setting reverse RPM to `0`.

## Grind-by-Weight Design

The GBW system is built around a BLE scale, not a load cell wired into the ESP32.

### Connection model

The background scale task decides whether to connect based on the top-level state:

- in `IDLE_GBW` and `GRINDING_GBW`
  - connect and maintain
- in regular idle, regular grinding, and sleep
  - do not maintain the connection

### Saved pairing

The firmware can store a scale name and MAC address.

When "Save scale" is enabled in Menu 3:

- the current connected scale identity is copied into persistent storage

When it is disabled:

- the stored pairing is removed

### Shot lifecycle

When entering `GRINDING_GBW`, the firmware:

1. resets all shot-tracking state
2. verifies the scale is connected
3. tares if necessary
4. waits for tare stabilization
5. starts shot timing
6. begins motor control after the configured start delay

### Shot sample buffer

The project stores a rolling shot trace in:

- `_shot[300]`

Each element contains:

- time since shot start
- absolute weight in mg

This trace is used for:

- live prediction
- stop point estimation
- post-shot learning

### Prediction

Prediction combines:

- recent actual slope from the last few weight samples
- fallback model using:
  - `SpeedModifier`
  - GBW RPM

The system computes remaining grind time in ms and subtracts the configured offset.

When the predicted remaining time goes below zero:

- the firmware stops the motor

### Slow phase

If enabled:

- the motor runs at normal GBW RPM first
- once current weight crosses `setWeight - GBW_SLOW_MG`
- the firmware switches to `GBW_SLOW_RPM`

This is a simple two-stage feed strategy:

- bulk fill
- controlled finish

### Learning

After a proper shot, the firmware estimates:

- the weight at the moment the motor actually stopped
- the live grind speed near stop
- the shot overshoot

It then uses that to update:

- `GBW_SPEEDMOD`
- `GBW_OFFSET`

but only if the shot passes several sanity checks.

The code rejects obviously bad shots, for example:

- too much overshoot
- impossible offset value
- too-short sample window
- invalid speed estimate

### GBW failure modes

The code explicitly handles:

- no scale connected at start
- scale disconnect during shot
- no grounds detected after 3 seconds

## Calibration System

The calibration system builds the torque baseline used by purge detection.

### Procedure

1. Ensure the drive is connected.
2. Start at the minimum RPM point.
3. Wait a short stabilization period.
4. Average torque samples for a fixed time window.
5. Store the average in `calibrateArray`.
6. Move to the next RPM step.
7. Repeat until done.
8. Save the array to persistent storage.

### Step size

The step size is controlled by:

- `rpm_scalar = 25`

### Why it exists

Without calibration, the purge detector would not know what "normal" torque looks like at each RPM.

## Sleep and Wake Behavior

The firmware supports deep sleep after inactivity.

### Sleep entry

If the user-configured sleep time expires and the controller is not actively grinding:

- `state` becomes `SLEEPING`

The main loop then:

- turns off the LED
- stops the motor
- turns off the display
- asks the drive to sleep if supported
- enables deep sleep

### Wake source

The actual implemented wake source is the start button on GPIO 4.

There are constants for encoder wake pins in `src/definitions.h`, but the current code only configures EXT0 wake on the start button.

## Error Model

Application errors are defined centrally in `src/main.h`.

### Fatal or blocking errors

- `1`
  - could not connect to drive
- `2`
  - drive connection lost
- `3`
  - drive returned error frame
- `4`
  - packet loss / communication discrepancy
- `5`
  - failed to create mutex
- `6`
  - motor stalled
- `7`
  - initial write/configuration failure
- `8`
  - first boot or drive not fully configured yet

### Non-fatal or recoverable errors

- `101`
  - calibration could not start
- `102`
  - calibration cancelled
- `103`
  - stored data load issue
- `104`
  - scale disconnected during GBW
- `105`
  - no grounds detected after 3 seconds
- `106`
  - no scale connected
- `107`
  - drive reboot needed after rigidity change

## Common Development Tasks

### Change a pin or timing constant

Edit:

- `src/definitions.h`

Examples:

- RS-485 timing
- display pinout
- default RPMs
- default purge settings

### Change menu items or defaults

Edit:

- `src/main.h`
  - enums
- `src/main.cpp`
  - `Menu1`, `Menu2`, `Menu3`
- `src/definitions.h`
  - default values if needed

Persistence mostly follows automatically because the menu arrays carry preference keys.

### Change screen layout or text

Edit:

- `src/display.cpp`

### Change encoder or button behavior

Edit:

- `src/io.cpp`

### Change serial transport behavior

Edit:

- `src/comm.cpp`
- `src/comm.h`

Examples:

- baud rate
- timing
- CRC handling
- request/response parsing

### Change drive register protocol

Edit:

- `src/motorcontrol_jmc.cpp`
  - for current build
- `src/motorcontrol_rt.cpp`
  - for RT variant

### Change GBW logic

Edit:

- `src/gbw.cpp`

Examples:

- prediction
- slow phase threshold
- learning logic
- scale connection behavior

### Change what is stored in NVS

Edit:

- `src/pdata.cpp`

If the new storage layout is incompatible with what existing boards already have, bump `BUILD_ID` in `src/definitions.h`.

## Known Quirks and Technical Debt

This section is important for new contributors. A few things in the code are surprising and can waste time if you assume they are cleaner than they are.

### 1. `src/JMC/comm_jmc.cpp` is not active

That file is wrapped in:

```cpp
#ifdef JMC_DRIVE_n
```

Since `JMC_DRIVE_n` is not defined, that file does not build. The active transport for both drive variants is `src/comm.cpp`.

### 2. The "servo protocol" is really industrial drive register control

This project does not command a hobby servo.

What the code calls the servo protocol is:

- RS-485 register reads and writes
- a CiA-402-like JMC register model
- or a custom RT register map

### 3. `default_sleepTime` comment is misleading

The menu labels sleep time in minutes and the sleep check multiplies by `60000`, so this setting behaves as minutes.

### 4. Some constants and comments are stale

Examples:

- comments mention packet-loss thresholds that do not match the actual code
- some wake-pin constants exist but are not used
- some read comments do not exactly match the request sizes

Treat the executable logic as the source of truth, not the comments.

### 5. Calibration array sizing is confusing

`SIZEOFCALIBRATEARRAY` is used as if it were a byte count in some places and an element count in others.

That means any contributor touching calibration storage should review:

- declaration size
- iteration counts
- NVS byte counts

before changing it.

### 6. The drive is expected to be partially configured by firmware

The JMC path writes a firmware-specific save marker and configuration values into the drive. That means the drive is not treated as a fixed black box. The firmware and drive setup are coupled.

## Suggested Onboarding Path for New Contributors

If you are completely new to the project, the best reading order is:

1. `src/definitions.h`
   - know the hardware and defaults
2. `src/main.h`
   - know the states and menus
3. `src/main.cpp`
   - understand the top-level loop
4. `src/io.cpp`
   - understand how user actions change state
5. `src/comm.cpp`
   - understand the transport
6. `src/motorcontrol_jmc.cpp`
   - understand the active motor protocol
7. `src/gbw.cpp`
   - understand the second major feature set
8. `src/display.cpp`
   - understand how the UI reflects system state
9. `src/pdata.cpp`
   - understand persistence and migration

## Quick Mental Model

If you only remember one architectural summary, use this one:

- `setRPM` and `setWeight` are user-facing targets
- `state` chooses which workflow is active
- `motor_setRPM` is the actual command handed to the drive layer
- `do_comm()` turns `motor_setRPM` into drive register traffic
- `motor_currentRPM` and `motor_currentTorque` come back from the drive
- `newData` tells purge and calibration code that fresh torque data arrived
- `gbwVitals()` maintains the BLE scale in the background
- `do_gbw()` uses live weight feedback to decide when to slow and stop
- `pdata_*()` makes settings survive reboots
- `display.cpp` turns all of that into the UI

## Reference Documents

Vendor and hardware references already included in this repository:

- `Documentation/JMC Manual RU.pdf`
- `Documentation/JAND-D 15002 Servo drive.pdf`
- `Documentation/JAND-P28_DB44 Use manual.pdf`
- `Documentation/RT RS RS485 Series Servo User Manual V4.0.pdf`
- `Documentation/Instructions for use.pdf`

Most useful protocol pages:

- `Documentation/JAND-D 15002 Servo drive.pdf`
  - PDF pages 173-177 for Modbus RTU framing examples
- `Documentation/JMC Manual RU.pdf`
  - PDF page 434 for CiA402 / object-dictionary context
  - PDF page 450 for the CiA402 state-machine overview
  - PDF pages 467-470 for `0x6040` and `0x6041`
  - PDF page 475 for `0x606C`
  - PDF page 478 for `0x6077`
- `Documentation/RT RS RS485 Series Servo User Manual V4.0.pdf`
  - PDF pages 64-73 for torque-mode and command-source behavior
  - PDF pages 108-109 for `P05.14`, `P05.15`, and `P05.20`
  - PDF page 135 for `P13.00`, `P13.01`, and `P13.03`
  - PDF pages 140-147 for Modbus RTU framing and CRC

Those documents should be used when:

- confirming exact vendor register semantics
- checking wiring details
- validating alarm meanings
- extending support for drive features not already implemented here

## Final Notes

For new project work:

- use `src/comm.cpp` and the active `motorcontrol_*.cpp` file as protocol truth
- treat `src/definitions.h` as the hardware and behavior configuration root
- bump `BUILD_ID` when changing persisted settings layout
- be careful with calibration storage and timing assumptions
- remember that manual grind, auto purge, and GBW are three different workflows layered on the same motor abstraction
