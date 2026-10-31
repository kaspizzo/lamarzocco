# Acaia Lunar 2021 / USB-C bridge beta

Status: implemented from public BLE protocol implementations and covered by
synthetic host tests; **no Lunar 2021 hardware test yet**. This profile targets
the Lunar 2021 / USB-C with standard Acaia firmware. It does not require or
install the La Marzocco Connected Scale firmware upgrade on the physical scale.
The machine still needs native Brew-by-Weight capability and compatible firmware.
Umbra models, other Acaia products and scales already converted to La Marzocco
firmware are outside this profile's claimed scope.

## Setup

1. Build and flash the controller using the normal
   [firmware update procedure](FLASHING_CONTROLLER.md). Preserve settings; a
   flash erase is unnecessary. Existing Mini-USB selections remain unchanged.
2. Put the Lunar in manual weight/timer **Mode 2**, displaying **grams**. Close
   other apps or devices connected directly to it. Keep the machine paddle off
   during connection setup.
3. Open the local portal's **Scale Adapter** section and select
   **Acaia Lunar 2021 / USB-C (experimental)** (`acaia_2021`). Enable the adapter,
   leave its source address empty, save and reload after a few scan windows.
4. Select the physical scale's exact BLE address and matching public/random
   address type. If it is absent from the candidate list, enter its address/type
   manually. Confirm manual mode and save. Keep the virtual name `LUNAR-C` and
   the controller's position stable for machine registration/calibration tests.
5. Check fresh weight/battery in the portal. The scale icon is dim while the
   source is connected, then fully lit when fresh data and the machine scale
   link are both present. Enable **Connection Debug Log** before testing.

If tare/timer commands fail, record the standard Acaia firmware version. The
[pyacaia project](https://github.com/lucapinello/pyacaia) reports that early
Lunar 2021 firmware needed an Acaia firmware update for these commands. Do not
confuse a normal Acaia update with conversion to La Marzocco firmware.

## Transport and commands

| Channel | UUID |
| --- | --- |
| Service | `49535343-fe7d-4ae5-8fa9-9fafd205e455` |
| Commands | `49535343-8841-43f4-a8d4-ecbe34729bb3` |
| Notifications | `49535343-1e4d-4bd9-ba61-23c647249616` |

Initialization subscribes to the notification CCCD, sends the 15-byte ASCII
identity `012345678901234`, then requests the weight/battery/timer/button/settings
stream. The adapter sends a heartbeat every two seconds once ready. Command
writes prefer Write Without Response when offered; otherwise they use Write
Request. The identity differs from the older Mini-USB driver's 15 hyphens.

Both profiles share `EF DD` framing, the two interleaved payload checksums,
settings validation, compound-event decoding and tare/timer command encoding.
The decoder handles multiple frames per notification and frames fragmented
across notifications, including weight-plus-timer and heartbeat-plus-weight.
Seven-/nine-byte settings require an explicit gram unit and a valid battery.
New or unrecognized firmware layouts are not assumed compatible.

The existing Acaia control-confirmation path is retained: a matching protocol
ACK is required, and tare also requires a new weight sample. ATT write success,
button events and unknown ACK codes do not complete commands. The 450 ms control
timeout and one-second measurement freshness limit are unchanged. Actual ACK
behavior and timing on the Lunar 2021 remain part of hardware validation.

Protocol references inspected on 2026-10-31:

- [LunarGateway transport example, revision 58db782](https://github.com/frowin/LunarGateway/blob/58db7820848b5e4daef624ef58e7ddd7731e1c19/examples/basic/basic.ino):
  service and characteristic UUIDs, heartbeat timing.
- [acaia-lunar-ble, revision eea7e7c](https://github.com/beat843796/acaia-lunar-ble/blob/eea7e7c0b68bc4fc8070817f0c09809246e6d74b/acaia_lunar_ble/__init__.py):
  identity/stream/heartbeat/tare/timer bytes and compound weight/timer layout.
  Its author reports testing model AL008; this is not a test of our adapter.

These are community interoperability implementations, not an official Acaia
wire-protocol specification. The controller implementation reuses its existing
Acaia parser; no external BLE library or runtime dependency is added.

## Hardware acceptance checklist

- Record scale model, standard firmware version, mode, controller build and
  machine firmware. Test an empty platform, a known object, a negative reading
  and both 0.1 g / 0.01 g display resolutions against the portal.
- Confirm the machine connects to `LUNAR-C` and reports the real battery.
  Verify source-only and full-link icon states separately.
- Request tare before flow, timer reset/start/stop and a repeated start. Compare
  the physical display with logged command results. Unknown/missing ACKs must
  fail rather than be reported as completed.
- Power the scale off/on, then restart the controller and machine separately.
  Check automatic source reconnect, fresh data and the machine link. Verify
  location calibration persistence separately from simple reconnection.
- Finally perform a supervised water-only weight-stop test and record target
  versus final weight. Automatic stop precision is not validated by host tests.
- Download the persistent connection log after any failure. It includes full
  rejected frames and pending-command/ACK context. After a protocol/control
  fault, correct the setup and save adapter settings again to retry.
