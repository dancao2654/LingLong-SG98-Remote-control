# Remote USB UI Testing

The serial console runs at 115200 baud. `api set <menu> <item> <value>`
tests menu values but does not browse the display or activate action-only items.

USB-only simulated input commands:

```text
api ui secondary_short
api ui dial 1
api ui dial -1
api ui primary_short
api ui primary_long
```

These queue one event through the normal GUI input loop, including activity
tracking, menu navigation, editing, and confirmation. Dial steps must be nonzero
and between -20 and 20. A pending event, flash screen, or controller OTA operation
rejects new input. Read `api status` after each event before sending another.
Like physical input, the first event can dismiss the dock clock.

On the round remote, a secondary short press opens System from Home. Scroll to
About and press primary. Once the remote update is available, press primary once
to arm, then again within the confirmation window to download over Wi-Fi and
reboot. `remoteOtaArmed`, `screen`, `selection`, and `inputs` in `api status`
allow the test to verify the actual GUI path. `api ota check latest` refreshes
the availability check without downloading.

Do not press primary on Home during an OTA test: it is the normal grind action.
Do not activate Controller OTA when testing the remote's own update.

Home and dock-clock screens show controller online/offline independently from
Wi-Fi association/IP information. Controller freshness expires after five
seconds, and changes request a redraw without user input.
