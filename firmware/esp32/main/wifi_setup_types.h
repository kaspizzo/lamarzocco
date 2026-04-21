#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cloud_machine_status.h"
#include "controller_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Persisted LAN web-access policy for the setup portal outside the captive setup AP. */
typedef enum {
  LM_CTRL_WEB_AUTH_UNSET = 0,
  LM_CTRL_WEB_AUTH_DISABLED,
  LM_CTRL_WEB_AUTH_ENABLED,
} lm_ctrl_web_auth_mode_t;

/** Fixed on-device header logo width used for optional custom uploads. */
#define LM_CTRL_CUSTOM_LOGO_WIDTH 150
/** Fixed on-device header logo height used for optional custom uploads. */
#define LM_CTRL_CUSTOM_LOGO_HEIGHT 26
/** Schema version for persisted controller header logo data. */
#define LM_CTRL_CUSTOM_LOGO_SCHEMA_VERSION 1
/** Fixed byte size for LVGL TRUE_COLOR_ALPHA data at the configured dimensions. */
#define LM_CTRL_CUSTOM_LOGO_BLOB_SIZE (LM_CTRL_CUSTOM_LOGO_WIDTH * LM_CTRL_CUSTOM_LOGO_HEIGHT * 3)

/** Snapshot of Wi-Fi, portal, and cloud selection state for the controller UI. */
typedef struct {
  bool has_credentials;
  bool has_cloud_credentials;
  bool cloud_connected;
  bool cloud_probe_active;
  bool cloud_live_updates_active;
  bool cloud_ws_transport_connected;
  bool cloud_ws_connected;
  bool cloud_machine_status_known;
  bool machine_cloud_online;
  bool has_machine_selection;
  bool has_custom_logo;
  bool has_cloud_provisioning;
  bool heat_display_enabled;
  bool debug_screenshot_enabled;
  bool portal_running;
  bool sta_connecting;
  bool sta_connected;
  lm_ctrl_web_auth_mode_t web_auth_mode;
  ctrl_language_t language;
  lm_ctrl_cloud_machine_status_t cloud_machine_status;
  char portal_ssid[33];
  char portal_password[65];
  char sta_ssid[33];
  char hostname[33];
  char sta_ip[16];
  char cloud_username[96];
  char machine_name[64];
  char machine_model[32];
  char machine_serial[32];
  uint8_t cloud_http_requests_in_flight;
} lm_ctrl_wifi_info_t;

/** Live brew timer state derived from cloud dashboard websocket updates. */
typedef struct {
  bool websocket_connected;
  bool brew_active;
  bool available;
  uint32_t seconds;
} lm_ctrl_shot_timer_info_t;

#ifdef __cplusplus
}
#endif
