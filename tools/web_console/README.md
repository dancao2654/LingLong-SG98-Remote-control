# SG98 Web Console

This is a local browser UI for the USB-C diagnostic console. It uses Web Serial, so it is meant for Chrome or Edge on desktop.

## Run

From the repo root:

```bash
python3 -m http.server 8080 -d tools/web_console
```

Then open:

```text
http://localhost:8080
```

## Notes

- Use the board's USB-C connection.
- This UI talks to the firmware over the `api ...` commands added to `src/diag_console.cpp`.
- The page mirrors the firmware menus instead of reading the round LCD.
- Bluefy is for the BLE path; this page is for USB-C Web Serial.
