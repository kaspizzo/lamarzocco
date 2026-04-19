#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cloud_api.h"
#include "controller_state.h"
#include "esp_err.h"

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
/** Clear Wi-Fi, cloud, installation, and machine-binding settings while keeping other controller state. */
esp_err_t lm_ctrl_settings_reset_network(void);
/** Clear all persisted controller settings, including presets and custom logo data. */
esp_err_t lm_ctrl_settings_factory_reset(void);
/** Copy the active machine binding into the supplied output structure. */
bool lm_ctrl_settings_get_machine_binding(lm_ctrl_machine_binding_t *binding);

#ifdef __cplusplus
}
#endif
