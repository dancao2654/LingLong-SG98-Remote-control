# How To Add Support For A New Motor

This guide explains the current motor architecture in plain language and shows exactly how to add a new motor backend.

It is written for someone who is new to this repo and just wants a safe checklist to follow.

If you want the copy-and-fill version that focuses on "where do I put the motor-specific commands?", read:

- `Documentation/New-Motor-Backend-Implementation-Template.md`

## What The New Architecture Looks Like

The controller firmware now treats each motor family as a backend.

The shared layer handles:

- motor selection
- auto-detect
- menu-to-command mapping
- common state and telemetry access

Each backend handles:

- how to probe the drive
- how to initialize it
- how to send run/stop commands
- how to read live RPM and torque
- how to sleep or reset it

The important files are:

- `src/motor_backend.h`
  - the backend contract every motor must implement
- `src/motor_hal.h`
  - shared enums, probe report, command descriptors, and motor descriptor shape
- `src/motor_hal.cpp`
  - the backend registry and auto-detect logic
- `src/motor_probe.h`
  - helper APIs for safe probing
- `src/motor_probe.cpp`
  - shared register read/write probe helpers
- `src/comm.h`
  - communication profiles
- `src/comm.cpp`
  - baud, parity, and timing behavior for each profile
- `src/JMC/motorcontrol_jmc.h` and `src/JMC/motorcontrol_jmc.cpp`
  - example backend with multi-baud probing and a more complex state machine
- `src/RT/motorcontrol_rt.h` and `src/RT/motorcontrol_rt.cpp`
  - example backend with a simpler register-style protocol

## If You Only Remember 3 Things

1. Start by copying an existing backend. Do not build a new one from scratch unless you really have to.
2. Keep all drive-specific logic inside the new backend and `comm.cpp`. Do not scatter it through the UI or app code.
3. Make the probe very specific. A bad probe is the fastest way to break auto-detect for every motor.

## Before You Touch The Code

Collect this from the motor manual first:

- default RS-485 baud rate
- parity and stop-bit requirements
- slave ID or addressing rules
- one safe read-only register that always exists
- one safe write register you can use for confirmation, if needed
- run command registers
- stop command registers
- actual RPM register
- actual torque or load register
- fault or status register
- direction handling
  - signed speed, or separate forward/reverse registers
- whether settings need an explicit save command
- whether settings need a drive reboot to take effect

If you do not have those answers first, implementation will be much slower.

## Pick The Best Starting Template

Use `RT` as your template if the new motor:

- uses one fixed baud rate
- uses straightforward register reads and writes
- behaves like a normal jog-speed or parameter-driven servo

Use `JMC` as your template if the new motor:

- has multiple supported baud rates
- uses a state machine or CiA402-style objects
- needs more than one register family to probe safely

Most new drives should probably start by copying `src/RT/motorcontrol_rt.h` and `src/RT/motorcontrol_rt.cpp`.

## Step 1: Create The New Backend Files

Create a new folder for the family, then copy the closest backend.

Example:

```text
src/FOO/motorcontrol_foo.h
src/FOO/motorcontrol_foo.cpp
```

Rename the symbols to match the new family:

- `motor_rt_probe_backend` -> `motor_foo_probe_backend`
- `motor_rt_init_backend` -> `motor_foo_init_backend`
- `motor_rt_do_comm_backend` -> `motor_foo_do_comm_backend`
- `motor_rt_off_backend` -> `motor_foo_off_backend`
- `motor_rt_calibrate_backend` -> `motor_foo_calibrate_backend`
- `motor_rt_sleep_backend` -> `motor_foo_sleep_backend`
- `MOTOR_BACKEND_RT` -> `MOTOR_BACKEND_FOO`

If the new drive supports reset, also add:

- `motor_foo_reset_backend`

Your header should end up looking like this shape:

```cpp
#pragma once

#include "motor_backend.h"

struct MotorProbeReport;

bool motor_foo_probe_backend(MotorProbeReport* report);
void motor_foo_init_backend();
bool motor_foo_do_comm_backend();
bool motor_foo_off_backend();
bool motor_foo_calibrate_backend();
void motor_foo_sleep_backend();
void motor_foo_reset_backend();

extern const MotorBackendDefinition MOTOR_BACKEND_FOO;
```

If reset is not supported, you can omit the function and use `nullptr` in the backend metadata, just like RT does today.

## Step 2: Add A New Drive Type

Open `src/motor_hal.h`.

Add a new `DriveType` entry:

```cpp
enum DriveType {
    DRIVE_TYPE_UNKNOWN,
    DRIVE_TYPE_JMC,
    DRIVE_TYPE_RT,
    DRIVE_TYPE_FOO,
};
```

Then extend `MotorProbeReport` with only the fields needed to identify the new drive.

Example:

```cpp
struct MotorProbeReport {
    bool jmc_status_word;
    bool jmc_magic_word;
    bool jmc_control_word;
    bool rt_status_block;
    bool foo_status_block;
    uint32_t jmc_baud;
};
```

Keep this struct small. It is only there to remember what the probe saw.

## Step 3: Register The Backend In The Shared Table

Open `src/motor_hal.cpp`.

### 3A. Include the new header

Add the include near the other backend headers:

```cpp
#include "FOO/motorcontrol_foo.h"
```

### 3B. Add a command table

Add a `MotorCommandDescriptor` array for the new family.

This is where you say which shared menu items the drive supports.

The four shared commands are:

- `MOTOR_COMMAND_SET_RPM`
- `MOTOR_COMMAND_TORQUE_LIMIT`
- `MOTOR_COMMAND_RAMP_LEVEL`
- `MOTOR_COMMAND_DIRECTION`

For each command you choose:

- whether it is supported
- what menu item controls it
- how UI values scale before writing to the drive
- help text shown by shared code

Use the existing tables `JMC_COMMANDS` and `RT_COMMANDS` as the model.

Important rules:

- if the drive does not support a command, set `supported` to `false`
- do not fake support just to make the menu look complete
- if a command is not in a menu, use the same `MOTOR_MENU_NONE` pattern used by existing backends

### 3C. Add a `MotorDescriptor`

Add a new entry to `MOTOR_DESCRIPTORS[]`.

You must fill:

- `driveType`
- `key`
- `label`
- `description`
- `guidance`
- `commProfile`
- `maxRpm`
- `torquePercentNumerator`
- `probePriority`
- `commands`
- `commandCount`
- `backend`

What those fields mean:

- `key`
  - short stable ID like `"foo"`
- `label`
  - short UI label like `"FOO"`
- `commProfile`
  - which serial timing/parity profile to use
- `maxRpm`
  - the shared UI clamp for this motor family
- `torquePercentNumerator`
  - scaling used when converting raw torque to percent
- `probePriority`
  - higher number wins if more than one backend claims the bus

Set `probePriority` carefully.

Today:

- `JMC` uses `10`
- `RT` uses `20`

So if your probe is more specific than RT, give it a higher number. If it is broad or uncertain, keep it lower.

## Step 4: Implement The Probe First

Do this before the runtime logic.

If auto-detect is wrong, everything else becomes confusing.

Use the helpers from `src/motor_probe.h`:

- `motor_probe_set_profile(...)`
- `motor_probe_read_register(...)`
- `motor_probe_write_register(...)`

Good probe rules:

- switch to the right comm profile first
- read a register that should exist only on this family
- return `true` only when the response is real and repeatable
- update the matching `MotorProbeReport` field so debug code can explain what matched
- avoid destructive writes during probe

Bad probe patterns:

- probing with a register that many unrelated drives also expose
- treating any response as success without checking register meaning
- leaving the serial port in a strange mode after the probe

If the drive supports several baud rates, follow the JMC pattern:

- build an ordered list of baud candidates
- try them one by one
- record which one succeeded
- restore the preferred/default profile when probe ends

## Step 5: Implement The Standard Hooks

Every backend follows the same API from `src/motor_backend.h`.

### `probe`

Purpose:

- decide if this drive is attached

What it should do:

- set the right comm profile
- perform the safest possible identification read
- fill `MotorProbeReport`
- return `true` only on a confident match

### `init`

Purpose:

- prepare the backend after it is selected

What it usually does:

- clear local cached state
- reset backend timers
- switch to the correct comm profile
- write one-time init registers if the manual requires it

### `tick`

Purpose:

- run the normal communication loop

This is the most important function.

It usually:

- reads drive status
- updates `motor_currentRPM`
- updates `motor_currentTorque`
- applies `motor_setRPM`
- handles direction rules
- updates global `currentStatus`
- resets or increments communication watchdog counters as needed

Keep `tick()` focused. It should be the one place where normal live motor traffic happens.

### `off`

Purpose:

- stop the motor safely right now

This must be reliable even if the drive was already running.

### `calibrate`

Purpose:

- run the motor-specific calibration or tuning action used by the existing controller flow

If the new drive has no true calibration feature, implement the safest small behavior that matches current app expectations.

### `sleep`

Purpose:

- leave the drive in a safe low-activity state before the controller sleeps

### `reset`

Purpose:

- reboot or reset the drive if supported

If unsupported, set the API hook to `nullptr`.

## Step 6: Add A New Comm Profile Only If You Really Need One

Do not add a new `CommProfile` unless the motor truly needs different serial behavior.

You only need a new profile when the new drive changes things like:

- baud rate
- parity
- stop bits
- send timing
- receive timing
- idle timing
- active poll interval

If you do need one:

1. Add the enum in `src/comm.h`.
2. Update `src/comm.cpp` in these functions:
   - `baudRateForProfile`
   - `serialConfigForProfile`
   - `sendDelayForProfile`
   - `receiveDelayForProfile`
   - `idleDelayForProfile`
   - `activeIntervalForProfile`
3. Point the new `MotorDescriptor.commProfile` to the new profile.

Keep profile logic inside `comm.cpp`.

Do not hardcode `Serial2.begin(...)` settings inside the backend itself.

## Step 7: Menu Mapping Is Usually Data, Not UI Code

In most cases you do not need to change the UI.

The shared menu system already asks `motor_hal.cpp` which commands are supported for the current drive.

That means most of the time all you need is the right `MotorCommandDescriptor` entries:

- correct `menuNum`
- correct `itemIdx`
- correct `valueScale`
- correct `supported`

Examples from existing drives:

- RT torque uses `valueScale = 10`
- JMC torque uses `valueScale = 1`
- RT ramp is marked unsupported

Only change UI code if the new motor needs a completely new user-facing control that does not already exist.

## Step 8: Keep Drive-Specific Knowledge In The Backend Metadata

At the bottom of each backend `.cpp` file there is a `MotorBackendDefinition`.

Fill it in carefully.

Those strings are not filler. They are the quickest explanation for the next person reading the code.

Include:

- what kind of backend this is
- how the probe works
- which command families matter
- where RPM and torque come from

That metadata should help a future maintainer understand the backend without reading the whole source file first.

## Step 9: Build In Small Steps

Do not wait until everything is "finished" before testing.

Recommended order:

1. Build after creating the new files and declarations.
2. Build again after adding the `DriveType` and descriptor registration.
3. Build again after the probe compiles.
4. Build again after `tick()` returns real telemetry.
5. Only then test run/stop and calibration behavior on hardware.

Useful controller build:

```bash
platformio run -e controller-universal-s3
```

If you are testing a specific controller board:

```bash
platformio run -e adafruit_feather_esp32s3
platformio run -e waveshare_esp32_s3_rs485_can
```

## Step 10: Hardware Test Checklist

Before calling the backend complete, verify all of this on a real unit:

- auto-detect picks the correct drive
- forcing the drive type also works
- stop works from idle and while running
- positive RPM spins the correct direction
- reversed direction setting really flips the sign
- torque control matches the expected scale
- actual RPM reads sensibly
- actual torque or load reads sensibly
- communication recovers after unplug/replug or power-cycle
- sleep does not leave the motor running
- reset works, or is correctly unavailable
- unsupported menu items stay unavailable

## Common Mistakes

- Copying a backend but forgetting to rename one exported symbol.
- Adding the backend files but forgetting to include the new header in `src/motor_hal.cpp`.
- Making the probe too generic and stealing another motor's bus.
- Hardcoding backend-specific serial settings outside `comm.cpp`.
- Marking a command as supported even though the drive cannot really do it.
- Forgetting that higher `probePriority` wins.
- Writing to a destructive register during probe.
- Changing shared UI code when a descriptor-table change would have been enough.

## A Very Safe Work Pattern

If you want the lowest-risk path, use this order exactly:

1. Copy `RT`.
2. Rename symbols.
3. Add `DRIVE_TYPE_FOO`.
4. Register `MOTOR_BACKEND_FOO` in `src/motor_hal.cpp`.
5. Make the probe work.
6. Make `off()` work.
7. Make RPM readback work.
8. Make live run control work.
9. Add torque, ramp, direction, save, sleep, and reset details.
10. Test on hardware.

This order keeps the system debuggable the whole time.

## What A Good Pull Request Should Include

- the new backend files
- the `DriveType` update
- the `MotorProbeReport` update, if needed
- the new command descriptor table
- the new `MotorDescriptor` registration
- any new `CommProfile` changes, only if required
- a short note naming the manual and the key probe/status registers used
- bench-test notes saying what was verified on hardware

## Final Rule

When in doubt, keep the shared layer generic and push motor-specific behavior down into the backend.

That is the whole point of the new architecture.
