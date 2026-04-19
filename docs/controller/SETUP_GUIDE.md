# Controller Setup Mode (Wi-Fi + La Marzocco Cloud)

This guide describes the current setup flow for the round controller firmware.

Visual reference:

- [SCREENSHOTS.md](./SCREENSHOTS.md)

## Goals

- document the first-boot and post-reset onboarding path
- store home Wi-Fi credentials on the controller
- connect a La Marzocco cloud account
- select the active machine and persist its binding
- keep the default text header or store an optional custom header logo as a controller setting
- explain the current connectivity indicators and recovery actions
- optionally inspect BLE/cloud state in the simulator tooling

## Simulator flow

1. **Open setup** using the `Setup` button or the `S` key.
2. Fill in **Wi-Fi & Portal**:
   - Wi-Fi SSID
   - Wi-Fi password
   - country code
   - setup hostname
3. Click **Save setup**.
   The values are stored locally in browser `localStorage`.
4. In **Machine Link**, enter cloud credentials:
   - account email
   - password
   - click `Load machines`
5. Select the machine from the list with `Use`.
   This stores the machine serial in the simulator state.
6. Optionally use the **Bluetooth** section to:
   - BLE scan
   - set an address
   - copy or inspect the BLE token
7. Desktop-side simulator and inspector tooling are maintained separately and are no longer included in this repository.

## Current ESP32 flow

The current firmware provides a real local setup path:

1. On first boot without stored Wi-Fi credentials, the controller opens `Setup` automatically and starts a setup AP.
2. The same automatic setup path happens again after a full `Factory Reset`.
3. In that onboarding state, the device shows a QR code for the setup AP together with the AP name, password, and setup IP.
4. Scan the QR code with a phone or join the controller AP manually, then open the captive portal.
5. If the captive portal does not open automatically, open `http://192.168.4.1/` manually.
6. In the browser portal:
   - optionally change the controller hostname
   - optionally switch the controller UI language
   - optionally upload a custom local SVG header logo
7. In **Network**, save the home Wi-Fi SSID and password.
8. After saving Wi-Fi, the controller immediately tries to join the configured home network.
9. Once the controller is on the home network, the setup portal remains available at `http://<hostname>.local/`.
10. In **La Marzocco Cloud**, store the cloud account email/password and load the machine list.
11. Select the active machine in **Select Machine** so the controller can persist the machine binding.
12. The browser portal currently supports:
   - `Overview` with portal reachability, current IP, and stable `.local` URL
   - `Controller` settings for hostname, language (`English` default), and optional local SVG header logo
   - `Network` with Wi-Fi storage and scan
   - `Cloud` account storage, machine loading/selection, and setup-AP provisioning import
   - `Recipes` for controller preset editing
   - `Advanced` controller tuning and factory reset
   - `Diagnostics` for cloud heat debug and the optional remote screenshot route
13. Swipe up on the main controller screen to reopen `Setup` later.
14. Swipe down on the main controller screen to open `Presets`.

## Connectivity indicators

- The Bluetooth icon is shown when the controller currently has a BLE path to the machine.
- A crossed Wi-Fi icon means the controller has network/account context but the selected machine is not currently reachable through cloud.
- A solid Wi-Fi icon means the selected machine is reachable through cloud.
- USB and battery indicators appear only when the controller reports USB power, charging, or low-battery state.
- In the crossed-Wi-Fi state, local BLE control for temperature, steam, and status can stay available while cloud-only values such as prebrewing continue to wait for the remote path.

## Cloud account note

- The controller login flow expects a direct La Marzocco account email/password.
- Accounts created only through Apple sign-in or Google sign-in are not expected to work with the current cloud onboarding path.
- This matches the general upstream assumption in `pylamarzocco`, which authenticates with a stored username/password pair.
- A possible workaround is to create a second La Marzocco account with a regular email/password login and grant that account machine access in the official app. This is a practical suggestion, not a guaranteed compatibility promise.

## On-device gestures and reset behaviour

- Swipe left/right on the main screen to move between the main settings pages.
- Swipe down on the main screen to open `Presets`.
- Swipe up on the main screen to open `Setup`.
- On the `Presets` screen, swipe up to return to the main screen.
- On the `Setup` screen, swipe down to return to the main screen.
- Long-press on the `Setup` screen to open the on-device `Recovery` flow.
- `Recovery` currently offers `Clear web password` and `Reset network`.
- `Clear web password` removes the LAN portal password outside the setup AP and keeps the stored Wi-Fi, cloud, and machine-binding state.
- `Reset Network` clears Wi-Fi, cloud credentials, and machine selection, then reboots back into setup mode.
- Full `Factory Reset` is available in the setup portal under `Advanced`; it also erases presets and the optional custom logo.

## Header logo setting

- The controller ships with a text header (`la marzocco`) by default.
- The setup portal can store an optional custom header logo as a controller setting.
- Uploads are local SVG files only. The browser rasterizes the SVG to the fixed LVGL header format before the generated bitmap is sent to the device.
- The project does not bundle an official La Marzocco logo asset, and the portal does not fetch logo URLs on your behalf.
- `Reset Network` keeps the stored custom logo. `Factory Reset` removes it.

## Security note

- This is still a community firmware setup flow, not a hardened appliance setup flow.
- Credentials and tokens currently live in controller storage for practical operation.
- Before a wider public release, credential handling should be reviewed with a stricter security bar.

## Flashing

For step-by-step flashing instructions, see:

- [FLASHING_CONTROLLER.md](./FLASHING_CONTROLLER.md)
