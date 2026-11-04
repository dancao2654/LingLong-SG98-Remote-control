# KFM checkpoint: supervised tests on 2026-11-03

Firmware: **2026.11.03.1**, paired `controller-kfm-standalone-candidate` and `remote-waveshare-175-standalone-candidate`. Hardware: Waveshare ESP32-S3 RS485/CAN controller, Waveshare Touch AMOLED 1.75, TSE-B-D-100-KFM servo, Bookoo scale. This is partial hardware acceptance, not a production release or a months-long reliability claim.

## Update and verification

Application-only updates at 0x10000, verified by esptool hashes and boot identity. Existing partition tables and settings were retained; 36 common visible menu values were unchanged immediately after flashing. Backups, images and raw USB logs were saved locally under `outputs/`, which is excluded from Git. Both devices booted with the expected environment and connected by ESP-NOW. No host Start or arm command was issued during the motion tests; both devices remained USB powered.

## Passed on this firmware

- Short physical touchscreen Start/Stop runs at 100 (twice), 500, 1000, 1500 and 2000 rpm. Measured peaks matched the setpoints; each ended at zero, disarmed, with complete stop readback and no new running error. These were empty-grinder tests, not loaded/endurance tests.
- Physical touchscreen power loss during a 100-rpm run: `remote_link_lost`, measured zero, stop mask 63 and acknowledgement required. Restoring display power caused no restart and did not clear the latch. Switching GBW then manual acknowledged the verified stop without motion. A subsequent explicit Start/Stop passed.
- Servo mains off/on while controller/display remained powered: loss detected, driver reidentified, no spontaneous start, complete zero-speed readback. Historical timeout/rejected-register fields from mains-off are not fresh running failures.
- Button purge at 100 rpm in both directions: positive/negative measured speeds, intervening zero, automatic completion. Separate test: first physical Stop initiated purge; a second distinct Stop interrupted it and stopped the motor.
- Bookoo readings 53.8 -> 0.0 -> 53.8 g, plus recorded physical power-off disconnect and automatic reconnection to stable 53.8 g. Scanning is active in GBW mode, not manual mode. Button pressure created transient weights; this stationary test is not proof of running scale-loss shutdown.
- GBW threshold test with empty grinder, 19.4-g target and 200/100 rpm: first attempt stopped with error 105/no output before the object arrived, so was not a target-stop pass. Retry: 16.1 g led to command 100 rpm, 46.2 g led to command zero, then measured zero and IDLE_GBW/error 0. No Stop command was sent. This validates coarse threshold behavior, not dose accuracy.

These results belong to this version. The historical real 18.0-g coffee dose and earlier injected scale-loss test belong to their earlier firmware versions.

## Final recorded settings and power state

Main Power was confirmed OFF at completion and the object removed. USB diagnostic ports were closed. Controller: IDLE_GBW, zero command/measurement, disconnected servo (drive_conn expected while powered off). Bookoo was connected at zero in the last snapshot.

GBW restored and verified at **800/300 rpm**, target **19.4 g**, learned modifier 2100, offset 150 ms. Manual setpoint 100 rpm. Button-Purge OFF. Low purge test values remain **100/100 rpm**, delay 2000 ms, duration 2000 ms, zero pause 1000 ms; these differ from the pre-test 2000/0-rpm and 3000-ms-duration settings. No servo EEPROM or tuning changes were requested in this round; firmware performs its existing guarded startup/stop writes.

## Remaining acceptance work

1. Independent shutdown for failed MCU or broken RS485 is still unverified. Software cannot send Stop through a broken link. Do not run a live unplug test before establishing an independent stop strategy; the operator's mains switch does not prove a drive watchdog.
2. Independent controller power without laptop, further controller reboot/power sequences and prevention of unintended restart.
3. Actual 120-second runtime/auto-off behavior, sustained/in-run speed changes and desired higher-speed purge on this build.
4. Running scale-link-loss stop on this version. A stationary reconnect or no-output stop does not prove this; diagnostic injection is disabled in standalone.
5. Repeated real coffee doses and accuracy on this version. One or two days of use supplement, rather than replace, targeted acceptance tests.
6. Torque-based auto-purge/calibration remains unavailable pending verified scaling and calibration.

USB sampling and drive polling do not establish precise stop latency or independently capture every RS485 frame. Additional user-reported tests without logs have not been assumed to pass.

## Reproducible offline checks

`python3 tools/run_kfm_checks.py` covers native backend, transport, API/GBW/purge flows, gesture idempotency, limits and operator tools, including sanitizer checks. Both paired PlatformIO environments were built successfully for the tested images. Offline model passes do not replace the remaining physical tests.
