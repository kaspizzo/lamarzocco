#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "controller_state.h"
#include "machine_link_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Accepted cloud command metadata returned from the machine command API. */
typedef struct {
  bool accepted;
  char command_id[64];
  char command_status[24];
  char error_code[32];
} lm_ctrl_cloud_command_result_t;

/** Refresh the customer fleet from the cloud account and optionally persist an auto-restored selection. */
esp_err_t lm_ctrl_cloud_session_refresh_fleet(char *banner_text, size_t banner_text_size, bool *selection_changed);
/** Execute a signed cloud command against the currently selected machine. */
esp_err_t lm_ctrl_cloud_session_execute_machine_command(
  const char *command,
  const char *json_body,
  lm_ctrl_cloud_command_result_t *result,
  char *status_text,
  size_t status_text_size
);
/** Fetch the current dashboard values for the selected machine. */
esp_err_t lm_ctrl_cloud_session_fetch_dashboard_values(
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  lm_ctrl_machine_heat_info_t *heat_info,
  lm_ctrl_machine_water_status_t *water_status
);
/** Fetch only the prebrewing timing values from the cloud dashboard. */
esp_err_t lm_ctrl_cloud_session_fetch_prebrewing_values(float *seconds_in, float *seconds_out);
/** Fetch a raw JSON debug view of the warmup-related dashboard widgets. The caller owns the returned string. */
esp_err_t lm_ctrl_cloud_session_fetch_heat_debug_json(
  char **json_text,
  char *error_text,
  size_t error_text_size
);
/** Log and summarize the prebrewing-related dashboard widgets. */
esp_err_t lm_ctrl_cloud_session_log_prebrew_dashboard_state(char *status_text, size_t status_text_size);
/** Return a cached cloud access token or sign in again if the cache is stale. */
esp_err_t lm_ctrl_cloud_session_fetch_access_token_cached(
  const char *username,
  const char *password,
  char *access_token,
  size_t access_token_size,
  char *error_text,
  size_t error_text_size
);
/** Build signed HTTP headers for the cloud websocket upgrade request. */
esp_err_t lm_ctrl_cloud_session_build_websocket_headers(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif
