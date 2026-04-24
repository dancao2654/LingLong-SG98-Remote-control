# 130ST-M07725 Motor Backend Log

## Summary

- Target: `130ST-M07725`, 2.0 kW, 2500 rpm, 7.7 N.m servo motor.
- Firmware target: controller workspace, not `sg98-remote`.
- Implementation basis: `Documentation/New-Motor-Backend-Guide.md` and `Documentation/New-Motor-Backend-Implementation-Template.md`.

## Evidence Used

- KRS technical manual maps `130st_m07725` to `Pn001 = 20`.
- KRS Modbus parameter range maps `Pn000..Pn239` to `0x0000..0x00EF`.
- KRS monitor range maps `Dn000..Dn021` to `0x0170..0x0185`.
- KRS default serial profile is `9600`, `8,O,1`, Modbus RTU.

## Implementation Notes

- Added a KRS-specific backend that only auto-detects when `Pn001 == 20`; this avoids relabeling all RT/JMC drives as 130ST.
- Commands use speed mode with signed `Pn169` internal speed 1, communication-controlled `SON/SP` bits, and `Pn008/Pn009` torque limits.
- Ramp is intentionally unsupported until KRS acceleration/deceleration register behavior is validated on hardware.
- Added manual KRS selection in the controller UI, Web/API drive action, and diagnostic console (`k`).

## Verification

- `platformio run -e controller-universal-s3`: success.
