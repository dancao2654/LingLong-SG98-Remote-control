# SG98 Remote Back-Cover Case

This folder contains the current 3D-print concept for the Waveshare ESP32-S3
Touch AMOLED 1.75 remote, LiPo battery, pressure-device connector, and internal
magnet strip.

The current preferred concept is a back-cover base: the case sits below the
remote, the remote locates into a deep open-sided round upper socket, and the
enclosed cavity covers the wiring behind the remote. The case body is narrower
than the remote so the two control sides stay exposed.

## Source Dimensions

- Touch panel OD: 48.96 mm
- Visible area: 44.16 mm
- Round PCB reference: 46.0 mm
- Vendor total module height: 10.4 mm
- Battery: 25.5 x 37.0 x 4.3 mm
- Magnet strip: 10.0 x 60.0 x 1.5 mm
- Back-cover body: 44.0 x 70.0 mm, intentionally narrower than the 48.96 mm
  remote OD
- Lower shell height: 16.5 mm, increased from the M5 prototype to clear a
  GX12-3 pressure connector

## Print Files

Use these for the new back-cover design:

- `print_files/sg98_remote_backcover_shell.stl`: main lower shell with a deep
  open-sided round upper socket, rounded lower corners, side slots for the
  snap-in bottom lid, and a recessed GX12-3 pressure connector bay.
- `print_files/sg98_remote_backcover_bottom_lid.stl`: bottom lid with internal
  recessed magnet pocket and four snap tabs.
- `print_files/sg98_pressure_socket_snap_cover.stl`: optional snap-in cover for
  the recessed pressure connector bay when the sensor cable is not attached.

## Preview

- `sg98_remote_case_preview.svg`: generated technical preview.
- `sg98_remote_case_preview.png`: PNG thumbnail of the preview.
- `sg98_pressure_connector_recess_mockup.svg`: connector and snap-cover mockup.
- `sg98_pressure_connector_recess_mockup.png`: PNG thumbnail of the connector
  mockup.
- `preview_files/sg98_remote_backcover_fit_check_assembly_NOT_FOR_PRINT.stl`:
  preview-only STL with a dummy remote body seated in the shell.

Regenerate the preview with:

```sh
python3 tools/generate_remote_case_preview.py
```

Regenerate the STL files without OpenSCAD:

```sh
python3 tools/generate_remote_case_stl.py
```

The printable STL files contain only the plastic case parts. The PNG preview and
the `NOT_FOR_PRINT` assembly STL include a dummy remote shape to show how the
real remote sits in the socket.

## Assembly Concept

1. Put the battery and pressure-sensor wiring inside the lower shell.
2. Put the 10 x 60 x 1.5 mm magnet strip into the recessed pocket on the inside
   face of the bottom lid.
3. Press the bottom lid into the shell until the four side tabs click into the
   matching receiver slots.
4. Press the remote into the deep upper socket. The socket is round where it
   contacts the remote, but it opens through the two narrower sides so USB-C and
   button access stay clear.

The magnet is inside the case, not exposed underneath. The outside face of the
bottom lid remains smooth.

## Pressure Connector

Use a detachable 3-pin connector instead of hardwiring the pressure sensor cable
directly into the remote. The remote/case side should be the recessed female
side, and the pressure-sensor cable should carry the matching plug.

The pressure connector bay is on the short end of the back-cover shell. It is a
recessed 28.0 x 15.5 mm rounded pocket with a 12.6 mm printable clearance bore
for a GX12-3 panel-mount aviation connector. The connector mounting plane is
about 4.8 mm behind the outer lip, and the clamp wall is 1.2 mm thick, so the
connector sits protected inside the case edge instead of sticking out from a
flat wall.

Wire the GX12-3 consistently:

- `GX12 pin 1 = SUP / +5V`
- `GX12 pin 2 = OUT / pressure signal`
- `GX12 pin 3 = GND`

Use the socket-contact half on the remote/case side if the connector pair gives
you a choice, so the powered remote side does not expose live pins. The current
CAD target is a cheap GX12-3 pair with a 12 mm mounting thread/body and an
approximately 15 mm mating shell. Measure the exact connector body, nut, and
solder-cup clearance before a final print and adjust the bay if your chosen
Amazon listing differs.

When the sensor cable is not attached, press the printed snap cover into the
recessed bay as a dust cover. Print the cover in PETG or TPU so the small side
bumps can flex without cracking.

## Battery And Charging

The LiPo stays on the MX1.25 battery connector. Do not wire the battery to the
case pressure connector or to the H2 header.

USB-C charges through the board PMIC. If you add external 5V charging contacts,
wire them only to:

- `H2 pin 8 = VBUS / +5V`
- `H2 pin 7 = GND`

## Print Notes

- Treat this as a first fit-check prototype.
- Start with PLA/PETG at 0.2 mm layers.
- Use 3 walls and 20-30% infill.
- Print the bottom lid with the smooth outside face on the bed and the magnet
  pocket/snaps upward.
- Print the main shell with supports enabled for the internal overhangs.
- PETG is preferred over brittle PLA for the snap tabs.
- If the snap tabs are too tight on the first print, sand the four tabs slightly
  instead of forcing the lid.
- Put thin foam or Kapton between the battery pouch and PCB.
- Do not clamp the pouch tightly; keep at least 0.5-0.7 mm height clearance.
