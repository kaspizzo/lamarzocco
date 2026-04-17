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
  - steam boiler
  - standby
  - presets
  - setup
- `input.[ch]`
  vendor-style polling for the physical outer ring
- `board_display.[ch]`
  ST77916 QSPI display bring-up plus CST816S touch registration
- `board_backlight.[ch]`
  PWM backlight control
- `board_haptic.[ch]`
  DRV2605L click feedback
- `wifi_setup.[ch]`
  setup portal with:
  - Wi-Fi scan
  - home Wi-Fi storage
  - on-device language selection (`English` default, optional `Deutsch`)
  - optional SVG upload for a custom controller header logo
  - cloud credential storage
  - machine loading and selection
  - setup AP QR generation and captive-portal helpers
- `machine_link.[ch]`
  machine command transport:
  - BLE for coffee boiler temperature, steam boiler, standby
  - cloud for prebrewing mode/times and dashboard fallback values
- `dev.sh`
  fast build/flash/monitor loop

## On-device gestures

- Swipe left/right: next or previous main settings page
- Swipe down: open presets
- Swipe up: open Wi-Fi/cloud setup
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

They do not store or restore:

- steam boiler enable
- standby

## Setup portal

If no home Wi-Fi is stored, the controller starts its own setup AP automatically:

- SSID: `LM-CTRL-XXXX`
- Password: shown on the controller setup screen
- URL: `http://192.168.4.1`

On first boot without stored Wi-Fi credentials, and again after a full factory reset, the controller opens the setup screen automatically so the AP QR code is visible immediately.

The same setup portal can be reopened any time from the on-device setup screen.

The browser portal currently supports:

- storing the La Marzocco cloud email/password locally on the controller
- scanning nearby Wi-Fi networks so the SSID does not need to be typed manually
- saving home Wi-Fi credentials and reconnecting immediately
- keeping the default text header or storing a user-provided custom logo as a controller setting
- loading machines from the cloud account
- selecting the active machine and storing the serial/BLE binding needed by the controller

The cloud login path expects a direct La Marzocco account email/password. Accounts created only through Apple or Google sign-in are not expected to work with the current controller onboarding flow. A possible workaround is to create a second La Marzocco account with a normal email/password login and share machine access to that account in the official app.

The firmware does not ship an official vendor logo asset. By default, the display header uses text (`la marzocco`). If you want a logo on the device, upload a local SVG in the setup portal; the browser rasterizes it to the controller header format before the generated bitmap is stored on-device.

## Runtime sync behaviour

- BLE is preferred whenever the controller has an authenticated local machine link.
- Cloud is used for prebrewing writes and as a dashboard-value fallback.
- The controller refreshes machine-facing values periodically while it stays online.
- If a value has not been loaded yet, the UI shows placeholders instead of stale defaults.

## Known limitations

- The firmware currently targets a specific JC3636K718-style board family.
- Some machine capabilities still depend on La Marzocco cloud/backend behaviour.
- The setup flow and UI are optimized for this project, not for generic reuse across controller hardware.

## Build & flash quickstart

```bash
cd firmware/esp32
./dev.sh full
```

For iterative changes after the first full flash:

```bash
cd firmware/esp32
./dev.sh quick
```

Useful commands:

```bash
./dev.sh build
./dev.sh monitor
./dev.sh flash
ESPPORT=/dev/cu.usbmodemXXXX ./dev.sh quick
```

The `quick` command uses `idf.py app-flash monitor`, so only the app partition is reflashed after the initial full flash.

Detailed flashing guide and setup notes:

- `docs/controller/FLASHING_CONTROLLER.md`
- `docs/controller/SETUP_GUIDE.md`
