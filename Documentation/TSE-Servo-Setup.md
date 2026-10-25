# TSE Family Servo Support

Target nameplate: TSE-B-D-100-KFM, 1 kW, single-phase AC 220 V.
Implementation reference: [TSD/TSE family manual, 2024-12-16](https://makerselectronics.com/wp-content/uploads/2025/06/TSD-TSE%E4%BA%A4%E6%B5%81%E4%BC%BA%E6%9C%8D%E8%AF%B4%E6%98%8E%E4%B9%A6-20241216-1.pdf).
This is a machine-translated reseller-hosted family manual, not confirmation
of the KFM variant. Hardware validation is still required. Do not treat a
successful build or simulated drive test as motor commissioning.

## Connection And Selection

The manual's CN3/CN4 table (printed page 81) specifies pin 1 RS485+,
pin 2 RS485-, pin 4 signal GND; other pins are unused. These are not Ethernet
ports. Confirm connector orientation and labeling on the actual drive before wiring.
Do not connect or rewire an energized drive; follow the drive's discharge warnings.

Configure PA71=1 (slave ID), PA72=96 (9600 baud), PA73=0 (8N2).
AUTO uses read-only checks of PA1 (model 40..80), PA2 (nonzero software
version), PA62 (encoder 4 or 5), and PA71/72/73. This is a family fingerprint,
not a unique manufacturer ID. Other baud/parity configurations are not searched.
The existing external-RS485-first probe order is unchanged.

Select AUTO or TSE in the controller/remote drive menu. USB/API also accepts
`api action drive tse`; raw Modbus diagnostics accept profile `tse`.
Forced selection still verifies the fingerprint before writing anything.

## Required Drive Commissioning

Keep the mechanical load disconnected for initial validation, establish a
working independent emergency stop, and follow the manufacturer's setup procedure.
Software stop and a serial link are not safety-rated emergency stops.

The backend uses PA53 software enable, not a physical SON input. Before use:

1. Set the persistent PA24 internal speed 1 to 0 and PA53 forced enable to 0.
   These power-on defaults avoid retaining a nonzero command or forced enable
   after a drive-only power cycle. Firmware changes these only in volatile RAM.
2. Reserve enable and speed selection for software: unassign SON, SP1, SP2,
   command inversion, zero-command, and mixed-mode functions from DI1..DI4.
   With factory inputs, change P3-0 from 1 (SON) to 0 (unassigned).
3. Leave P3-15..19 at 0 (no forced inputs), P3-30=0 (physical input mode),
   and P3-31=0. The backend checks these before enabling.
4. Keep needed physical direction/torque-limit inputs and their wiring.
   Accepted DI functions are 0, 2, 3, 4, 5, 6. Firmware does not change PA20,
   input polarity, terminal assignments, encoder/motor selection, or tuning.
5. Set PA40/41 acceleration/deceleration appropriately on the drive.
   Shared ramp levels and automatic torque calibration are not implemented.

An incompatible input setup remains NOT READY with controller error 4 and is
rechecked without modifying input configuration. A corrected setup can recover
without rebooting. Test positive and negative rotation at a low speed before
connecting the grinder load. The 3000 rpm firmware ceiling is a profile limit,
not proof of the attached motor's rated speed; reduce the application RPM limit
when the actual motor has a lower rating.

## Register Mapping

PA parameters use their decimal parameter number as the holding-register
address. Section 8.4.6 adds 0x0080 for volatile, write-only PA aliases.
Use the original address for reads. Runtime writes use function 06 only.

| Function | Parameter / Address | Value |
| --- | --- | --- |
| Control mode | PA4 / volatile 0x0084 | 1, speed mode |
| Speed source | PA22 / volatile 0x0096 | 1, internal speed |
| Maximum speed | PA23 / volatile 0x0097 | 3000 rpm |
| Speed command | PA24 / volatile 0x0098 | Signed rpm, clamped to +/-3000 |
| CCW torque limit | PA34 / volatile 0x00A2 | 0..300 percent |
| CW torque limit | PA35 / volatile 0x00A3 | -300..0 percent |
| Software enable | PA53 / volatile 0x00B5 | 0 stop, 1 enable |
| Communication error action | PA74 / volatile 0x00CA | 1, alarm and stop |
| Live speed | 0x1000 | Signed rpm |
| Live torque | 0x1007 | Signed percent, reported as magnitude for shared load/purge logic |
| Live mode | 0x1009 | Must be 1 |
| Alarm | 0x1013 | 0 means no alarm |

PA74 is described as communication-error handling, not a documented watchdog
timeout. Do not assume it detects an unplugged cable or controller power loss.
An independent hardware stop is required. The translated monitoring table has
wrapped row descriptions; the mapping above follows the ordered monitor list
in sections 6.4/8.4.5 and must be checked against the actual drive on the bench.

Initialization verifies the fingerprint, commands zero speed and removes forced
enable, checks input setup, then configures volatile speed mode. Valid speed/mode
and alarm telemetry are required before READY. Both signed torque limits and
the speed command must be acknowledged before enabling. Stop sends zero speed
before removing enable, attempting both even if an acknowledgement is lost.
Faults cancel the run and are not automatically cleared on the drive.
Periodic PA22..24 and PA53 readbacks also detect lost volatile settings after
a drive-only reboot, even when the persistent control mode is already speed mode.

An unacknowledged transaction times out after 300 ms, cancels the requested
run, attempts both stop writes, and restarts read-only verification after a
one-second retry delay. A recovered connection never restores the old run
command; another Start is required. No EEPROM write, motor reset, or live
motor movement is performed by building or running the host tests.

## Verification

Run the simulated backend regression suite:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=undefined \
  -Itools/tests/tse/stubs -Isrc tools/tests/tse/test_tse_backend.cpp \
  src/TSE/motorcontrol_tse.cpp -o /tmp/sg98-test-tse
/tmp/sg98-test-tse
```

AddressSanitizer can be added where supported. The installed macOS ASan runtime
deadlocked during initialization before entering this test; UBSan is used here.

Bench acceptance remains: confirm fingerprint and monitor scaling, power-on
defaults, input interlocks, low-speed direction, loaded torque limiting,
Start/Stop, drive-only reboot, cable loss, controller power loss, and manual
restart after recovery. Do not publish this as hardware-validated firmware
until those checks pass.
