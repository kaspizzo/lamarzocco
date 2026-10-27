# Controller BLE scale adapter

The ESP32 controller connects to a physical BLE scale and presents a virtual
`LUNAR-C` scale to the machine. After selecting a scale once, the controller
handles its connection and reconnects when the scale wakes up. A phone or Mac
does not need to run the adapter.

The legacy Mini-USB Acaia Lunar has passed a connection and measurement smoke
test. BOOKOO Mini/Ultra support is experimental and has only synthetic host
tests; follow the separate [BOOKOO beta guide](BOOKOO_BETA_TEST.md). Newer Acaia
transports and other brands need their own drivers.

## Building and updating

Use the [flashing guide](FLASHING_CONTROLLER.md) for the supported round
ESP32-S3 board. This adapter build was verified with ESP-IDF **5.5.4**.
Fresh configurations inherit these settings from `sdkconfig.defaults`:

```ini
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y
```

If the checkout already has a generated `firmware/esp32/sdkconfig`, use
`idf.py menuconfig` from that directory to enable peripheral and broadcaster
roles, allow three connections and select external RAM for NimBLE allocation.
Existing generated settings take precedence over defaults. Keep the three
**LVGL registry compatibility** options disabled for this project's LVGL 8.

Build from the repository root after loading your ESP-IDF environment:

```sh
./firmware/esp32/dev.sh build
./firmware/esp32/dev.sh test
```

For an existing installation with the current partition layout:

```sh
cd firmware/esp32
./dev.sh quick
```

`quick` updates the app and opens the serial monitor. Exit with `Ctrl+]`.
Use `./dev.sh full` for the first installation or after partition-layout changes.
Normal firmware updates retain scale settings and presets. Network reset also
retains scale settings; factory reset erases them. Do not erase flash for a
normal adapter update.

## Select the Acaia once

1. Complete the controller's [network and machine setup](SETUP_GUIDE.md).
   Open its local portal at `http://<hostname>.local/` or its IP address and
   sign in if portal access is protected.
2. Turn on the Acaia in **Mode 2**, with the display using **grams**. Close
   Acaia/La Marzocco apps or other devices holding a direct BLE connection to
   the scale. Leave the machine switched on and its paddle off during setup.
3. Open **Scale Adapter**. Select **Acaia Lunar (legacy)**, check
   **Enable Scale Adapter**, leave **Source BLE Address** empty and save.
   The controller scans in short windows; reload the page after a few seconds.
4. Find the physical scale under **Observed Scales** and select its exact
   address in **Source BLE Address**. The selector copies its public/random
   address type; verify it against the observed entry. If necessary, enter
   the address and matching type manually. An observed name alone does not
   select or authorize a connection.
5. Keep **Virtual Scale Name** at `LUNAR-C`, confirm manual weight/timer mode
   with grams and press **Save Scale Adapter** again.
6. Reload the portal. Check **Source: fresh**, compare its weight with the
   scale display and wait for **Machine scale link: connected**. Initial
   registration can take longer than the source connection.

The address, driver, mode confirmation and virtual name are stored in encrypted
controller NVS. Later boots reconnect to that exact address. To change scales
or rescan, clear the source address, save, then repeat selection. Saving restarts
the adapter session, so perform configuration changes with the paddle off.

## Understand the two connections

The physical scale connects to the controller; the machine independently
connects to the controller's virtual scale. Both connections must work.

| Indicator | Meaning |
| --- | --- |
| Balance-scale header icon | Fresh physical-scale measurements reach the controller. It disappears when the scale sleeps, disconnects or faults, or when the adapter is disabled. |
| `Source: connected, awaiting data` | BLE connected, but setup or the fresh measurement stream is incomplete. |
| `Source: fresh` | Validated measurements are at most one second old. |
| Machine link `advertising` | The virtual scale is available and waiting for the machine. |
| Machine link `connected` | The machine connected and subscribed to scale notifications. |
| Source / Machine sessions | Connection counts since controller boot; useful when testing recovery. |
| Registration requests | Position-registration requests sent since boot, including delivery failures; not successful calibrations. |

While the scale adapter is active, periodic machine status uses the cloud.
The controller releases its owner BLE link so the machine can connect back to
its virtual scale. Controls fall back to the cloud while that scale link is
established. A missing Bluetooth header icon can therefore be normal; inspect
the two scale statuses separately.

## Reconnection and controller placement

Wake the selected scale and leave the controller powered. A source disconnect
invalidates its old weight, stops virtual-scale advertising and closes the
machine scale session. A new source session must supply a validated handshake
and fresh weight before the virtual scale becomes available again.

There is no configured distance threshold for the source connection. BLE range
depends on placement and obstructions. For diagnosis, put the controller near
both devices, then test at its intended permanent position. The machine's
location training measures the **controller's** radio signal, so leave its
position and virtual name stable during registration and normal use.

If the machine link is absent while source data is fresh, the controller checks
the machine's registrations. It sends `SetScalePosition` with `force:false`
only when its registration is missing and no other scale is registered. Checks
retry with 30/60/120/240-second backoff. An existing registration is retained;
another scale, standby or an unreadable response prevents an automatic write.

A runtime entry or `calibrated:true` does not establish persisted location
calibration. Persistence after machine restart still needs a hardware test.

## Presets with dose targets

When the selected machine reports Brew by Weight support, recipe presets save
and restore the BBW mode (**Dose 1**, **Dose 2** or **Continuous**) and both dose
weights, alongside boiler temperature and prebrewing times.

Swipe down to open presets, select a slot with the ring, then use **Save** or
**Load**. On the German UI these buttons read **Sichern** and **Laden**. The
buttons follow the actual summary height with a constant gap, so the extra BBW
lines remain above them. Long preset names are shortened to one line.

![Preset with BBW mode and both dose targets](images/preset-bbw-en.png)

## Troubleshooting

| Status or symptom | Action |
| --- | --- |
| `Scale adapter disabled` | Enable and save the adapter in the portal. |
| No observed scales | Wake the scale, close connected apps, clear the source address, enable and save, then reload. Check driver/model and proximity. Discovery lists known name prefixes; manual address entry remains available. |
| Repeated setup timeout | Verify the selected address and public/random type, the legacy Acaia transport, Mode 2 and grams. |
| Fresh source, machine still waiting | Turn the machine on, check its selected binding and registration status, and allow the retry backoff. Another scale's registration must be resolved before automatic registration can proceed. |
| Invalid source protocol or non-gram-unit fault | Verify the physical display uses grams and the correct driver is selected, then save the settings again. If it recurs, capture the `lm_scale` rejection log over USB. |
| Control confirmation timeout | Check physical mode and whether tare/timer actually responded. Correct the setup, then save again to clear the fault. |
| Header icon present but machine link absent | The icon confirms only physical-scale reception. Check the machine link and registration status in the portal. |

Protocol/control faults stop the adapter until settings are saved again. The
hardware debug session also observed an intermittent Acaia protocol
fault while the display showed grams; the rejected packet was not captured.
The firmware now logs the driver, length and first 40 source-protocol bytes of
a rejected notification to help diagnose a recurrence. This is an open issue,
not a confirmed unit-setting error.

For a report, include controller revision, scale model/firmware/mode, machine
model/firmware, both link statuses, session counts and relevant `lm_scale` USB
lines. Keep account credentials, machine tokens, raw account exports and flash
backups private.

## Validation and remaining hardware checks

The 2026-10-26 controller smoke test matched a displayed **2.9 g** to received
values around **2.89–2.91 g**. The machine independently reported `LUNAR-C`
connected, the source battery and BBW availability. One scale off/on cycle
restored both links without another position request. The revised preset layout
was flashed and visually accepted on 2026-10-27.

To repeat the connection test, leave the paddle off, check zero and an object
on the scale, power the scale off for about five seconds, then on. Confirm the
icon disappears and returns, both links recover, and the received weight matches.
Repeat separately after a controller restart and a machine restart; record the
registration counters and position-calibration behavior.

Physical tare/timer translation, repeated recovery, machine-restart calibration
persistence and automatic weight-stop precision still need controller hardware
validation. A water-only brew test should be supervised and stopped manually
if the machine does not stop at its target. The adapter sends scale operations;
it does not issue pump or valve commands.

## Extending the driver layer

Implement the `scale_driver.h` contract for a new BLE protocol and register it
in `scale_drivers.c`, the portal selector and the build. Drivers emit normalized
gram weights, battery/readiness and control completions. ATT write acceptance
alone must not confirm tare/timer execution. Keep protocol callbacks on the
shared NimBLE queue and add host fixtures for malformed data, fragmentation,
commands and completion timing. A driver also needs its own hardware validation;
selecting an arbitrary BLE device does not make its protocol compatible.
