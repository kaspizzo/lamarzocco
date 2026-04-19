#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cloud_api.h"
#include "controller_state.h"
#include "esp_err.h"
#include "wifi_setup_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Persisted cloud-selected machine binding used for BLE and cloud commands. */
typedef struct {
  bool configured;
  char serial[32];
  char name[64];
  char model[32];
  char communication_key[128];
} lm_ctrl_machine_binding_t;

/** Load persisted controller, Wi-Fi, cloud, logo, and machine-binding settings into runtime state. */
esp_err_t lm_ctrl_settings_load(void);
/** Persist home Wi-Fi credentials together with controller-local hostname and language state. */
esp_err_t lm_ctrl_settings_save_wifi_credentials(const char *ssid, const char *password, const char *hostname, ctrl_language_t language);
/** Persist controller-local hostname and language preferences only. */
esp_err_t lm_ctrl_settings_save_controller_preferences(const char *hostname, ctrl_language_t language);
/** Persist a browser-rasterized controller header logo blob. */
esp_err_t lm_ctrl_settings_save_controller_logo(uint8_t schema_version, const uint8_t *logo_data, size_t logo_size);
/** Remove the persisted controller header logo blob. */
esp_err_t lm_ctrl_settings_clear_controller_logo(void);
/** Persist cloud credentials and report whether the account identity changed. */
esp_err_t lm_ctrl_settings_save_cloud_credentials(const char *username, const char *password, bool *credentials_changed);
/** Persist the selected cloud machine binding used by BLE and cloud command paths. */
esp_err_t lm_ctrl_settings_save_machine_selection(const lm_ctrl_cloud_machine_t *machine);
/** Copy the effective machine selection, auto-falling back to the only fleet entry when unambiguous. */
bool lm_ctrl_settings_get_effective_selected_machine(lm_ctrl_cloud_machine_t *machine);
/** Persist or replace the device-local cloud provisioning bundle used for signed requests. */
esp_err_t lm_ctrl_settings_save_cloud_provisioning(
  const char *installation_id,
  const uint8_t *secret,
  const uint8_t *private_key_der,
  size_t private_key_der_len
);
/** Ensure that device-local cloud provisioning exists, generating fresh per-device material if needed. */
esp_err_t lm_ctrl_settings_ensure_cloud_provisioning(void);
/** Persist the cloud-installation registration state cached for the current provisioning bundle. */
esp_err_t lm_ctrl_settings_set_cloud_installation_registered(bool registered);
/** Persist the setup-AP password shown on the on-device setup screen and QR code. */
esp_err_t lm_ctrl_settings_save_portal_password(const char *password);
/** Persist a LAN admin password hash and enable authenticated web access outside the setup AP. */
esp_err_t lm_ctrl_settings_save_web_admin_password(const char *password);
/** Disable LAN web auth and clear the persisted admin password hash. */
esp_err_t lm_ctrl_settings_clear_web_admin_password(void);
/** Verify a candidate LAN admin password against the persisted hash. */
bool lm_ctrl_settings_verify_web_admin_password(const char *password);
/** Persist whether the remote debug screenshot endpoint is enabled. */
esp_err_t lm_ctrl_settings_set_debug_screenshot_enabled(bool enabled);
/** Clear Wi-Fi, cloud-account, and machine-binding settings while keeping device provisioning and other controller state. */
esp_err_t lm_ctrl_settings_reset_network(void);
/** Clear all persisted controller settings, including device provisioning, presets, and custom logo data. */
esp_err_t lm_ctrl_settings_factory_reset(void);
/** Copy the active machine binding into the supplied output structure. */
bool lm_ctrl_settings_get_machine_binding(lm_ctrl_machine_binding_t *binding);

#ifdef __cplusplus
}
#endif
