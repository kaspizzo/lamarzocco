# La Marzocco Knob Controller Firmware (ESP32)

This folder contains the standalone firmware for the round ESP32-S3 controller used in this project.

The current target is a JC3636K718-style round controller board with:

- ESP32-S3
- `360x360` round display
- CST816S touch
- physical outer ring
- DRV2605L haptics

## Current feature set

- `controller_state.[ch]`
  portable controller state machine and recipe presets
- `controller_ui.[ch]`
  round LVGL UI with dedicated screens for:
  - coffee boiler
  - prebrewing on-time
  - prebrewing off-time
  - brew by weight mode and dose targets when the selected machine reports BBW support
  - steam boiler
  - standby
  - shot timer
  - presets
  - setup
- `controller_shot_timer.[ch]`
  small runtime-owned shot-timer state machine with `hidden`, `live`, and `sticky` modes
- `input.[ch]`
  vendor-style polling for the physical outer ring
- `board_display.[ch]`
  ST77916 QSPI display bring-up plus CST816S touch registration
- `board_backlight.[ch]`
  PWM backlight control
- `board_haptic.[ch]`
  DRV2605L click feedback
- `wifi_setup.[ch]`
  setup/network façade and portal orchestration with:
  - Wi-Fi scan
  - home Wi-Fi storage
  - on-device language selection (`English` default, optional `Deutsch`)
  - optional SVG upload for a custom controller header logo
  - cloud credential storage
  - machine loading and selection
  - setup AP QR generation and captive-portal helpers
- `controller_settings.[ch]`
  persisted settings for:
  - Wi-Fi credentials
  - hostname and language
  - custom logo blob
  - cloud credentials
  - selected machine binding
- `cloud_session.[ch]`
  public cloud REST/session API for:
  - installation bootstrap
  - access-token caching
  - fleet refresh
  - dashboard fetch
  - machine command execution
- `cloud_auth.c`
  internal signed-auth helper implementation for:
  - installation bootstrap
  - request signing
  - cached access-token lookup
  - websocket upgrade headers
- `cloud_machine_api.c`
  internal machine/fleet REST implementation for:
  - fleet refresh
  - machine selection restoration
  - cloud command execution
- `cloud_dashboard.c`
  internal dashboard REST implementation for:
  - dashboard fetch
  - prebrewing extraction
  - dashboard summaries/logging
- `cloud_live_updates.[ch]`
  cloud websocket/live-update handling for:
  - reachability probes
  - websocket/STOMP lifecycle
  - async dashboard updates
  - async command status updates
  - shot timer snapshot state
- `machine_link.[ch]`
  public machine-link API and reducer surface
- `machine_link.c`
  machine-link core state, worker orchestration, and public API glue
- `machine_link_ble.c`
  BLE transport, GATT helpers, authentication, and direct machine commands
- `machine_link_cloud.c`
  cloud fallback fetches, async command acknowledgements, and dashboard updates
- `machine_link_types.h`
  type-only machine DTOs and bitmasks shared with UI and setup page rendering
- `wifi_setup_types.h`
  type-only Wi-Fi/setup DTOs shared with UI/runtime and portal page rendering
- `dev.sh`
  fast build/flash/monitor loop

## On-device gestures

- Swipe left/right: next or previous main settings page
- Swipe down: open presets
- Swipe up: open Wi-Fi/cloud setup
- Shot-timer screen while a shot is running: swipes stay blocked
- Sticky shot-timer screen after a shot: any swipe dismisses the frozen final time
- Presets screen: swipe up to return
- Setup screen: swipe down to return
- Setup screen: long-press to open the on-device network reset flow
- Reset flow: rotate clockwise to arm, rotate to choose `Yes`/`No`, tap the button to confirm, swipe down to cancel

The current CST816S + LVGL path gives reliable single-finger direction gestures, so presets/setup use vertical swipes instead of multi-touch gestures.

## Preset behaviour

Presets are intentionally recipe-only.

They currently store and load:

- coffee boiler temperature
- prebrewing on-time
- prebrewing off-time
- brew-by-weight mode, dose 1, and dose 2 when the selected machine reports BBW support

They do not store or restore:

- steam boiler level (`Off / 1 / 2 / 3`)
- standby

## Setup portal

If no home Wi-Fi is stored, the controller starts its own setup AP automatically:

- SSID: `LM-CTRL-XXXX`
- Password: shown on the controller setup screen
- URL: `http://192.168.4.1`

On first boot without stored Wi-Fi credentials, and again after a full factory reset, the controller opens the setup screen automatically so the AP QR code is visible immediately.

The same setup portal can be reopened any time from the on-device setup screen.

If home Wi-Fi credentials are already stored, the controller tries to join that
network immediately on boot. If the STA link drops later, the firmware retries
with exponential backoff starting at 1 second and capped at 30 seconds. After
repeated failures, the setup AP is enabled again as a local recovery path while
the controller keeps retrying the saved home network in the background.

The browser portal currently supports:

- storing the La Marzocco cloud email/password locally on the controller
- scanning nearby Wi-Fi networks so the SSID does not need to be typed manually
- saving home Wi-Fi credentials and reconnecting immediately
- keeping the default text header or storing a user-provided custom logo as a controller setting
- loading machines from the cloud account
- selecting the active machine and storing the serial/BLE binding needed by the controller
- editing brew-by-weight mode and dose targets when the selected machine reports BBW support through the cloud dashboard

The cloud login path expects a direct La Marzocco account email/password. Accounts created only through Apple or Google sign-in are not expected to work with the current controller onboarding flow. A possible workaround is to create a second La Marzocco account with a normal email/password login and share machine access to that account in the official app.

The firmware does not ship an official vendor logo asset. By default, the display header uses text (`la marzocco`). If you want a logo on the device, upload a local SVG in the setup portal; the browser rasterizes it to the controller header format before the generated bitmap is stored on-device.

## Runtime sync behaviour

- BLE is preferred whenever the controller has an authenticated local machine link.
- Cloud websocket live updates run when Wi-Fi, cloud credentials, and a machine selection are available.
- Cloud is used for prebrewing writes, websocket live updates, and as a dashboard-value fallback.
- Brew by weight is currently cloud-only in this firmware: BBW values are read from the cloud dashboard and BBW writes go through cloud machine commands, not the local BLE transport.
- The controller refreshes machine-facing values periodically while it stays online.
- If a value has not been loaded yet, the UI shows placeholders instead of stale defaults.
- The shot timer is driven only from cloud dashboard signals `status == Brewing` or `brewingStartTime > 0`.
- `mode == BrewingMode` alone intentionally does not start the shot timer.

## Brew By Weight status

Current BBW implementation status:

- The firmware parses BBW support, mode, and dose targets from the cloud dashboard widget `CMBrewByWeightDoses`.
- The round controller UI exposes BBW mode, Dose 1, and Dose 2 pages only when the selected machine reports BBW support.
- Controller presets store and restore BBW mode and doses when BBW is available for the selected machine.
- The setup portal also exposes a dedicated BBW form when the selected machine reports BBW support.
- BBW writes currently go through cloud machine commands, not the local BLE path.
- In practice that means BBW is implemented, but it still depends on cloud reachability and on the selected machine exposing BBW through the dashboard.

## Internal architecture

The firmware is now split by responsibility instead of putting setup, persistence, cloud session logic, live updates, UI rendering, and runtime coordination into a few large files.

- `app_main.c`
  bootstrap and top-level wiring only
- `controller_runtime.[ch]`
  event loop coordination, sync cadence, and UI view-model assembly
- `controller_shot_timer.[ch]`
  shot-timer runtime state machine used by `controller_runtime`
- `controller_ui.[ch]`
  LVGL rendering only, driven by `ctrl_state_t` plus `lm_ctrl_ui_view_t`
- `machine_link_types.h`
  type-only machine status and field-mask contracts shared outside the link implementation
- `wifi_setup_types.h`
  type-only Wi-Fi/setup status contracts shared outside the Wi-Fi/setup implementation
- `wifi_setup.[ch]`
  AP/DNS/HTTP façade and setup portal orchestration
- `controller_settings.[ch]`
  persisted local settings
- `cloud_session.[ch]`
  public cloud REST/session surface
- `cloud_auth.c`
  internal cloud auth/signing implementation
- `cloud_machine_api.c`
  internal fleet/command implementation
- `cloud_dashboard.c`
  internal dashboard fetch/parse implementation
- `cloud_live_updates.[ch]`
  cloud websocket/live-update worker
- `machine_link.[ch]`
  public machine-link seam with injected cloud/settings dependencies
- `machine_link.c`
  machine-link core worker/reducer implementation
- `machine_link_ble.c`
  BLE transport and command implementation
- `machine_link_cloud.c`
  cloud fallback and websocket-ack implementation
- `setup_portal_routes.[ch]`
  setup portal route registration and HTTP handler implementation
- `setup_portal_presenter.[ch]`
  setup portal view-model assembly between route handlers and pure page rendering
- `setup_portal_page.[ch]`
  pure HTML rendering against DTO headers only; no direct runtime/service calls

Additional module notes:

- `docs/controller/FIRMWARE_ARCHITECTURE.md`

## Known limitations

- The firmware currently targets a specific JC3636K718-style board family.
- Some machine capabilities still depend on La Marzocco cloud/backend behaviour.
- Brew by weight is only shown when the cloud dashboard exposes BBW support for the selected machine, and BBW changes require a live cloud path.
- The shot timer only appears when the cloud dashboard exposes usable brewing fields. It is implemented for Micra/Linea Mini-style dashboard signals, but not yet confirmed on a real Micra or Linea Mini.
- The setup flow and UI are optimized for this project, not for generic reuse across controller hardware.

## Build & flash quickstart

`dev.sh` is the local Bash helper for macOS/Linux. Native Windows flashing is documented through the standard ESP-IDF terminal flow in `docs/controller/FLASHING_CONTROLLER.md`.

### macOS / Linux

```bash
cd firmware/esp32
./dev.sh full
```

For iterative changes after the first full flash:

```bash
cd firmware/esp32
./dev.sh quick
```

Useful macOS/Linux helper commands:

```bash
./dev.sh test
./dev.sh build
./dev.sh monitor
./dev.sh flash
ESPPORT=/dev/cu.usbmodemXXXX ./dev.sh quick
```

If you want the host-side firmware tests to run automatically before every local `git push`, install the repo-local hook once:

```bash
./dev.sh install-hooks
```

The hook runs `./firmware/esp32/dev.sh test` from the repository root. On a fresh checkout, build the firmware once first so `managed_components/` is available for the host test runner.

The `quick` command uses `idf.py app-flash monitor`, so only the app partition is reflashed after the initial full flash.

If `partitions.csv` changes, run `./dev.sh full` once again instead of `quick`.
Partition-layout changes can move the app offset, so reflashing only the app is
not sufficient for that transition.

`./dev.sh test` runs the host-side unit-test harness for the pure controller state machine, shot-timer state machine, cloud JSON parsing, and setup portal HTML rendering seams. It does not exercise BLE, Wi-Fi drivers, or on-device ESP-IDF runtime tasks.

### Windows

Use the native ESP-IDF Windows environment and flash with:

```powershell
cd C:\dev\lamarzocco\firmware\esp32
idf.py set-target esp32s3
idf.py -p COM5 flash monitor
```

Use an ESP-IDF-managed terminal such as `ESP-IDF PowerShell` or `ESP-IDF Command Prompt`, and keep the project path free of spaces. See the flashing guide below for the full Windows setup flow.

Detailed flashing guide and setup notes:

- `docs/controller/FLASHING_CONTROLLER.md`
- `docs/controller/SETUP_GUIDE.md`
