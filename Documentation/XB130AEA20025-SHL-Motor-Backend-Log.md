# XB130AEA20025-SHL Motor Backend Log

## Summary

- Target: `XB130AEA20025-SHL`, 2.0 kW, 2500 rpm, 7.7 N.m servo motor.
- Nameplate: IP65, 3-phase AC 220 V, 7.5 A, printed code `47`.
- Driver manual: `Documentation/dkNEW_C.pdf`, Technical Manual For AASD Series Servo Drive.
- Manual KRS table maps the matching 2 kW / 2500 rpm / 7.7 N.m profile to `130st_m07725` with `Pn001 = 20`.
- Firmware target: controller workspace, not `sg98-remote`.

## Implementation Notes

- Added a dedicated selectable drive entry for `XB130AEA20025-SHL`.
- Auto-detect claims this profile when KRS `Pn001` reads motor code `20`; the printed nameplate `CODE: 47` is not used as the Modbus motor-code probe value.
- Reused the validated KRS 2 kW speed-mode command path: `Pn002`, `Pn168`, `Pn169`, `Pn068`, `Pn070`, and `Pn008/Pn009`.
- Rated max RPM is capped at 2500 rpm in the controller descriptor.
- Ramp remains unsupported until KRS acceleration/deceleration register behavior is validated on hardware.
