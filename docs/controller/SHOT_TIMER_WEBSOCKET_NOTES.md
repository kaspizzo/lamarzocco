# Shot Timer and Cloud WebSocket Notes

Last updated: 2026-04-16

## Current status

The controller-side shot timer has been removed again for now.

Reason:
- the old local shot timer path used by earlier `pylamarzocco` versions is no longer available on the current machine firmware
- the currently known La Marzocco cloud websocket topics do not expose a usable live shot timer for this machine

The controller firmware should therefore currently be treated as:
- no shot timer on-device
- no active dependency on the unfinished cloud live-update path

## What was tested

### 1. Old local API path

Older `pylamarzocco` releases used a local websocket on the machine itself:

- `ws://<machine>:8081/api/v1/streaming`

That path delivered events such as:
- `BrewingStartedGroup1StopType`
- `BrewingUpdateGroup1Time`
- `BrewingStopped...`
- `FlushStoppedGroup1Time`

Those events drove:
- `brew_active`
- `brew_active_duration`

This was confirmed from:
- `/private/tmp/pylm149/unpacked/pylamarzocco/devices/machine.py`
- `/private/tmp/pylm149/unpacked/pylamarzocco/clients/local.py`
- `/Users/kaspar/Documents/Projekte/pylamarzocco/docs/legacy/websockets.md`

### 2. Machine LAN reachability

The machine was identified on the local network as:
- `mi020904.fritz.box`
- `192.168.178.50`

Ports such as:
- `80`
- `443`
- `8080`
- `8081`
- `8082`
- `8083`
- `8888`

were tested and the old local API path did not respond.

Conclusion:
- the old local websocket route appears to be gone on the current firmware

### 3. Current cloud websocket path

The current cloud STOMP websocket connection works:

- endpoint: `wss://lion.lamarzocco.io/ws/connect`

Authenticated STOMP subscriptions were tested locally on the Mac against the real machine.

Confirmed working topics:
- `/ws/sn/<serial>/dashboard`
- `/ws/sn/<serial>/scheduling`

Additional wildcard-style subscription attempts were also tested:
- `/ws/sn/<serial>/*`
- `/ws/sn/<serial>/**`
- `/ws/sn/<serial>/*/*`
- `/ws/sn/<serial>/**/**`
- `/ws/sn/<serial>/#`
- `/ws/sn/<serial>`

These did not reveal any additional third topic. They only produced duplicates of `dashboard` and `scheduling`.

## What the cloud websocket actually returned

During real machine activity, including a shot, the websocket delivered live frames, but only snapshot-style payloads for:
- `dashboard`
- `scheduling`

The relevant `dashboard` fields remained unusable for shot timing:

- `brewingStartTime: null`
- `lastCoffee: null`
- `lastFlush: null`

Even during a real shot, no dedicated shot event or timer payload appeared on the known topics.

## Key conclusion

The shot timer is currently not available through any of the known and testable paths:

1. The old local websocket path is gone.
2. The known cloud websocket topics do not expose a usable shot timer for this machine.
3. Broad subscription attempts did not reveal an additional public topic carrying shot state.

At this point, the shot timer should be considered unresolved, not partially implemented.

## Why earlier shot timer behaviour existed before

The earlier shot timer behaviour came from the old local machine websocket path, not from the current cloud dashboard topic.

That explains why:
- older references still mention `brew_active_duration`
- older integrations could expose a live shot timer
- the current firmware and current cloud topic do not reproduce that behavior

## Useful files and local test artifacts

Local repo files:
- `/Users/kaspar/Documents/Projekte/pylamarzocco/pylamarzocco/clients/_cloud.py`
- `/Users/kaspar/Documents/Projekte/pylamarzocco/pylamarzocco/models/_schedule.py`
- `/Users/kaspar/Documents/Projekte/pylamarzocco/docs/legacy/websockets.md`

Temporary local test files created during debugging:
- `/tmp/lm_ws_raw.py`
- `/tmp/lm_ws_multi.py`
- `/tmp/lm_poll_dashboard.py`
- `/tmp/lm_poll_dashboard_slow.py`

Temporary package inspection directories:
- `/private/tmp/pylm149/unpacked`
- `/private/tmp/pylm224/unpacked_now`

## Recommended next step

Do not continue shot timer work on the ESP first.

Instead, reverse engineer the official mobile app traffic:

### Preferred order

1. Try HTTPS/WSS MITM on the iPhone app
   - `mitmproxy` or `Proxyman`
   - same Wi-Fi as the Mac
   - manual proxy on iPhone
   - trusted root certificate on iPhone
2. If the app uses certificate pinning:
   - fall back to app analysis / decompilation
   - or platform-specific pinning bypass work
3. Only when a real shot-related destination or event is found:
   - wire the controller shot timer back in

## Suggested rule for future implementation

Only re-enable the controller shot timer when at least one of these is true:
- a stable live shot event source is identified
- a stable cloud field with real-time shot start/stop becomes available
- a new documented websocket destination is confirmed

Until then, the cleaner behavior is:
- no on-device shot timer
- no fake timer
- no partially working timer based on guesses
