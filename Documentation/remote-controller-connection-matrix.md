# SG98 Remote / Controller Connection Matrix

Primary rule: keep ESP-NOW available in every normal connectivity state, then park both devices in ESP-NOW-only whenever the ESP-NOW link is healthy and Wi-Fi/web access is quiet. ESP-NOW-only is the automatic destination state, not a normal user option. The user-facing control is Wi-Fi enabled/disabled, plus Radio off for full-size standalone controllers.

## Modes

| Side | Mode | Normal use | Recovery role |
| --- | --- | --- | --- |
| Controller | ESP-NOW | Default low-resource parked state. Wi-Fi/AP is off. | If headless and no remote ESP-NOW traffic is heard for the recovery timeout, open WEB+ESP. |
| Controller | WEB+ESP | Temporary setup/recovery or active Wi-Fi/web UI. ESP-NOW remains alive. | Stays open while AP clients/web requests are active or ESP-NOW is not healthy. Closes back to ESP-NOW when quiet. |
| Controller | WEB only | Full-size bench/debug only. | Not allowed on headless because it strands the remote. |
| Controller | OFF | Full-size only. | Not allowed on headless; local button/screen/encoder must exist for recovery. |
| Remote | ESP-NOW | Default operation. Remote Wi-Fi is off. | Scans/locks controller peer. |
| Remote | WebSocket | Recovery path only when saved Wi-Fi exists or the user intentionally joins the controller AP. | Requests controller WEB+ESP, then returns to ESP-NOW. |

## User-Facing Controls

| UI wording | Internal state | Meaning |
| --- | --- | --- |
| Wi-Fi disabled | ESP-NOW | Lowest resource state. Controller and remote use ESP-NOW; Wi-Fi/AP is off. |
| Wi-Fi enabled | WEB+ESP | Opens controller AP/home web UI while keeping ESP-NOW alive. |
| Radio off | OFF | Full-size controller only, because local controls can recover it. |
| Web only | WEB | Hidden/diagnostic only; not a normal setup mode because it strands ESP-NOW remotes. |

## Operation And Recovery Matrix

| Case | Controller start | Remote start | Expected operation | Recovery trigger | Exit / final state |
| --- | --- | --- | --- | --- | --- |
| Fresh normal boot | ESP-NOW | ESP-NOW, no saved Wi-Fi required | Remote scans, discovers controller, locks peer, and polls over ESP-NOW. | None. | Controller ESP-NOW, remote ESP-NOW, both Wi-Fi stacks off. |
| Full-size controller only | ESP-NOW | No remote present | Controller remains usable from local screen/buttons/encoder with Wi-Fi off. | User wants Wi-Fi/web UI. | User switches locally to WEB+ESP; after web quiet and ESP-NOW not needed, user can return to ESP-NOW. |
| Full-size controller only with Wi-Fi/web UI | WEB+ESP selected locally | No remote required | Controller AP/home web UI is available for browser control. | User leaves web UI or wants low resource. | User switches locally to ESP-NOW. Auto-park requires healthy ESP-NOW traffic, so controller-only web use is not silently closed. |
| Headless controller only | ESP-NOW | No remote present | Controller waits briefly for remote ESP-NOW traffic. | No ESP-NOW command/status/heartbeat before timeout. | Controller opens WEB+ESP recovery AP so a phone can configure/control it. |
| Controller boots headless with no remote nearby | ESP-NOW | Offline/out of range | Controller waits for ESP-NOW traffic. | No ESP-NOW command/status/heartbeat before timeout. | Controller opens WEB+ESP recovery AP. When remote returns and web is quiet, close to ESP-NOW. |
| Remote goes out after link was healthy | ESP-NOW | Was ESP-NOW, then silent | Controller tracks last ESP-NOW command/heartbeat. | Headless controller hears no ESP-NOW traffic for timeout. | Controller opens WEB+ESP. Returning remote relocks over ESP-NOW, then quiet web bridge closes. |
| Wi-Fi/web UI wanted | WEB+ESP | ESP-NOW | Browser uses AP/home web UI; remote still controls over ESP-NOW. | AP client connected or web API requests active. | Keep WEB+ESP while active; after quiet hold and healthy ESP-NOW, close to ESP-NOW. |
| Wi-Fi setup / captive portal | WEB+ESP | ESP-NOW or WebSocket recovery | Controller serves setup page at AP; ESP-NOW remains alive. | No saved STA, failed STA, or user joins AP. | After setup and quiet web activity, ESP-NOW remains primary and Wi-Fi can close. |
| Remote has saved Wi-Fi but no ESP-NOW peer | Any reachable web mode | Remote tries ESP-NOW first, then WebSocket if saved Wi-Fi exists | Remote finds controller through mDNS/AP host, requests WEB+ESP if needed. | ESP-NOW discovery fails but WebSocket candidate exists. | Remote switches back to ESP-NOW once peer is discovered. |
| Controller accidentally in WEB only | WEB only | Remote ESP-NOW cannot connect | Full-size only debug state; web UI works but ESP-NOW is off. | Remote cannot recover unless it also has a WebSocket path. | User/local/API should move controller to WEB+ESP or ESP-NOW. Headless rejects WEB only. |
| Headless OFF request | OFF requested | Any | Request is rejected/sanitized. | OFF would strand controller. | Controller stays WEB+ESP or ESP-NOW. |
| Full-size OFF request | OFF | Any | Allowed because local UI exists. | User presses local controls. | Local UI can restore ESP-NOW or WEB+ESP. |
| Grinding/running link loss | ESP-NOW | Remote silent during running | Safety heartbeat/status traffic counts as remote activity. | Headless remote deadman stops motor states first; network recovery timer then opens WEB+ESP if silence continues. | Safe idle plus recovery AP; final ESP-NOW after relock. |

## Test Expectations

| Test | Must prove |
| --- | --- |
| Generic remote + full-size controller | Fresh no-info pair reaches ESP-NOW; WEB+ESP keeps web GUI alive while active; WEB only is visibly non-dual; OFF remains local-recoverable. |
| Full-size controller-only | With no remote ESP-NOW traffic, controller remains locally usable and does not require recovery AP; WEB+ESP can be selected locally for phone control. |
| Generic remote + headless-sim controller | Fresh no-info pair reaches ESP-NOW; WEB only and OFF are rejected; no-remote boot opens WEB+ESP; lost remote opens WEB+ESP; returned remote closes back to ESP-NOW after quiet. |
| Headless controller-only | With no remote ESP-NOW traffic, controller opens WEB+ESP recovery so a phone can reach it. |
| Load probe | Serial/API and link polling stay below latency guard in ESP-NOW and WEB+ESP recovery paths. |
