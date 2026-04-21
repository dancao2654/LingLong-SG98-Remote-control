# LingLong SG98 I/O Reference

This document explains what I/O is available on the SG98 controller board, what the current firmware already uses, what is likely safe for expansion, and where to find official reference pinout pictures online.

This board is a custom PCB built around an `ESP32-S3-WROOM-1-N8` module. That matters because:

- the custom board does not expose pins in the same way as a generic ESP32 dev board
- the firmware pin map in this repo is the most accurate source for what this board actually uses
- online pinout pictures are still useful, but they are reference material for the module or Feather board, not a guaranteed map of this custom PCB header order

## Trust Levels

Use this document with three levels of confidence:

1. Confirmed from this repo
   - BOM entries
   - firmware pin definitions
   - firmware features and active peripherals
2. Confirmed from official online documentation
   - Espressif module and chip pin/function documentation
   - Adafruit Feather ESP32-S3 reference pinout pictures
3. Not fully confirmed yet
   - exact left-to-right orientation on the board when viewed in every possible rotation
   - the `M12 4 pin female connector` net mapping

If you plan to make a cable or plug in a sensor, verify header pin order against the PCB CAD, Gerbers, continuity measurements, or a board photo before powering anything.

## Board Overview

### Main MCU Module

- BOM entry: `U18`
- Part: `ESP32-S3-WROOM-1-N8`
- Meaning:
  - ESP32-S3
  - 8 MB flash
  - no PSRAM on this specific module variant

That last point is important because official Espressif documentation says GPIO35, GPIO36, and GPIO37 are unavailable only on Octal PSRAM module variants. This board uses the `N8` no-PSRAM module, so those pins are available in principle.

## Confirmed External Interfaces On The Custom PCB

The following connectors or interfaces are visible in the repo BOM and/or firmware:

| Board surface | BOM / repo evidence | Used now | Intended role |
|---|---|---:|---|
| `USBC1` | USB-C connector in BOM | Yes | flashing, USB serial, Web Serial UI |
| `RS485` | 3-pin terminal block in BOM | Yes | servo-drive comms |
| `EX_I2C` | 4-pin header in BOM | No | external I2C expansion |
| `EX_SPI` | 6-pin header in BOM | No | external SPI expansion |
| `UART` | 3-pin header in BOM | No | serial expansion or debug |
| `SPI_DISPLAY` | 8-pin header in BOM | Yes | onboard display connection |
| Wi-Fi radio | ESP32-S3 | Yes | onboard AP and home Wi-Fi |
| BLE radio | ESP32-S3 | Yes | BLE scale and BLE console |

## Current Firmware Pin Map

These signals are explicitly defined in `src/definitions.h`.

### RS-485 Motor Bus

| Function | GPIO | Notes |
|---|---:|---|
| `RS485_TXD` | `17` | UART TX to RS-485 transceiver |
| `RS485_RXD` | `18` | UART RX from RS-485 transceiver |
| `RS485DE` | `16` | driver enable |
| `RS485RE` | `15` | receiver enable |

The board also has a `MAX3485ESA+` transceiver in the BOM, so the exposed `RS485` connector is a real differential RS-485 interface, not TTL UART.

### Front Panel Inputs

| Function | GPIO | Notes |
|---|---:|---|
| start button | `4` | wake source for deep sleep |
| start button LED | `5` | output |
| encoder button | `42` | input |
| encoder A | `41` | input |
| encoder B | `40` | input |

### Display Bus

| Function | GPIO | Notes |
|---|---:|---|
| display SCK | `10` | SPI clock |
| display MOSI / SDA | `11` | SPI data out |
| display reset | `12` | output |
| display DC | `13` | output |
| display CS | `14` | chip select |
| display backlight | `21` | PWM-capable brightness control |

The display is active in the current firmware. Do not assume these pins are available for add-ons.

### Expansion Headers In Firmware

#### `EX_I2C`

| Function | GPIO | Notes |
|---|---:|---|
| SDA | `8` | also `ADC1_CH7` |
| SCL | `9` | also `ADC1_CH8` |

#### `EX_SPI`

| Function | GPIO | Notes |
|---|---:|---|
| MOSI | `35` | digital only on this board |
| MISO | `37` | digital only on this board |
| SCLK | `36` | digital only on this board |
| CS | `39` | also a JTAG-capable pin |

These `EX_I2C` and `EX_SPI` pins are present in the firmware pin map but not used by the current application logic.

## Custom Header Pinouts Recovered From The PCB DXF

The local board DXF includes silkscreen text next to the headers. That gives a much stronger answer than firmware pin names alone.

### `EX_SPI` 6-pin Header

The board DXF contains the silkscreen label:

- `3V3_GND_35_37_36_39`

That strongly indicates the six positions on `EX_SPI` are:

1. `3V3`
2. `GND`
3. `GPIO35` = `EXSPI_MOSI`
4. `GPIO37` = `EXSPI_MISO`
5. `GPIO36` = `EXSPI_CLK`
6. `GPIO39` = `EXPSI_CS`

This is the first time the repo gives both signal names and likely power pins for that header.

### `EX_I2C` 4-pin Header

The board DXF contains the silkscreen label:

- `3V3_GND_GPIO9_GPIO8`

That strongly indicates the four positions on `EX_I2C` are:

1. `3V3`
2. `GND`
3. `GPIO9` = `EXSCL`
4. `GPIO8` = `EXSDA`

So the likely bus mapping is:

- `GPIO9` = `SCL`
- `GPIO8` = `SDA`

### `UART` 3-pin Header

Even though this guide is focused on `EX_SPI` and `EX_I2C`, the DXF also contains:

- `GND-TXD0-RXD0`

That strongly suggests the `UART` header is:

1. `GND`
2. `TXD0`
3. `RXD0`

On ESP32-S3 that corresponds to:

- `TXD0` = `GPIO43`
- `RXD0` = `GPIO44`

This header still is not used by the current firmware application logic, but the board artwork does appear to define it.

## How The Current Firmware Controls GPIO

The firmware does not toggle most pins manually in ad-hoc code. Instead, it uses four patterns:

1. Plain GPIO input/output
   - `pinMode()`
   - `digitalWrite()`
   - `digitalRead()`
2. PWM output through LEDC
   - start button LED
   - display backlight
3. Peripheral-driven pins
   - `Serial2` for the RS-485 UART path
   - hardware SPI for the display
4. Radio stacks and USB
   - Wi-Fi, BLE, and USB are not treated as regular user GPIO toggles

### Initialization Order

During boot, the important I/O initializers run in this order:

1. `display_init()`
2. `io_init()`
3. `comm_init()`
4. `motor_init()`
5. `diag_console_init()`
6. `wifi_ui_init()`

At runtime, the main loop services the active subsystems in this order:

1. motor/drive communication
2. diagnostic console
3. Wi-Fi UI
4. front-panel I/O
5. sleep checks
6. display update

That means the code conceptually treats GPIO as part of modules, not as one big pin-control file.

## GPIO Ownership Map

This table answers: what GPIO is it, what board function owns it now, and how the firmware uses it.

| GPIO | Board signal / role | Current owner | How firmware uses it | Safe to reuse? |
|---:|---|---|---|---|
| `4` | start button | `io.cpp` + sleep logic | input button, wake source from deep sleep | No |
| `5` | start button LED | `io.cpp` | LEDC PWM brightness and patterns | No |
| `8` | `EX_I2C` SDA | unused by current app | only declared in `definitions.h` | Yes |
| `9` | `EX_I2C` SCL | unused by current app | only declared in `definitions.h` | Yes |
| `10` | display SCK | `display.cpp` | hardware SPI clock | No |
| `11` | display MOSI | `display.cpp` | hardware SPI data out | No |
| `12` | display RST | `display.cpp` | display reset line | No |
| `13` | display DC | `display.cpp` | display data/command select | No |
| `14` | display CS | `display.cpp` | display chip select | No |
| `15` | `RS485RE` | `comm.cpp` + sleep logic | receive-enable for RS-485 transceiver | No |
| `16` | `RS485DE` | `comm.cpp` + sleep logic | driver-enable for RS-485 transceiver | No |
| `17` | `RS485_TXD` | `comm.cpp` | `Serial2` TX to motor bus | No |
| `18` | `RS485_RXD` | `comm.cpp` | `Serial2` RX from motor bus | No |
| `19` | USB / system pin | USB subsystem | reserved by USB/JTAG behavior | Avoid |
| `20` | USB / system pin | USB subsystem | reserved by USB/JTAG behavior | Avoid |
| `21` | display backlight | `display.cpp` | LEDC PWM dimming/backlight off in sleep | No |
| `35` | `EX_SPI` MOSI | unused by current app | only declared in `definitions.h` | Yes |
| `36` | `EX_SPI` CLK | unused by current app | only declared in `definitions.h` | Yes |
| `37` | `EX_SPI` MISO | unused by current app | only declared in `definitions.h` | Yes |
| `39` | `EX_SPI` CS | unused by current app | only declared in `definitions.h`; also JTAG-capable | Yes, with care |
| `40` | encoder B | `io.cpp` | quadrature encoder input | No |
| `41` | encoder A | `io.cpp` | quadrature encoder input | No |
| `42` | encoder push button | `io.cpp` | input button | No |
| `43` | `TXD0` on `UART` header | not used by current app | likely UART0 TX from board silk | Maybe |
| `44` | `RXD0` on `UART` header | not used by current app | likely UART0 RX from board silk | Maybe |

Notes:

- `GPIO8/9/35/36/37/39` are the cleanest expansion pins because the current firmware never references them outside `definitions.h`.
- `GPIO43/44` are likely the `UART` header pins, but using them means you are sharing or repurposing the chip's default UART0 path.
- `GPIO19/20` are tied to USB behavior by the ESP32-S3 platform and should not be treated as casual spare GPIO.

## Front Panel Controls And Their Functions

The front panel is built from one start button, one encoder button, one encoder rotation input, and one LED in the start button.

### Start Button

- GPIO: `4`
- Read path: `START_BUTTON()`
- Library: `JC_Button`
- Wake source: yes, for deep sleep

What it does in current firmware:

| State | Behavior |
|---|---|
| `IDLE` | start grinding |
| `IDLE_GBW` | start grind-by-weight |
| `GRINDING` | stop grinding, optionally enter purge depending on settings |
| `PURGING` | stop purge and return to idle |
| `GRINDING_GBW` | stop grind-by-weight and return to `IDLE_GBW` |
| `MENU1` / `MENU2` / `MENU3` / `MENU4` | back / cancel current edit / return |
| `MOTOR_SELECT` | back out of drive selector |
| `CALIBRATING` | abort calibration |
| `SLEEPING` | wake source through RTC EXT0 |

### Encoder Push Button

- GPIO: `42`
- Read path: `ENC_BUTTON()`
- Library: `JC_Button`
- Long-press detection: `pressedLong()`

What it does in current firmware:

| State | Short press | Long press |
|---|---|---|
| `IDLE` | open `MENU1` or drive selector | switch to `IDLE_GBW` |
| `IDLE_GBW` | open `MENU1` or drive selector | switch to `IDLE` |
| `GRINDING` | stop and return to idle | not used separately |
| `PURGING` | stop purge | not used separately |
| `GRINDING_GBW` | stop and return to `IDLE_GBW` | not used separately |
| `MENU1..MENU4` | enter/edit/confirm selections | long press only used for `CALIBRATE` in `MENU1` |
| `MOTOR_SELECT` | apply selected drive | not used separately |
| `CALIBRATING` | abort calibration after release logic allows it | not used separately |

### Encoder Rotation

- GPIOs: `41` and `40`
- Library: `ESP32Encoder`
- Attach mode: `attachHalfQuad()`

What rotation does:

| State | Behavior |
|---|---|
| `IDLE` | adjust `setRPM` |
| `IDLE_GBW` | adjust `setWeight` |
| `PURGING` | adjust `setRPM` while purging |
| `MENU1..MENU4` | move selection or edit current item |
| `MOTOR_SELECT` | move through drive options |

### Start Button LED

- GPIO: `5`
- Control style: LEDC PWM on channel `0`
- Frequency / resolution: `2 kHz`, `10-bit`

Patterns used by the firmware:

| Pattern code | Meaning |
|---:|---|
| `0` | off |
| `1` | solid on |
| `2` | flashing |
| `3` | fading / breathing |
| `4` | fast flashing |
| `5` | fastest flashing |
| `100..200` | direct brightness percentage-like values |

Typical states:

- idle modes: breathing
- grinding: flashing or fast flashing depending on purge readiness
- purging: solid on
- menu calibration item: flashing
- calibration running: solid on

## Subsystem-Level GPIO Usage

### RS-485 / Motor Communication

GPIOs used:

- `15` = `RE`
- `16` = `DE`
- `17` = `TXD`
- `18` = `RXD`

How it works:

- `Serial2.begin(...)` owns the UART data path on GPIO17/18
- `setTXmode()` drives `DE` and `RE` high before transmit
- `setRXmode()` drives them low after transmit

This means the code only manually toggles the RS-485 transceiver direction pins. The UART hardware handles the data bits.

### Display

GPIOs used:

- `10`, `11`, `12`, `13`, `14`, `21`

How it works:

- the display bus is driven by hardware SPI and the Adafruit ST7789 driver
- the backlight on GPIO21 is controlled with LEDC PWM
- brightness is dimmed after inactivity and fully forced off during sleep

### Sleep Control

Automatic idle timeout now keeps the controller fully awake and only turns off the LCD/backlight while the radio stays alive.

GPIOs used directly by deep-sleep logic:

- `4` as wake input
- `15`, `16`, `21` held in a safe state through RTC GPIO hold

When deep sleep is entered explicitly:

- `RS485RE` is driven high
- `RS485DE` is driven low
- `DISP_BL` is driven low
- those levels are held across deep sleep
- the start button wakes the system on low level

### Wi-Fi And BLE

These are not managed as raw GPIO in application logic.

- Wi-Fi is started through the ESP32 Wi-Fi stack
- BLE scale and BLE console are started through NimBLE
- no expansion header pins are used by either feature

## What Is Actually Free Today

These signals are declared but not referenced anywhere else in the firmware:

- `EXSDA` = `GPIO8`
- `EXSCL` = `GPIO9`
- `EXSPI_MOSI` = `GPIO35`
- `EXSPI_MISO` = `GPIO37`
- `EXSPI_CLK` = `GPIO36`
- `EXPSI_CS` = `GPIO39`

That means the current firmware does not initialize them, drive them, or read them.

Practically, they are available for new devices as long as your new code claims them.

## What Is Actually Available For New Devices

### Best Expansion Path: `EX_I2C`

This is the safest and most flexible place to add low-speed sensors or control peripherals.

Typical good candidates:

- temperature / humidity sensors
- barometric sensors
- ADC expanders
- GPIO expanders
- real-time clocks
- small OLEDs if you choose to add one in firmware

Advantages:

- only two signal pins
- easy to share multiple devices on one bus
- both exposed pins are `ADC1` pins if you later decide not to use I2C and want analog input instead

Main cautions:

- assume `3.3V` logic only
- the DXF silk indicates the header order is `3V3`, `GND`, `GPIO9/SCL`, `GPIO8/SDA`
- still verify physical orientation on the assembled board before plugging in a keyed cable
- add pull-ups only if your target device or bus needs them and the board does not already provide them

### Good Expansion Path: `EX_SPI`

This is the best place for higher-speed peripherals if you are willing to add firmware support.

Typical good candidates:

- external ADCs
- CAN/SPI coprocessors
- SPI flash-like peripherals
- e-paper displays
- some radio modules that use SPI

Advantages:

- separate MOSI, MISO, SCLK, and CS already named in firmware
- current module variant does not consume GPIO35-37 for PSRAM

Main cautions:

- `GPIO39` is also one of the external JTAG pins on ESP32-S3
- the DXF silk indicates the header order is `3V3`, `GND`, `GPIO35/MOSI`, `GPIO37/MISO`, `GPIO36/CLK`, `GPIO39/CS`
- SPI peripherals often need a confirmed `3.3V`, `GND`, reset, or interrupt line in addition to the four main SPI pins
- if you need more than one SPI device, you will need an extra GPIO for a second chip select or a small decoder scheme

### Possible But Unconfirmed: `UART` Header

The custom board BOM includes a `UART` 3-pin header, and the board DXF silk strongly suggests it is `GND-TXD0-RXD0`.

That means:

- the header is physically present
- it may be useful for a TTL serial sensor or adapter
- it is much better defined now than before, but you should still confirm physical orientation on the assembled board

If you want to use this header, confirm:

- logic voltage
- physical orientation on the assembled board

## How To Add New Devices

This section focuses on how to integrate new hardware into the existing firmware without breaking the current board behavior.

### Step 1: Choose The Right Expansion Path

Use this rule of thumb:

- choose `EX_I2C` for sensors and low-speed control peripherals
- choose `EX_SPI` for faster peripherals or devices that need strict timing
- choose `UART` only when the target device is serial-only and you understand the UART0 tradeoff

### Step 2: Avoid Pins Already Owned By The Firmware

Do not build new hardware features on:

- UI pins: `4`, `5`, `40`, `41`, `42`
- display pins: `10`, `11`, `12`, `13`, `14`, `21`
- motor comm pins: `15`, `16`, `17`, `18`

If you reuse those, you will break visible system behavior unless you redesign the firmware intentionally.

### Step 3: Add A Small Module Wrapper

The cleanest pattern in this project is to add a dedicated source pair for the new device, for example:

- `src/my_sensor.h`
- `src/my_sensor.cpp`

Then give it:

- `my_sensor_init()`
- `my_sensor_poll()`
- optional getters like `my_sensor_value()`

Call `*_init()` from `setup()` and `*_poll()` from `loop()` only if the device needs periodic service.

### Step 4: Keep Ownership Clear

If you add a new I2C or SPI device, the new module should own:

- bus initialization
- transfers
- timing and retries
- any data caching

The menu, display, and state-machine code should only consume the results.

### Step 5: Decide Whether The Feature Is Passive Or Interactive

Examples:

- passive device:
  - temperature sensor
  - current monitor
  - ADC input expander
- interactive device:
  - second display
  - external keypad
  - radio module

Passive devices can usually be polled from the main loop.

Interactive devices may need:

- a timer-driven poll
- event queueing
- or even a dedicated FreeRTOS task if the library blocks

### I2C Integration Pattern

For a new I2C device, use the recovered header mapping:

- `SCL = GPIO9`
- `SDA = GPIO8`

Minimal pattern:

```cpp
// my_i2c_device.cpp
#include <Wire.h>
#include "definitions.h"

void my_i2c_device_init() {
    Wire.begin(EXSDA, EXSCL);
    // device init sequence here
}

void my_i2c_device_poll() {
    // optional periodic reads
}
```

Best use cases:

- sensors
- ADCs
- GPIO expanders
- RTCs

Good engineering practice:

- initialize the bus once
- do not call `Wire.begin()` from multiple modules
- if several I2C devices are added later, centralize bus startup in one place

### SPI Integration Pattern

For a new SPI device, use the recovered header mapping:

- `MOSI = GPIO35`
- `MISO = GPIO37`
- `SCLK = GPIO36`
- `CS = GPIO39`

Minimal pattern:

```cpp
// my_spi_device.cpp
#include <SPI.h>
#include "definitions.h"

static SPIClass exSpi(HSPI);

void my_spi_device_init() {
    exSpi.begin(EXSPI_CLK, EXSPI_MISO, EXSPI_MOSI, EXPSI_CS);
    pinMode(EXPSI_CS, OUTPUT);
    digitalWrite(EXPSI_CS, HIGH);
}
```

Best use cases:

- fast ADCs
- e-paper
- SPI shift/GPIO expanders
- radio modules using SPI control

Good engineering practice:

- keep chip select inactive when idle
- if you add multiple SPI devices, keep one shared SPI bus and one CS pin per device
- choose another free GPIO for extra chip selects if needed

### UART Integration Pattern

The board silk suggests:

- `TXD0 = GPIO43`
- `RXD0 = GPIO44`

But because that is UART0, be careful:

- boot output and debug assumptions may interact with it
- external serial devices can inject noise into boot/debug paths

Use the UART header only if:

- the target device really needs UART
- you do not mind sharing or repurposing UART0 behavior

### Displaying New Data In The Existing UI

If a new device only provides extra measurements:

1. add a new module for the device
2. expose getters
3. read those values from `display.cpp`
4. optionally add menu items for configuration in `main.cpp`
5. store settings through `pdata.cpp`

That keeps the code consistent with the current project structure.

### Example Add-On Ideas That Fit Well

Good candidates for `EX_I2C`:

- `BME280`
- `SHT4x`
- `INA219` or `INA226`
- `ADS1115`
- `MCP23017`

Good candidates for `EX_SPI`:

- SPI ADC
- e-paper panel controller
- SPI GPIO expander
- some `3.3V` LoRa or other SPI radio modules

Less ideal first add-ons:

- devices needing strict `5V` logic
- devices that need many extra control pins beyond what the headers expose
- devices that must share the existing motor RS-485 bus

### Not Recommended For Expansion Without Rework

These are already committed to core board functions:

- `GPIO4`, `GPIO5`, `GPIO40`, `GPIO41`, `GPIO42` for the UI
- `GPIO10`, `GPIO11`, `GPIO12`, `GPIO13`, `GPIO14`, `GPIO21` for the display
- `GPIO15`, `GPIO16`, `GPIO17`, `GPIO18` for the RS-485 bus
- `GPIO19`, `GPIO20` are tied to USB by default

## Pin Capability Notes

### Digital GPIO

ESP32-S3 GPIO is highly multiplexed. In general, one pin can often be assigned to different digital peripherals in software. However, on this board the practical limit is not just the chip, it is what the PCB actually breaks out.

### Analog Input

ESP32-S3 has:

- `ADC1` on `GPIO1` through `GPIO10`
- `ADC2` on `GPIO11` through `GPIO20`

For this custom board, the most interesting exposed analog-capable pins are:

- `GPIO8` on `EX_I2C`
- `GPIO9` on `EX_I2C`

Pins that are analog-capable but already used:

- `GPIO4`, `GPIO5`
- `GPIO10` through `GPIO18`

Important limitation:

- Espressif documents that `ADC2` access can fail while Wi-Fi is active

That means `GPIO8` and `GPIO9` are the friendliest candidates if you want to add an analog sensor and still keep Wi-Fi active elsewhere in the firmware.

### PWM

ESP32-S3 can route PWM output to many GPIOs. On this board, the practical expansion candidates are the same as the exposed free-ish digital pins:

- `GPIO8`
- `GPIO9`
- `GPIO35`
- `GPIO36`
- `GPIO37`
- `GPIO39`

Do not forget that the header pin order is not confirmed from the repo alone.

### I2C

The firmware already declares:

- `SDA = GPIO8`
- `SCL = GPIO9`

This is the clearest expansion bus in the project today.

### SPI

The firmware already declares:

- `MOSI = GPIO35`
- `MISO = GPIO37`
- `SCLK = GPIO36`
- `CS = GPIO39`

Because the installed module is `ESP32-S3-WROOM-1-N8`, the Octal PSRAM restriction on `GPIO35-37` does not apply to this board.

### UART

The chip supports UART remapping, but the custom board header assignment is not confirmed in this repo. Treat the header as physically present but electrically unverified until checked on hardware.

## Special Cautions Before Wiring New Hardware

### 1. All MCU I/O should be treated as `3.3V` logic

Do not connect `5V` logic directly to the ESP32-S3 GPIO.

### 2. `RS485` is not the same thing as TTL serial

The `RS485` connector is behind a dedicated RS-485 transceiver. It is for the motor drive bus, not for a normal UART sensor or USB serial adapter.

### 3. Some ESP32-S3 pins have boot or debug implications

Official Espressif docs mark these as strapping pins:

- `GPIO0`
- `GPIO3`
- `GPIO45`
- `GPIO46`

This repo does not confirm that those pins are exposed on a user connector, but if you ever solder directly to the module or redesign the board, they need extra care.

Espressif also documents external JTAG on:

- `GPIO39` = TCK
- `GPIO40` = TDO
- `GPIO41` = TDI
- `GPIO42` = TMS

On this board:

- `GPIO40-42` are already used by the encoder
- `GPIO39` is assigned as `EX_SPI` chip-select

### 4. USB uses `GPIO19` and `GPIO20`

Espressif documents that these are used by USB/JTAG by default. Reusing them as ordinary GPIO can interfere with USB debug behavior.

### 5. Flashing warning from this project still applies

The project README says to flash over `USB-C` without the main `+24V` supply connected.

## Practical Recommendation By Device Type

### If you want to add an I2C sensor

Use `EX_I2C`.

Best fit examples:

- BME280 / BMP280
- SHT4x
- INA219 / INA226
- MCP23017
- ADS1115

### If you want to add an SPI device

Use `EX_SPI`.

Good fit examples:

- MCP3008-class SPI ADC
- e-paper controller boards
- SPI GPIO expanders
- some LoRa or radio modules that accept `3.3V` logic and separate power properly

### If you want to add a TTL UART module

Use the `UART` header only after verifying the actual pinout on the physical board.

### If you want to add another wired motor or industrial bus device

Do not hang it directly on the existing motor `RS485` bus unless you intentionally design for shared addressing and bus timing. That connector is part of the grinder drive control path, not a general fieldbus expansion port.

## What The Repo Does Not Yet Tell Us

These are the key unknowns that still need hardware verification:

- what the `M12 4 pin female connector` maps to

The local DXF appears to answer the `EX_I2C`, `EX_SPI`, and `UART` silk order question.

The repo does include local PCB artifacts that can help answer those questions:

- `PCB/REV0A/Gerber_SG98_PCB_SG98_2025-06-16.zip`
- `PCB/REV0A/OBJ_PCB_SG98_2025-06-18.zip`
- `PCB/REV0A/PCB_SG98_2025-06-18.dxf`

If you want to go from "GPIO names" to "which physical header pin is pin 1", those files are the next place to inspect.

## Online Pinout Pictures And Official References

These are useful reference pictures and docs for understanding the module and the ESP32-S3 signal families. They are not guaranteed to match the custom header order on this SG98 PCB.

### Board-Level Reference Picture

- Adafruit ESP32-S3 Feather pinout page:
  - <https://learn.adafruit.com/adafruit-esp32-s3-feather/pinouts>
- Adafruit labeled board photo:
  - <https://cdn-learn.adafruit.com/assets/assets/000/118/387/original/adafruit_products_FESPS3_pinouts.jpg?1675967136=>
- Adafruit logic-pin overlay:
  - <https://cdn-learn.adafruit.com/assets/assets/000/110/798/original/adafruit_products_FESP3_logic_pins.jpg?1649880830=>

These are useful because the PlatformIO project targets `adafruit_feather_esp32s3`, and the general ESP32-S3 peripheral behavior is similar. However, the SG98 board is a custom PCB, not an actual Feather breakout.

### Module-Level Reference Picture

- Espressif `ESP32-S3-WROOM-1` datasheet:
  - <https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf>

The most useful figure in that PDF for expansion work is:

- `Figure 3-1. Pin Layout (Top View)`

That figure shows where all module pins sit around the package, including:

- `IO8`, `IO9`
- `IO15-18`
- `IO35-39`
- `IO40-42`
- `TXD0`, `RXD0`

### GPIO Function And Restriction References

- ESP-IDF GPIO summary for ESP32-S3:
  - <https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html>
- ESP-IDF ADC reference for ESP32-S3:
  - <https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32s3/api-reference/peripherals/adc.html>
- ESP-IDF external JTAG pin reference:
  - <https://docs.espressif.com/projects/esp-idf/en/v4.4.1/esp32s3/api-guides/jtag-debugging/configure-other-jtag.html>

## Quick Decision Guide

If you want the safest path for a new device:

- choose `EX_I2C` first
- choose `EX_SPI` second
- use `UART` only after probing the board
- leave `RS485` alone unless the new hardware is intentionally part of the motor bus

If you want the least risky new add-on for a first experiment:

- add a simple `3.3V` I2C sensor on `EX_I2C`

If you want the most important next improvement to this documentation:

- identify the exact physical pin order of `EX_I2C`, `EX_SPI`, and `UART` from the PCB files or continuity measurements and add it here

## Source Summary

### From This Repo

- `src/definitions.h`
- `README.md`
- `PCB/REV0A/BOM_SG98_2025-06-16.csv`
- `PCB/REV0A/PickAndPlace_PCB_SG98_2025-06-16.csv`

### From Official Online Docs

- Espressif `ESP32-S3-WROOM-1` datasheet
- Espressif ESP-IDF GPIO, ADC, and JTAG docs
- Adafruit ESP32-S3 Feather pinout docs and pinout images
