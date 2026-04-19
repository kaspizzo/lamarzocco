#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lvgl.h"
#include "cloud_session.h"
#include "controller_settings.h"
#include "controller_state.h"
#include "machine_link_types.h"
#include "wifi_setup_types.h"

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
/** Persist or replace the cloud provisioning bundle used for signed La Marzocco requests. */
esp_err_t lm_ctrl_wifi_save_cloud_provisioning(
  const char *installation_id,
  const uint8_t *secret,
  const uint8_t *private_key_der,
  size_t private_key_der_len
);
/** Persist a LAN admin password and require it for portal access outside the setup AP. */
esp_err_t lm_ctrl_wifi_save_web_admin_password(const char *password);
/** Disable LAN portal auth and clear the persisted admin password hash. */
esp_err_t lm_ctrl_wifi_clear_web_admin_password(void);
/** Verify a candidate LAN admin password against the persisted hash. */
bool lm_ctrl_wifi_verify_web_admin_password(const char *password);
/** Persist whether the on-device heating status UI is enabled. */
esp_err_t lm_ctrl_wifi_set_heat_display_enabled(bool enabled);
/** Persist whether the remote debug screenshot endpoint is enabled. */
esp_err_t lm_ctrl_wifi_set_debug_screenshot_enabled(bool enabled);
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
esp_err_t lm_ctrl_wifi_fetch_dashboard_values(
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  lm_ctrl_machine_heat_info_t *heat_info
);
/** Read only the prebrewing timing values from the cloud dashboard. */
esp_err_t lm_ctrl_wifi_fetch_prebrewing_values(float *seconds_in, float *seconds_out);
/** Clear Wi-Fi, cloud, and machine binding settings, then reboot into setup mode. */
esp_err_t lm_ctrl_wifi_reset_network(void);
/** Clear controller, Wi-Fi, cloud, and preset state, then reboot into factory defaults. */
esp_err_t lm_ctrl_wifi_factory_reset(void);
/** Reboot the controller after replying to the current request. */
esp_err_t lm_ctrl_wifi_schedule_reboot(void);
