# SEW MOVILINK Backend Log

## Target

- SEW MOVITRAC/MOVIDRIVE family drives that expose MOVILINK over RS-485.
- Initial request came from a SEW MC07B/MOVILINK use case.

## Communication

- Added a `COMM_PROFILE_SEW_MOVILINK` UART profile:
  - 9600 baud
  - 8 data bits
  - even parity
  - 1 stop bit
- Added MOVILINK frame support:
  - request start delimiter `0x02`
  - response start delimiter `0x1D`
  - PDU type `0x05` for 3 process-data words
  - PDU type `0x86` for acyclic 8-byte parameter channel without process data
  - XOR BCC over all frame bytes before the BCC byte

## Backend Behavior

- Added `SEW` as a selectable motor backend.
- Auto-probe tries zero-command 3PD frames at MOVILINK addresses `0`, `1`, and `2`.
- Runtime control uses cyclic 3PD:
  - PO1: control word 1
  - PO2: signed speed setpoint
  - PO3: current limit
- Runtime telemetry expects:
  - PI1: status word 1
  - PI2: actual speed
  - PI3: actual current

## Drive Setup Assumptions

The backend intentionally does not write persistent SEW drive parameters during probe.
Before using the backend for real motion, configure the drive-side process data mapping
with MOVITOOLS/MotionStudio or the keypad:

- control source and setpoint source set to RS-485/MOVILINK as appropriate
- PO data enabled
- PO1 mapped to control word 1
- PO2 mapped to speed
- PO3 mapped to current limit
- PI1 mapped to status word 1
- PI2 mapped to speed
- PI3 mapped to output or active current

Speed process data uses 0.2 rpm/count. Current process data uses 0.1% In/count.
