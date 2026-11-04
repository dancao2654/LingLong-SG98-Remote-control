# TSE KFM commissioning status

Base source: `438efcc3276cef820a9145303f94a1ee4d7b3d47`. Current installed version: **2026.10.27.8**, dedicated one-shot touchscreen test, flashed and checked with servo main power OFF. Latest servo-connected experiment: **2026.10.27.7**, one supervised100rpm pulse with confirmed automatic stop. The PA25 marker/reset experiment has now produced a zero-return observation in the operator-present .6 follow-up below; one separately supervised fixed-speed pulse has now passed; normal operating-mode runs remain unperformed.

## .8 physical touchscreen controls and timed stop passed

One human touchscreen Start/Stop pair reached100rpm and ended with touchscreen_stop_verified/pulse cancelled before any USB cleanup. User confirmation, stop echoes/readback and separate final direct reads all passed. A separate human Start without Stop ended with pulse complete/pulse_completed_motion_observed at the nominal5000ms setting, peak100rpm and verified zero end state. Both tests ended unarmed, IDLE/connected/error0; no auto-rearm was observed. Actual mechanical timing and packet-level radio START/STOP traces were not measured.

The powered-off touchscreen precheck at idle confirmed remoteFreshfalse with the drive still connected/stationary and no arm. Radio-loss stop under motion and link restoration/no restart remain next tests; other previously listed release limits are unchanged. Evidence: `outputs/KFM-touchscreen-controls-2026-10-27.zip`.

## .8 one-shot touchscreen Start/Stop (installed, still unarmed)

The dedicated `controller-kfm-pa25-touch-test` retains the private fixed100rpm PA25 pulse engine, with a5000ms nonzero interval, but disables standalone USB pulse start. A named USB test-arm operation permits one normal touchscreen Start within30000ms, only with fresh ESP-NOW traffic and clean startup/stationary KFM data. GUI Start/Stop use the existing remote command path; Stop bypasses purge and directly requests the known restore. The test path bypasses ordinary purge/GBW timing and ignores the stored1100rpm setpoint. Other speeds/EEPROM/presets/raw writes remain blocked.

A consumed/expired/lost-remote/faulted arm never automatically returns. Missing accepted ESP-NOW commands for over3000ms requests a stop while the controller remains alive; main switch remains necessary if communication/controller failure prevents that stop. Standard profiles are unchanged by the compile-time test branch. The .8 flash/boot/refusal checks passed with main power OFF and fresh real ESP-NOW traffic. No test-arm or Start was sent; zero successful servo writes. Evidence: `outputs/kfm-touch-test-device-20261027/`. One real UI Start/Stop and nominal timed-stop test have now passed; radio-loss and other fault cases are not yet tested. See [TSE-KFM-Touch-Test-DE.md](TSE-KFM-Touch-Test-DE.md). No daily/unattended release is implied.

## .7 first physical motor pulse passed

The operator explicitly confirmed readiness at the empty closed grinder and independent main switch. One fixed temporary PA25−100 pulse was requested after clean startup/preflight and verified SON enable at zero. The measured peak magnitude was100rpm, motionObservedtrue, alarm0; phase complete with pulse_completed_motion_observed. Stop acknowledgements AND zero-state readback passed. The operator confirmed brief physical rotation and automatic stop. Separate final direct reads confirmed PA24/PA25zero, SONoff, selection2 and RPM0; normal status IDLE/connected/error0. USB was closed. Evidence: `outputs/KFM-first-motor-pulse-2026-10-27.zip`.

This is the first tested low-speed motion/stop through PA25. Exact mechanical pulse timing and independent mechanical direction were not measured; the firmware signed-speed check passed. Normal UI starts remain blocked. Variable/higher speeds, loaded stopping, radio/controller loss, short mains dips and comprehensive restart protection still require separate work.

## .7 supervised pulse (flashed, no pulse started)

A separate `controller-kfm-pa25-pulse` build implements only an explicit operator-present fixed100rpm / nominal2000ms pulse through temporary PA25. Normal UI/USB arm remains compiled out. The prior marker/SON probe variants cannot be combined with this build. A clean power-on snapshot with PA25zero is required; runtime preflight rechecks factory mode/input/preset state, then confirms SON1 at zero before the one nonzero speed write. The wire direction follows the earlier original-controller negative normal-grinding capture. Physical direction and actual motor behaviour are not yet validated.

The private transport window allows only SON1 and temporary PA25zero/fixed−100; public writes, EEPROM and alternate selection remain blocked. Stop tries SON0/PA25zero/PA24zero/selection2 and separately checks acknowledgements and real readback. Healthy bounded coasting is observed read-only for up to3000ms; missing zero-state confirmation does not become success. Invalid reads, alarm, overspeed, wrong direction, changed inputs and cancellation abort without automatic re-run. A zero-only recovery path handles a known retained −100 command after session loss, after fresh identity/configuration checks.

The pulse timer includes serial work through a fresh clock callback; the software is not a safety-rated hard stop. A CPU, communication or power failure can prevent a stop, so the operator at the independent main switch remains essential. The .7 app-only flash and USB refusal checks have passed with main power OFF; no pulse was requested and no successful servo write occurred. Cold connected startup and the first actual motor pulse remain untested. Evidence: `outputs/kfm-pa25-pulse-device-20261027/`. See [TSE-KFM-PA25-Pulstest-DE.md](TSE-KFM-PA25-Pulstest-DE.md).

## .6 physical PA25 audit result

The operator-present follow-up is now recorded in `outputs/KFM-PA25-shared-test-2026-10-27.zip`. Round1 expired while the driver was off; it is invalid as a PA25 reset test. Explicit recovery after switch-on succeeded with write acknowledgements, zero-state readback and separate direct-register confirmation. An actual controller-reset recovery still has only offline coverage.

Round2 confirmed temporary PA25=1 with SONoff, selection2, RPM0 and alarm0. The operator reported off / wait60seconds / on as one sequence. The first frozen full post-gap sample BEFORE restoration showed PA25=0, PA24=-1000, SON=0, selection2, RPM0 and alarm0. Four full return samples over at least two seconds consistently showed zero. Restoration acknowledgements AND readback passed; direct final reads confirmed PA24/PA25zero, SONoff, selection2 and RPM0; normal status was IDLE/connected/error0. Human switch confirmations were journaled after the sequence; approximately56seconds of logged missing-link phase is not an exact switch-off duration or a full reset proof.

This supports PA25 as a candidate runtime channel for the next limited-build investigation. It does not validate short power dips, communication/controller loss, active SON/running values, motor run or loaded stopping. Normal starts remain locked. The old SON-retention observation remains a material restart concern.

## .6 recovery follow-up (isolated ESP32 flashed and checked)

Offline review reproduced an orphan-marker state after controller RAM was reset while the servo still held PA25=1. That state correctly blocked normal setup but could not be cleared by cancelling a newly empty local session. An explicit `api tse pa25_probe restore` path now rechecks the exact KFM fingerprint/communication settings and known factory configuration before sending only the four stop/zero values. It never enables, selects another preset, writes EEPROM or sets a nonzero marker. A mismatched identity/configuration or an active probe is refused. It does not rely on normal setup being complete.

All probe restorations now require a real complete zero-state readback in addition to write echoes. `restoreAcknowledged` and `restoreReadbackValid` are separate. An echoed-but-ignored clear cannot satisfy the latter; it blocks further starts and preserves unresolved state across backend reinitialisation. A fresh recovery before the first startup snapshot correctly marks `beforeAnyBackendWrite=false`. The operator console also distinguishes a no-op cancellation from verified restoration. The .6 console intentionally refuses older .5 images; the controller has now been updated to the matching .6 image.

The offline preparation was subsequently followed by an app-only .6 flash and isolated boot/refusal checks. The explicit restore path refused an absent drive, successfulServoWrites remained0, and the PA25 test was not started. Evidence: `outputs/kfm-pa25-device-v6-20261027/`. Successful physical recovery and the actual PA25 experiment remain untested. The preparation archive retains its historical not-flashed metadata.

## .5 follow-up (isolated ESP32 flashed and checked)

The new `controller-kfm-pa25-probe` variant is separate from the SON-enable zero-preset probe. It audits temporary PA25=1 while requiring SON=0, PA53=0, zero physical/virtual input status, unchanged factory input assignments, selection2 and stationary alarm-free telemetry. It never sends SON=1, writes persistent parameters or selects another preset. The former selected PA25=0 default is preserved in EEPROM. Public PA25 writes are blocked; only a synchronous private callback may write temporary PA25 values 0/1. First post-gap samples are frozen before restoration and compared over at least three samples and two seconds. All four restore writes are attempted even after an acknowledgement failure. Start remains locked after unconfirmed restoration; cancellation can retry it. There is a 180-second software deadline and no normal motion support. This test must remain operator-present and does not certify a safety-rated restart interlock. The .5 app-only flash and isolated boot/USB refusal checks have now passed on controller B8:1F:3F:C8:F5:30. The former SON probe is unavailable and PA25 probe remains idle; no real PA25 marker test has occurred. Evidence: `outputs/kfm-pa25-device-20261027/`.

The specific German procedure is [TSE-KFM-PA25-Pruefung-DE.md](TSE-KFM-PA25-Pruefung-DE.md). Use `tools/kfm_pa25_probe_operator.py` with the matching .5 build; the old zero-probe console remains specific to its earlier experiment.

## Operator-present .4 test on 2026-10-27

The zero-preset experiment was completed with the operator switching main power off/on. SON=1 at the zero preset was confirmed before interruption. The first full returned sample, before restoration writes, showed SON=1, SP1=2, PA24=-1000, PA25=0, RPM=0 and alarm=0. Four complete return samples consistently showed SON=1 over at least two seconds. Restoration was acknowledged; a separate valid FC03 read subsequently confirmed SON=0 and SP1=2. Final status was IDLE, connected, alarm-free and stationary. The operator then switched main power off again.

The observed missing-link interval was about 14 seconds. This confirms the tested interruption, not complete internal discharge or a full reset of every control supply. Do not infer EEPROM persistence of SON from this result alone. Runtime PA24=0 again returned to -1000. Normal motion remains compiled out. No running-preset or loaded-stop test has been performed.

Raw evidence and an English report: `outputs/kfm-session-20261027/`; shareable archive: `outputs/KFM-shared-test-2026-10-27.zip`.

Next motor-side work must establish a dependable restart interlock or a documented and experimentally verified persistent zero startup configuration. A USB/backend stop after reconnection alone cannot prevent an earlier restart while communication is absent. The dedicated zero-preset test does not establish what happens with the running preset selected. Do not relax the motion arm guard solely because SP1 remained at its zero-preset value during this test.

## Hardware evidence through .3

The measured KFM fingerprint is PA1=6, PA2=0x1F5C, PA62=4 and PA71/72/73=1/96/0. The factory mapping is P3-0..3=1/2/3/4; P3-15..19=0/2/0/0/0; P3-30/31=0/0. PA4=1, PA22=1, PA23=5000, PA25=0, PA53=0. The backend reaches complete, reports valid stationary telemetry and no drive alarm while retaining these assignments.

Pre-write cold-start snapshots showed PA24=-1000, not zero. The explicit base-register PA24=0 command was acknowledged once; the subsequent confirmed driver off/on and controller reboot still produced -1000 before any backend initialisation writes. Nonvolatile zero storage was therefore **not established**. Do not repeat undocumented writes or factory-reset the driver to work around it.

The observed initial SON-off/SP1-stop states do not establish their behaviour after an active command. The original full parameter export is a selected-register backup, not a complete EEPROM image. No motor run or loaded stopping test has been performed.

## .4 software changes

- A raw diagnostic read may surrender an outstanding telemetry read only while the backend is stationary, unarmed and has no control acknowledgement pending. It does not consume a reply behind the backend's state machine. Locally refused commands expose `blocked=true` and do not claim a wire timeout or change UART settings. A missing CRC check is reported as `crc=not_checked`, distinct from a failed CRC.
- Direct raw TSE writes are refused while the backend owns control; explicit guarded actions are used instead. Send restrictions are checked before shared request state or receive buffers are modified.
- Modbus read responses require the exact requested byte count and frame length. This prevents partial values, oversized byte counts and zero-filled data from satisfying preflight checks. FC06 acknowledgements must match slave, function, address, value, length and CRC. Exception replies require the expected slave/function/CRC and retain the 16-bit error marker; a former bool-return truncation is fixed. Exceptions are counted separately from timeouts.
- Diagnostic UART restoration preserves the drive's existing alarm code.
- KFM primary-direction polarity follows the user's original capture S007/S008: normal grinding uses negative PA24; reverse uses positive. An optional motion build is simulation-only and is not included in the commissioning images. Its limit remains 500 rpm and its arm guard continues to reject nonzero power-on defaults.
- KFM torque remains available as a signed raw word with validity and acquisition timestamp. Its percent scaling is unverified; it is not fed as a calibrated torque magnitude into shared learning. Factory torque settings are not rewritten by KFM commissioning and torque control is not advertised as applied.

The common Modbus decoder changes affect standard Modbus traffic for other profiles too. The existing TSE-family regression suite and valid generic protocol cases pass; physical regression on other drive models has not been performed.

## Build variants

| Environment | Transport behaviour |
| --- | --- |
| `controller-kfm-commissioning` | Fixed TSE, onboard RS485, no SON=1, no nonzero speed writes. Reads plus known stop values and the explicit PA24=0 candidate write only. |
| `controller-kfm-zero-probe` | Same restrictions, with a private one-write SON=1 window opened solely by the explicit zero-preset experiment after preflight. SP1=0 and all nonzero speed writes remain blocked. |

`SG98_TSE_KFM_ZERO_PROBE=0` actually disables the feature. A zero-probe build without the commissioning transport gate, or combined with motion-enabled code, is a compile-time error. Normal boot does not start the zero probe. General motor starts remain locked in both images.

## Zero-preset experiment

`api tse zero_probe start` is accepted only on the identified, stationary, idle KFM with complete valid telemetry and no active probe. The firmware reads source/presets, mode, PA53, physical-function mapping, forced/virtual inputs, speed, alarm and raw power/status monitors. It requires the measured factory mapping, PA25=0, PA53=0, SON=0, SP1=2 and zero speed. It then confirms temporary speed0 and stop selection2 before attempting SON=1. The next complete readback must confirm SON=1, the zero preset and zero actual speed.

During the operator's off/on cycle the probe owns the bus. Unrelated raw diagnostics and parameter actions are blocked. Communication loss is observed with reads only until return, cancellation or the software deadline. The first complete post-gap sample is frozen before restoration writes. At least three complete samples spanning at least two seconds then check for a changing boot state. First and settled SON values, consistency, validity and raw PA24/PA25 are reported separately. A communications gap is not automatically a verified mains cycle; the operator console journals the human switch confirmations.

Completion, explicit cancellation, failure or the 120-second software deadline attempts SON0, SP1=2 and temporary speed0. All stop writes are attempted even if an earlier acknowledgement fails. An unconfirmed restoration can be retried after the link returns. A USB/device/software failure can prevent a software stop; the operator must retain the independent main switch. Stop echoes do not certify physical or safety-rated disable. The test may energize the motor at the selected zero preset and must not run unattended.

## Diagnostics and tools

`api tse` emits `tse_zero_probe`, `tse_diagnostics`, the immutable first-connection `tse_startup` snapshot and the ordinary `tse` state. Snapshot validity is explicit; zero-valued absent data must not be treated as a measured zero. Raw torque is invalid while the special probe owns the bus because that probe does not sample torque.

`tools/kfm_capture.py` only sends `api device`, `api tse`, `api status`. It records raw USB bytes, text, timestamped events and a summary. Port selection is explicit and identity is checked. It is a console capture, not a passive trace of every physical RS485 frame.

`tools/kfm_zero_probe_operator.py` is separate and requires `--operator-present`, explicit port and expected controller MAC. It only allows the same read commands plus zero-probe start/cancel. Start requires the exact dedicated build/version, fresh stationary data and matching KFM diagnostics. Identity changes revoke the previous verified session. `off`/`on` only journal human actions. Program exit attempts a newly acknowledged cancellation and warns to use main power if cancellation cannot be confirmed.

The German session guide is [TSE-KFM-Next-Session-DE.md](TSE-KFM-Next-Session-DE.md). Run all host tests with `python3 tools/run_kfm_checks.py`. They open no hardware and include real Modbus/probe/backend/MOVILINK sources, malformed-frame cases and ASan/UBSan. They do not validate the physical KFM or its nonvolatile storage.

## Persistence wording clarification

Restrictions on persistent writes mean explicit persistent PA-group writes; they do not prove that known SON/SP1 register writes have no nonvolatile effects. Earlier broad no-EEPROM wording must not be used as such proof. SON retention was observed after a short interruption without verified full internal reset; its mechanism remains unestablished.
