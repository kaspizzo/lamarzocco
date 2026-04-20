# Controller Firmware Architecture

Last updated: 2026-04-20

## Goal

Keep the ESP32 firmware split by responsibility so Wi-Fi/setup, persisted settings, cloud session handling, live updates, machine transport, UI rendering, and runtime coordination can change independently.

## Module map

- `firmware/esp32/main/app_main.c`
  minimal bootstrap, hardware bring-up, and top-level event-loop wiring
- `firmware/esp32/main/controller_runtime.[ch]`
  runtime coordinator for controller state, sync cadence, deferred sends, and UI view-model construction
- `firmware/esp32/main/controller_shot_timer.[ch]`
  small runtime-owned shot-timer state machine for live/sticky/hidden transitions
- `firmware/esp32/main/controller_ui.[ch]`
  render-only LVGL UI; no direct Wi-Fi or cloud reads
- `firmware/esp32/main/machine_link_types.h`
  type-only machine DTOs and field/feature bitmasks shared with UI and setup-page rendering
- `firmware/esp32/main/wifi_setup_types.h`
  type-only Wi-Fi/setup DTOs shared with UI/runtime and setup-page rendering
- `firmware/esp32/main/controller_state.[ch]`
  portable controller state machine and preset persistence
- `firmware/esp32/main/wifi_setup.[ch]`
  façade/orchestrator for AP/portal lifecycle, DNS, HTTP routes, and setup-page assembly
- `firmware/esp32/main/controller_settings.[ch]`
  NVS-backed settings for Wi-Fi, language, hostname, custom logo, cloud credentials, and selected machine binding
- `firmware/esp32/main/cloud_session.[ch]`
  public cloud REST/session API consumed by the rest of the firmware
- `firmware/esp32/main/cloud_auth.c`
  internal auth/signing implementation: installation bootstrap, token cache, and websocket header preparation
- `firmware/esp32/main/cloud_machine_api.c`
  internal fleet/command implementation: fleet refresh, selection restore, and machine command requests
- `firmware/esp32/main/cloud_dashboard.c`
  internal dashboard implementation: dashboard fetch, prebrew parsing, and dashboard summaries
- `firmware/esp32/main/cloud_live_updates.[ch]`
  cloud reachability probes plus websocket/STOMP live-update handling
- `firmware/esp32/main/machine_link.[ch]`
  public machine-link seam with explicit injected dependencies
- `firmware/esp32/main/machine_link.c`
  machine-link core state, reducer/worker logic, and public API glue
- `firmware/esp32/main/machine_link_ble.c`
  BLE transport, authentication, read/write helpers, and direct machine commands
- `firmware/esp32/main/machine_link_cloud.c`
  cloud fallback fetches, async command acknowledgement handling, and dashboard update application
- `firmware/esp32/main/setup_portal_page.[ch]`
  pure server-side HTML generation from a prepared view model
- `firmware/esp32/main/setup_portal_http.[ch]`
  shared HTTP/form/JSON helpers used by the setup portal
- `firmware/esp32/main/setup_portal_presenter.[ch]`
  setup portal view-model assembly and response rendering orchestration
- `firmware/esp32/main/setup_portal_routes.[ch]`
  setup portal route registration, request parsing, and captive-portal HTTP handlers

## Dependency direction

- `app_main` wires modules together.
- `controller_ui` depends on `controller_state` and a prepared `lm_ctrl_ui_view_t`, not on Wi-Fi/cloud modules.
- `controller_ui` and `setup_portal_page` consume type-only DTO headers (`machine_link_types.h`, `wifi_setup_types.h`) instead of service headers.
- `machine_link` depends on injected callbacks, not directly on `wifi_setup`.
- `wifi_setup` depends on `controller_settings`, `cloud_session`, and `cloud_live_updates`.
- `cloud_session.h` is the public seam; `cloud_auth.c`, `cloud_machine_api.c`, and `cloud_dashboard.c` provide the split implementation behind it.
- `cloud_live_updates` depends on `cloud_session` for token/header preparation.
- `setup_portal_page` is pure rendering and should not trigger network side effects.
- `setup_portal_page` is intentionally fed by `setup_portal_presenter`; the page layer should stay free of runtime lookups.
- `setup_portal_presenter` prepares the page view model from runtime state but does not own HTTP routes.
- `setup_portal_routes` owns request parsing and route registration, while `wifi_setup` keeps AP/DNS and station orchestration.

## Inline documentation style

The firmware uses Doxygen-style inline comments:

- `/** ... */` for public types and functions in headers
- short local comments only where the code would otherwise be hard to parse

There is no JavaScript-style `JSDoc` tooling here, but the purpose is the same.

## Test harness

There is now a lightweight host-side test harness behind `firmware/esp32/dev.sh test`.

- It targets seams that are worth checking without ESP-IDF hardware drivers: `controller_state`, `controller_shot_timer`, cloud JSON parsing in `cloud_api`, and setup portal rendering in `setup_portal_page`.
- It does not try to fake BLE, Wi-Fi driver state, LVGL runtime, or full HTTP/TLS integration.
- The goal is fast regression coverage for the refactored pure/module boundaries, not a full firmware simulator.
