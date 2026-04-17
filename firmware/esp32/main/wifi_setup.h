#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "controller_state.h"

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
  bool has_machine_selection;
  bool has_custom_logo;
  bool portal_running;
  bool sta_connecting;
  bool sta_connected;
  ctrl_language_t language;
  char portal_ssid[33];
  char portal_password[65];
  char sta_ssid[33];
  char hostname[33];
  char sta_ip[16];
  char cloud_username[96];
  char machine_name[64];
  char machine_model[32];
  char machine_serial[32];
} lm_ctrl_wifi_info_t;

/** Live brew timer state derived from cloud dashboard websocket updates. */
typedef struct {
  bool websocket_connected;
  bool brew_active;
  bool available;
  uint32_t seconds;
} lm_ctrl_shot_timer_info_t;

/** Accepted cloud command metadata returned from the machine command API. */
typedef struct {
  bool accepted;
  char command_id[64];
  char command_status[24];
  char error_code[32];
} lm_ctrl_cloud_command_result_t;

/** Persisted cloud-selected machine binding used for BLE and cloud commands. */
typedef struct {
  bool configured;
  char serial[32];
  char name[64];
  char model[32];
  char communication_key[128];
} lm_ctrl_machine_binding_t;

/** Initialize Wi-Fi, NVS-backed settings, captive portal helpers, and cloud state. */
esp_err_t lm_ctrl_wifi_init(void);
/** Start the setup AP and local configuration portal if needed. */
esp_err_t lm_ctrl_wifi_start_portal(void);
/** Persist controller-local preferences such as hostname and language. */
esp_err_t lm_ctrl_wifi_save_controller_preferences(const char *hostname, ctrl_language_t language);
/** Persist a browser-rasterized custom header logo for the on-device controller UI. */
esp_err_t lm_ctrl_wifi_save_controller_logo(uint8_t schema_version, const uint8_t *logo_data, size_t logo_size);
/** Remove the persisted custom header logo so the controller falls back to text. */
esp_err_t lm_ctrl_wifi_clear_controller_logo(void);
/** Copy the latest Wi-Fi/cloud runtime state into the supplied structure. */
void lm_ctrl_wifi_get_info(lm_ctrl_wifi_info_t *info);
/** Return the optional custom header logo descriptor used by the on-device UI. */
const lv_img_dsc_t *lm_ctrl_wifi_get_custom_logo(void);
/** Format a user-facing setup status summary for the on-device setup screen. */
void lm_ctrl_wifi_format_status(char *buffer, size_t buffer_size);
/** Build the Wi-Fi QR payload shown on the controller setup screen. */
void lm_ctrl_wifi_get_setup_qr_payload(char *buffer, size_t buffer_size);
/** Request an asynchronous probe of cloud reachability and credentials. */
esp_err_t lm_ctrl_wifi_request_cloud_probe(void);
/** Ensure the live cloud websocket path gets a chance to start when conditions allow it. */
esp_err_t lm_ctrl_wifi_request_live_updates(void);
/** Report whether the live cloud websocket path is currently starting or connected. */
bool lm_ctrl_wifi_live_updates_active(void);
/** Report whether the live cloud websocket path is fully connected and subscribed. */
bool lm_ctrl_wifi_live_updates_connected(void);
/** Read the live shot timer state if the cloud websocket currently provides it. */
bool lm_ctrl_wifi_get_shot_timer_info(lm_ctrl_shot_timer_info_t *info);
/** Monotonic version counter that changes when Wi-Fi or portal state changes. */
uint32_t lm_ctrl_wifi_status_version(void);
/** Copy the currently selected machine binding if one has been configured. */
bool lm_ctrl_wifi_get_machine_binding(lm_ctrl_machine_binding_t *binding);
/** Execute a signed cloud command against the currently selected machine. */
esp_err_t lm_ctrl_wifi_execute_machine_command(
  const char *command,
  const char *json_body,
  lm_ctrl_cloud_command_result_t *result,
  char *status_text,
  size_t status_text_size
);
/** Read machine-facing dashboard values from the La Marzocco cloud. */
esp_err_t lm_ctrl_wifi_fetch_dashboard_values(ctrl_values_t *values, uint32_t *loaded_mask, uint32_t *feature_mask);
/** Read only the prebrewing timing values from the cloud dashboard. */
esp_err_t lm_ctrl_wifi_fetch_prebrewing_values(float *seconds_in, float *seconds_out);
/** Log and summarize the current prebrewing-related cloud dashboard state. */
esp_err_t lm_ctrl_wifi_log_prebrew_dashboard_state(char *status_text, size_t status_text_size);
/** Clear Wi-Fi, cloud, and machine binding settings, then reboot into setup mode. */
esp_err_t lm_ctrl_wifi_reset_network(void);
/** Clear controller, Wi-Fi, cloud, and preset state, then reboot into factory defaults. */
esp_err_t lm_ctrl_wifi_factory_reset(void);
/** Reboot the controller after replying to the current request. */
esp_err_t lm_ctrl_wifi_schedule_reboot(void);
