# Shot Timer and Cloud WebSocket Notes

Last updated: 2026-04-20

## Current status

The cloud websocket path is now implemented and working on the controller.

Current controller behaviour:

- authenticated STOMP websocket to `wss://lion.lamarzocco.io/ws/connect`
- subscription to `/ws/sn/<serial>/dashboard`
- automatic reconnect through the firmware worker when the websocket drops or Wi-Fi returns
- on-device shot-timer screen driven from cloud dashboard updates
- sticky final shot time after shot end, dismissed by any touch swipe

The shot timer itself is no longer "removed". It is implemented in firmware, but it still depends on whether the selected machine actually exposes usable shot fields through the cloud dashboard.

## Current shot-timer rules in firmware

The controller starts a shot only when at least one of these is true in `CMMachineStatus`:

- `status == "Brewing"`
- `brewingStartTime > 0`

It intentionally does not start a shot just because:

- `mode == "BrewingMode"`

That guard exists because the cloud dashboard can remain in `BrewingMode` even after a shot has ended.

Once a live shot is active:

- the timer counts up on a dedicated full-screen overlay
- when the shot ends, the last visible time is frozen
- the frozen time stays on-screen until the user dismisses it with any swipe gesture
- if a new shot starts before dismissal, the sticky value is replaced immediately by the new live timer

The current implementation does not yet use `lastCoffee.extractionSeconds` as a post-shot correction source.

## What is confirmed

### 1. Old local machine websocket path

Older `pylamarzocco` versions used a local machine websocket such as:

- `ws://<machine>:8081/api/v1/streaming`

That path was the source of the historic shot-timer behaviour in older integrations.

On the currently tested machine/firmware, that old local path could not be recovered and should still be treated as gone.

### 2. Cloud websocket transport

The controller-side cloud websocket is now confirmed working end-to-end:

- signed websocket upgrade headers
- HTTP upgrade
- STOMP `CONNECT`
- STOMP `CONNECTED`
- `SUBSCRIBE`
- live `MESSAGE` frames from the dashboard destination

This is no longer an unfinished transport path.

### 3. Current on-device implementation

The controller firmware now contains:

- working websocket/STOMP live updates
- shot state parsing from dashboard payloads
- a dedicated shot-timer runtime state machine (`hidden`, `live`, `sticky`)
- a dedicated shot-timer UI screen

## Important limitation

The tested machine used during controller development still did not expose usable live shot fields on the dashboard topic during a real shot.

Observed during live websocket traffic on that machine:

- `status: "PoweredOn"`
- `mode: "BrewingMode"`
- `brewingStartTime: null`
- `lastCoffee: null`
- `lastFlush: null`

In that situation the controller intentionally does not start the timer, because the payload is not trustworthy enough to infer a real shot.

## Micra / Linea Mini status

Current project status for Linea Micra and Linea Mini:

- the shot-timer implementation is in place
- the websocket transport is in place
- the parser is aligned with the expected cloud fields
- real-machine confirmation is still missing

Practical meaning:

- if a Micra or Linea Mini sends `status == "Brewing"` or a real `brewingStartTime`, the current firmware should show the shot timer as intended
- this behaviour is implemented, but not yet confirmed on a real Micra/Linea Mini shot sequence

So the correct statement is:

- Micra/Linea Mini shot-timer support is implemented in firmware
- it is not yet hardware-verified end-to-end

## Recommended next step

Validate the current implementation on a real Micra or Linea Mini that is known to produce dashboard updates during a shot.

For that validation, capture raw `dashboard` payloads across:

1. idle
2. shot start
3. mid-shot
4. shot end
5. a few seconds after shot end

Pay special attention to:

- `status`
- `mode`
- `brewingStartTime`
- `lastCoffee`
- `lastFlush`

If those machines still do not provide usable shot fields, the next investigation target is still the official app traffic. But that is now a data-source problem, not a missing controller websocket implementation.
