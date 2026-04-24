# New Motor Backend Implementation Template

This is the detailed version of the motor-backend guide.

Use this when you already understand the architecture and now want the shortest path to a working backend by filling in motor-specific registers and commands.

The goal is simple:

- copy the `RT` backend
- rename it to your new motor family
- replace only the motor-specific registers and command sequences
- register the new backend in `motor_hal`

If you follow this file, you should not need to invent a new architecture.

## The Best Mental Model

Treat `src/RT/motorcontrol_rt.cpp` as the default template.

Most of the file is already generic enough to keep:

- disconnect handling
- unstable-comm handling
- stall handling
- `commStatus` and `currentStatus` updates
- the general send/receive loop shape
- the split between:
  - connect-time setup
  - async receive decode
  - stop commands
  - forward commands
  - reverse commands
  - polling reads

What you actually replace is the motor-specific content inside that shape:

- register addresses
- register meanings
- probe read or write
- connect-time setup writes
- forward and reverse command writes
- telemetry decode
- stop, sleep, and reset writes

## The Exact Files You Will Touch

Always expect to touch these files:

- `src/FOO/motorcontrol_foo.h`
- `src/FOO/motorcontrol_foo.cpp`
- `src/motor_hal.h`
- `src/motor_hal.cpp`

Touch these only if the drive really needs different serial behavior:

- `src/comm.h`
- `src/comm.cpp`

Use these existing files as the truth:

- `src/RT/motorcontrol_rt.h`
- `src/RT/motorcontrol_rt.cpp`
- `src/JMC/motorcontrol_jmc.h`
- `src/JMC/motorcontrol_jmc.cpp`
- `src/motor_backend.h`
- `src/motor_probe.h`
- `src/motor_probe.cpp`

## What You Must Collect From The Motor Manual

Before you edit any code, fill in this worksheet:

```text
Motor family label: ______________________________
Protocol style: RT-like / JMC-like / other

Default baud: ___________________________________
Parity: _________________________________________
Stop bits: ______________________________________
Slave ID rules: _________________________________

Probe register: _________________________________
Probe register count: ___________________________
Probe expected meaning: _________________________

Enable register + value: ________________________
Disable register + value: _______________________

Forward run command: ____________________________
Reverse run command: ____________________________
Stop command: __________________________________

Speed command register(s): ______________________
Direction register or rule: _____________________
Torque register(s): _____________________________
Ramp register(s): _______________________________

Status register(s): _____________________________
Actual RPM register(s): _________________________
Actual torque register(s): ______________________
Fault register(s): ______________________________

Sleep command: _________________________________
Reset command: _________________________________
Save-to-flash command: _________________________
Needs reboot after setup changes: yes / no
```

If you cannot fill in most of that sheet, stop and get the manual details first.

## Which Comm Functions Mean What

These are the transport helpers you will actually use.

From `src/comm.cpp`, the current behavior is:

- `writeConfirm(reg, value)`
  - blocking
  - sends a single-register write and waits for a matching response
  - use for probe, one-time setup, `off()`, `sleep()`, and `reset()`
- `writeMultipleConfirm(reg, count, ...)`
  - blocking
  - sends a multi-register write and waits for a matching response
  - use for one-time setup or stop sequences when the drive requires several adjacent registers
- `readReturn(reg, count)`
  - blocking
  - sends a read and waits for a response
  - returns `0xFFFF` on failure
  - use for probe and connect-time checks
- `writeSingleRegister(reg, value)`
  - non-blocking send
  - only confirms that the frame was sent
  - use inside `tick()` for normal runtime traffic
- `writeMultipleRegisters(reg, count, ...)`
  - non-blocking send
  - use inside `tick()` for normal runtime traffic
- `readRegister(reg, count)`
  - non-blocking send
  - use inside `tick()` to poll status, RPM, torque, and fault data
- `receive()`
  - call this inside `tick()` to collect the response from the last async request
  - if successful, `lastRead[]` contains the decoded register values

The rule of thumb is:

- blocking helpers for probe and one-time setup
- async helpers for the live comm loop

## Step 1: Copy RT And Rename Everything

Create:

```text
src/FOO/motorcontrol_foo.h
src/FOO/motorcontrol_foo.cpp
```

Start by copying:

- `src/RT/motorcontrol_rt.h`
- `src/RT/motorcontrol_rt.cpp`

Then rename these symbols:

```text
motor_rt_probe_backend      -> motor_foo_probe_backend
motor_rt_init_backend       -> motor_foo_init_backend
motor_rt_do_comm_backend    -> motor_foo_do_comm_backend
motor_rt_off_backend        -> motor_foo_off_backend
motor_rt_calibrate_backend  -> motor_foo_calibrate_backend
motor_rt_sleep_backend      -> motor_foo_sleep_backend
MOTOR_BACKEND_RT            -> MOTOR_BACKEND_FOO
```

If the new drive supports reset, also add:

```text
motor_foo_reset_backend
```

## Step 2: Replace The Top-Of-File Register Mirrors

At the top of `src/RT/motorcontrol_rt.cpp`, the current backend keeps:

- a few helper constants in an anonymous namespace
- confirmed register mirrors such as `reg_201`, `reg_503`, `reg_514`
- optional `pending_reg_*` values for multi-register writes

Do the same for the new motor.

Use readable names tied to the motor function, not the old RT register numbers.

Example:

```cpp
#include "motorcontrol_foo.h"
#include "motor_hal.h"
#include "motor_probe.h"
#include "main.h"
#include "pdata.h"
#include "comm.h"
#include "display.h"
#include "error_history.h"

namespace {
constexpr uint8_t FOO_STATUS_PROBE_RETRIES = 2;

constexpr uint16_t FOO_REG_STATUS = 0x0000;         // TODO
constexpr uint16_t FOO_REG_RPM = 0x0000;            // TODO
constexpr uint16_t FOO_REG_TORQUE = 0x0000;         // TODO
constexpr uint16_t FOO_REG_ENABLE = 0x0000;         // TODO
constexpr uint16_t FOO_REG_DISABLE = 0x0000;        // TODO if different
constexpr uint16_t FOO_REG_RUN_MODE = 0x0000;       // TODO if needed
constexpr uint16_t FOO_REG_SPEED_FWD = 0x0000;      // TODO if separate forward speed
constexpr uint16_t FOO_REG_SPEED_REV = 0x0000;      // TODO if separate reverse speed
constexpr uint16_t FOO_REG_SPEED_ABS = 0x0000;      // TODO if one absolute speed register
constexpr uint16_t FOO_REG_DIRECTION = 0x0000;      // TODO if the drive uses a direction register
constexpr uint16_t FOO_REG_TORQUE_FWD = 0x0000;     // TODO if split torque limits
constexpr uint16_t FOO_REG_TORQUE_REV = 0x0000;     // TODO if split torque limits
constexpr uint16_t FOO_REG_RAMP = 0x0000;           // TODO if supported
constexpr uint16_t FOO_REG_FAULT = 0x0000;          // TODO if available

uint16_t motor_currentStatus = 0;
}

// Confirmed register mirrors
int16_t reg_enable = -1;
int16_t reg_run_mode = -1;
int16_t reg_speed_fwd = -1;
int16_t reg_speed_rev = -1;
int16_t reg_speed_abs = -1;
int16_t reg_direction = -1;
int16_t reg_torque = -1;
int16_t reg_torque_fwd = -1;
int16_t reg_torque_rev = -1;
int16_t reg_ramp = -1;

// Pending mirrors for async multi-register writes
int16_t pending_reg_speed_fwd = 0xFFFF;
int16_t pending_reg_speed_rev = 0xFFFF;
```

If the drive does not use some of those patterns, delete the unused ones.

## Step 3: Build A Tiny Probe That Cannot Lie

Start with a helper very close to RT:

```cpp
namespace {
bool probeFooStatusBlock(MotorProbeReport* report) {
    if(report == nullptr) {
        return false;
    }

    motor_probe_set_profile(COMM_PROFILE_FOO); // or COMM_PROFILE_RT if reuse is fine

    report->foo_status_block = motor_probe_read_register(
        FOO_REG_STATUS,     // TODO
        1,                  // TODO number of registers
        FOO_STATUS_PROBE_RETRIES
    );

    return report->foo_status_block;
}
}
```

Then expose it:

```cpp
bool motor_foo_probe_backend(MotorProbeReport* report) {
    if(report == nullptr) {
        return false;
    }

    report->foo_status_block = false;
    return probeFooStatusBlock(report);
}
```

If the drive supports multiple baud rates, follow the JMC pattern instead:

1. save the original baud/profile
2. try each supported baud
3. run the same probe at each baud
4. restore the original setting before returning

## Step 4: `init()` Should Only Reset Local State

Start from this pattern:

```cpp
void motor_foo_init_backend() {
    motor_setRPM = 0;
    motor_currentRPM = 0;
    motor_currentTorque = 0;

    reg_enable = -1;
    reg_run_mode = -1;
    reg_speed_fwd = -1;
    reg_speed_rev = -1;
    reg_speed_abs = -1;
    reg_direction = -1;
    reg_torque = -1;
    reg_torque_fwd = -1;
    reg_torque_rev = -1;
    reg_ramp = -1;

    commStatus = COMM_DISCONNECTED;
    currentStatus = MOTOR_NOT_CONNECTED;
}
```

Do not do heavy setup here.

Keep one-time drive initialization in the `commStatus == COMM_DISCONNECTED` block inside `tick()`, because that path already retries and is part of the real comm flow.

## Step 5: Keep The Shape Of `tick()` And Replace Only The Motor Commands

This is the single most important rule.

Do not rewrite `tick()` from scratch.

Copy the RT `motor_rt_do_comm_backend()` structure and replace only the sections below.

### 5A. Leave The Generic Safety Blocks Mostly Alone

You can usually keep these with little or no change:

- disconnect detection
- first-boot "no connection yet" handling
- unstable comm handling via `commCounter`
- stall handling
- `check...RpmZeroLog()` style helpers if you keep them

Those blocks are backend-agnostic safety behavior.

### 5B. Replace The Connect-Time Setup Block

In RT, the first connect block starts when:

```cpp
if(commStatus == COMM_DISCONNECTED) {
```

That is where you put motor-specific one-time setup writes.

Template:

```cpp
if(commStatus == COMM_DISCONNECTED) {
    if(writeConfirm(FOO_REG_ENABLE, 0x0001)) {             // TODO
        // Optional blocking setup writes:
        // readReturn(...)
        // writeConfirm(...)
        // writeMultipleConfirm(...)

        if(writeConfirm(FOO_REG_RUN_MODE, 0x0003)) {       // TODO if needed
            reg_run_mode = 0x0003;
        }

        if(writeConfirm(FOO_REG_RAMP, configuredRamp)) {   // TODO if supported
            reg_ramp = configuredRamp;
        }

        reg_enable = 0x0001;
        lastCommReceived = millis();
        nextCommType = 0;
        commStatus = COMM_CONNECTED;
        currentStatus = MOTOR_NOT_READY;
        commCounter = 0;
    }

    lastCommSend = millis();
    return false;
}
```

Only put commands here that must happen once per connection.

Examples:

- set control mode
- set command source
- set speed or torque source
- confirm save marker
- set a default ramp profile

Do not put the normal live run commands here.

### 5C. Replace The Async Receive Decode

Inside `tick()`, after:

```cpp
int16_t received = receive();
if(received > 0) {
```

you must decode the responses for your motor.

The current backends use these buckets:

- `lastRequestType == 0`
  - async single-register write acknowledgements
- `lastRequestType > 100 && lastRequestType < 200`
  - async reads
- `lastRequestType > 200 && lastRequestType < 205`
  - async multi-register write acknowledgements

Template:

```cpp
if(lastRequestType == 0) {
    switch(received) {
        case FOO_REG_ENABLE:
            reg_enable = lastRead[0];
            break;
        case FOO_REG_TORQUE:
            reg_torque = lastRead[0];
            break;
        case FOO_REG_SPEED_FWD:
            reg_speed_fwd = lastRead[0];
            break;
        case FOO_REG_SPEED_REV:
            reg_speed_rev = lastRead[0];
            break;
        case FOO_REG_SPEED_ABS:
            reg_speed_abs = lastRead[0];
            break;
        default:
            break;
    }
}

if(lastRequestType > 100 && lastRequestType < 200) {
    if(received == FOO_REG_STATUS) {
        motor_currentStatus = lastRead[0];
        // TODO decode to MOTOR_READY / MOTOR_ENABLED / MOTOR_FAULT
        currentStatus = MOTOR_ENABLED;
    }

    if(received == FOO_REG_RPM) {
        motor_currentRPM = static_cast<int16_t>(lastRead[0]); // TODO decode width/sign correctly
    }

    if(received == FOO_REG_TORQUE) {
        motor_currentTorque = static_cast<int16_t>(lastRead[0]); // TODO decode and scale if needed
        newData = true;
    }

    if(received == FOO_REG_FAULT) {
        // TODO optional fault decode/logging
    }
}

if(lastRequestType > 200 && lastRequestType < 205) {
    if(received == FOO_REG_SPEED_FWD && lastRequestType == 202) {
        reg_speed_fwd = pending_reg_speed_fwd;
        reg_speed_rev = pending_reg_speed_rev;
    }
}
```

This is where you update the cached register mirrors so the send path knows which writes are still needed.

### 5D. Replace The Stop Branch

Use the existing `effectiveSetRPM == 0` branch.

That branch should drive the motor to a safe stopped state in the order required by the manual.

Template:

```cpp
if(effectiveSetRPM == 0) {
    if(reg_speed_fwd != 0 || reg_speed_rev != 0) {
        if(writeMultipleRegisters(FOO_REG_SPEED_FWD, 2, 0, 0)) {
            lastCommSend = millis();
            nextCommType = 0;
            pending_reg_speed_fwd = 0;
            pending_reg_speed_rev = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_run_mode != 0) {
        if(writeSingleRegister(FOO_REG_RUN_MODE, 0)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_enable != 0) {
        if(writeSingleRegister(FOO_REG_ENABLE, 0)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_enable == 0 && nextCommType == 0) {
        nextCommType = 1;
    }
}
```

If the drive wants a different stop order, change only the commands, not the overall pattern.

### 5E. Replace The Forward Branch

There are three common patterns.

#### Pattern 1: separate forward and reverse speed registers

This is the RT style.

```cpp
if(effectiveSetRPM > 0) {
    if(reg_enable != 1) {
        if(writeSingleRegister(FOO_REG_ENABLE, 1)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_torque != configuredTorque) {
        if(writeSingleRegister(FOO_REG_TORQUE, configuredTorque)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_speed_fwd != effectiveSetRPM || reg_speed_rev != 0) {
        if(writeMultipleRegisters(FOO_REG_SPEED_FWD, 2, effectiveSetRPM, 0)) {
            lastCommSend = millis();
            nextCommType = 0;
            pending_reg_speed_fwd = effectiveSetRPM;
            pending_reg_speed_rev = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_run_mode != 1) {
        if(writeSingleRegister(FOO_REG_RUN_MODE, 1)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_enable == 1 && reg_torque == configuredTorque &&
       reg_speed_fwd == effectiveSetRPM && reg_speed_rev == 0 &&
       reg_run_mode == 1 && nextCommType == 0) {
        nextCommType = 1;
    }
}
```

#### Pattern 2: one absolute speed register plus a direction register

```cpp
if(effectiveSetRPM > 0) {
    if(reg_direction != 0) {
        if(writeSingleRegister(FOO_REG_DIRECTION, 0)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_speed_abs != effectiveSetRPM) {
        if(writeSingleRegister(FOO_REG_SPEED_ABS, effectiveSetRPM)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_enable != 1) {
        if(writeSingleRegister(FOO_REG_ENABLE, 1)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }
}
```

#### Pattern 3: one absolute speed register plus signed torque

This is closer to JMC.

```cpp
if(effectiveSetRPM > 0) {
    if(reg_speed_abs != effectiveSetRPM) {
        if(writeSingleRegister(FOO_REG_SPEED_ABS, effectiveSetRPM)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_torque != configuredTorque) {
        if(writeSingleRegister(FOO_REG_TORQUE, configuredTorque)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }

    if(reg_enable != 1) {
        if(writeSingleRegister(FOO_REG_ENABLE, 1)) {
            lastCommSend = millis();
            nextCommType = 0;
            motor_lastSetRPM = motor_setRPM;
            return false;
        }
    }
}
```

### 5F. Replace The Reverse Branch

Mirror the same pattern as forward.

Examples:

- RT-style:
  - `speed_fwd = 0`
  - `speed_rev = abs(effectiveSetRPM)`
  - `run_mode = reverse`
- direction-register style:
  - `direction = reverse`
  - `speed_abs = abs(effectiveSetRPM)`
- signed-torque style:
  - `speed_abs = abs(effectiveSetRPM)`
  - `torque = configuredReverseTorque`

Use `motor_get_formatted_command_value(MOTOR_COMMAND_TORQUE_LIMIT, -1)` when reverse torque must be negative.

### 5G. Replace The Polling Reads

At the bottom of `tick()`, keep the same idea:

- after writes settle, poll status
- then poll RPM
- then poll torque
- then optional fault registers

Template:

```cpp
if(nextCommType == 1) {
    nextCommType++;
    readRegister(FOO_REG_STATUS, 1);
    lastCommSend = millis();
    return false;
}

if(nextCommType == 2) {
    nextCommType++;
    readRegister(FOO_REG_RPM, 1); // TODO use 2 if 32-bit
    lastCommSend = millis();
    return false;
}

if(nextCommType == 3) {
    nextCommType++;
    readRegister(FOO_REG_TORQUE, 1);
    lastCommSend = millis();
    return false;
}

if(nextCommType == 4) {
    nextCommType = 0;
    if(FOO_REG_FAULT != 0) {
        readRegister(FOO_REG_FAULT, 1);
        lastCommSend = millis();
        return false;
    }
}
```

## Step 6: Fill The Other Standard Hooks

### `off()`

Use blocking confirmed writes.

Template:

```cpp
bool motor_foo_off_backend() {
    uint8_t score = 0;

    for(int i = 0; i < 10; i++) {
        if(writeConfirm(FOO_REG_SPEED_ABS, 0)) {
            score++;
            break;
        }
        delay(200);
    }

    for(int i = 0; i < 10; i++) {
        if(writeConfirm(FOO_REG_ENABLE, 0)) {
            score++;
            break;
        }
        delay(200);
    }

    return score == 2;
}
```

### `sleep()`

If the drive has a lower-power or safer servo-off mode, put it here, then call `off()`.

### `reset()`

Only implement this if the manual gives a reliable reset or factory-reload path.

If unsupported, leave the function out and use `nullptr` in the backend definition.

### `calibrate()`

If the new motor does not need a custom calibration path, copy the existing backend that is closest to your torque feedback model and change as little as possible.

## Step 7: Register The Backend In `motor_hal`

### `src/motor_hal.h`

Add the new drive type:

```cpp
enum DriveType {
    DRIVE_TYPE_UNKNOWN,
    DRIVE_TYPE_JMC,
    DRIVE_TYPE_RT,
    DRIVE_TYPE_FOO,
};
```

Add only the probe fields you need:

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

### `src/motor_hal.cpp`

Add the include:

```cpp
#include "FOO/motorcontrol_foo.h"
```

Add a command table.

Template:

```cpp
const MotorCommandDescriptor FOO_COMMANDS[] = {
    {MOTOR_COMMAND_SET_RPM, MOTOR_MENU_NONE, MOTOR_MENU_NONE, 1, true,
        "Set RPM", "rpm", "Primary speed target for FOO.", "Keep the target inside the stable range."},

    {MOTOR_COMMAND_TORQUE_LIMIT, 1, static_cast<uint8_t>(SETMOTORTORQUE), 1, true,
        "Motor torque", "%", "Torque limit for FOO.", "Adjust this to match the manual's allowed range."},

    {MOTOR_COMMAND_RAMP_LEVEL, 1, static_cast<uint8_t>(SETMOTORRAMP), 1, false,
        "Motor ramp", "lvl", "Ramp support depends on FOO.", "Mark unsupported if the drive has no shared ramp concept."},

    {MOTOR_COMMAND_DIRECTION, 1, static_cast<uint8_t>(SETMOTORDIRECTION), 1, true,
        "Motor direction", "", "Sets the primary grind direction.", "Normal keeps forward positive. Reversed flips the command sign."},
};
```

Then add the descriptor:

```cpp
{
    DRIVE_TYPE_FOO,
    "foo",
    "FOO",
    "FOO servo using TODO control style.",
    "Short note about what the drive supports or does not support.",
    COMM_PROFILE_FOO, // or COMM_PROFILE_RT if reused
    MOTOR_MAX_RPM_RT, // TODO choose the right limit
    100,              // TODO copy the closest unit system first
    30,               // TODO set higher than broad probes, lower than more specific probes
    FOO_COMMANDS,
    static_cast<uint8_t>(sizeof(FOO_COMMANDS) / sizeof(FOO_COMMANDS[0])),
    &MOTOR_BACKEND_FOO,
},
```

## Step 8: How To Set `valueScale` And Torque Scaling

The shared layer formats command values here:

```cpp
formatted = uiValue * command->valueScale
```

That means:

- if the UI menu is in whole percent and the drive expects whole percent:
  - `valueScale = 1`
- if the UI menu is in whole percent and the drive expects `0.1%`:
  - `valueScale = 10`
- if the UI menu is in whole RPM and the drive expects whole RPM:
  - `valueScale = 1`

Examples already in the repo:

- RT torque uses `valueScale = 10`
- JMC torque uses `valueScale = 1`

For `torquePercentNumerator`, the easiest safe rule is:

1. copy the backend whose raw torque unit matches your new drive
2. bench-test whether the displayed torque percent looks sensible
3. adjust only if the UI percentage is clearly wrong

Existing examples:

- JMC uses `torquePercentNumerator = 10`
- RT uses `torquePercentNumerator = 100`

## Step 9: Add A New Comm Profile Only When Needed

If the new motor uses different:

- baud
- parity
- stop bits
- send timing
- receive timing
- idle poll timing

then add a new `CommProfile`.

If not, reuse `COMM_PROFILE_RT` or `COMM_PROFILE_JMC`.

When adding a new profile, update:

- `src/comm.h`
- `src/comm.cpp`
  - `baudRateForProfile`
  - `serialConfigForProfile`
  - `sendDelayForProfile`
  - `receiveDelayForProfile`
  - `idleDelayForProfile`
  - `activeIntervalForProfile`

## Step 10: Minimal Bring-Up Order

Do not try to solve every command at once.

Use this order:

1. Make the backend compile.
2. Make the probe work.
3. Make `off()` work.
4. Make status polling work.
5. Make RPM readback work.
6. Make positive RPM run.
7. Make negative RPM run.
8. Make torque limits work.
9. Add ramp, save, sleep, and reset.

That order keeps the grinder safe while you bring the drive up.

## What You Usually Do Not Need To Change

Most new backends do not require UI changes.

If the motor still fits the shared controls:

- RPM
- torque
- ramp
- direction

then the UI usually works as soon as:

- the command table is correct
- `tick()` writes the right motor registers
- `tick()` reads the right status, RPM, and torque registers

## Final Checklist Before Bench Test

- The probe touches only safe registers.
- The new backend updates its cached `reg_*` mirrors.
- The stop path works before the run path.
- Forward and reverse logic match the manual.
- Polling reads use the right width and sign for RPM.
- Torque decode matches the drive's documented unit.
- Unsupported commands are marked unsupported.
- `commProfile` is correct.

## If You Want The Lowest-Risk Implementation Path

Do exactly this:

1. Copy `src/RT/motorcontrol_rt.cpp`.
2. Rename the exported symbols.
3. Rename the register mirrors to motor-specific names.
4. Replace only:
   - probe
   - connect-time setup writes
   - receive decode
   - stop writes
   - forward writes
   - reverse writes
   - polling reads
   - `off()`, `sleep()`, and optional `reset()`
5. Register the backend in `motor_hal`.
6. Bench-test in the order listed above.

That is the closest thing to a "just fill in the motor commands" workflow in the current architecture.
