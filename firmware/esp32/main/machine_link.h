#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "cloud_session.h"
#include "controller_settings.h"
#include "controller_state.h"
#include "machine_link_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** External operations the machine link needs from Wi-Fi/cloud services. */
typedef struct {
  bool (*get_machine_binding)(lm_ctrl_machine_binding_t *binding);
  esp_err_t (*execute_cloud_command)(
    const char *command,
    const char *json_body,
    lm_ctrl_cloud_command_result_t *result,
    char *status_text,
    size_t status_text_size
  );
  esp_err_t (*fetch_dashboard_values)(
    ctrl_values_t *values,
    uint32_t *loaded_mask,
    uint32_t *feature_mask,
    lm_ctrl_machine_heat_info_t *heat_info,
    lm_ctrl_machine_water_status_t *water_status
  );
} lm_ctrl_machine_link_deps_t;

/** Start the machine link worker and restore any persisted binding context. */
esp_err_t lm_ctrl_machine_link_init(const lm_ctrl_machine_link_deps_t *deps);
/** Queue a write for one or more machine fields using the supplied desired values. */
esp_err_t lm_ctrl_machine_link_queue_values(const ctrl_values_t *values, uint32_t field_mask);
/** Request a read/synchronization cycle without changing any values. */
esp_err_t lm_ctrl_machine_link_request_sync(void);
/** Request a sync cycle for specific sources such as BLE, cloud, or both. */
esp_err_t lm_ctrl_machine_link_request_sync_mode(uint32_t sync_flags);
/** Copy the most recently synchronized machine values and the loaded field mask. */
bool lm_ctrl_machine_link_get_values(ctrl_values_t *values, uint32_t *loaded_mask, uint32_t *feature_mask);
/** Copy the latest warmup status mirrored from the cloud dashboard. */
void lm_ctrl_machine_link_get_heat_info(lm_ctrl_machine_heat_info_t *info);
/** Apply dashboard values delivered asynchronously by the cloud websocket. */
void lm_ctrl_machine_link_apply_cloud_dashboard_values(
  const ctrl_values_t *values,
  uint32_t loaded_mask,
  uint32_t feature_mask,
  const lm_ctrl_machine_heat_info_t *heat_info,
  const lm_ctrl_machine_water_status_t *water_status
);
/** Apply command updates delivered asynchronously by the cloud websocket. */
void lm_ctrl_machine_link_apply_cloud_command_updates(const lm_ctrl_cloud_command_update_t *updates, size_t update_count);
/** Report whether the cloud websocket path is active and whether it is fully subscribed. */
void lm_ctrl_machine_link_set_live_updates_state(bool active, bool connected);
/** Clear any in-flight cloud command waits when the websocket disconnects. */
void lm_ctrl_machine_link_handle_cloud_websocket_disconnect(void);
/** Format a short human-readable status message for diagnostics. */
void lm_ctrl_machine_link_get_status(char *buffer, size_t buffer_size);
/** Copy the latest machine link runtime flags. */
void lm_ctrl_machine_link_get_info(lm_ctrl_machine_link_info_t *info);
/** Monotonic version counter that changes when machine link state changes. */
uint32_t lm_ctrl_machine_link_status_version(void);

#ifdef __cplusplus
}
#endif
