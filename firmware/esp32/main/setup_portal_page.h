#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "cloud_api.h"
#include "controller_state.h"
#include "machine_link_types.h"
#include "wifi_setup_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Read-only view model for the setup portal HTML page. */
typedef struct {
  char status_html[512];
  char machine_status[160];
  char debug_status_html[768];
  char banner_html[256];
  char ssid_html[96];
  char hostname_html[96];
  char cloud_user_html[192];
  char local_url_html[160];
  char selected_machine_html[224];
  ctrl_values_t dashboard_values;
  uint32_t dashboard_loaded_mask;
  uint32_t dashboard_feature_mask;
  ctrl_preset_t presets[CTRL_PRESET_MAX_COUNT];
  uint8_t preset_count;
  float temperature_step_c;
  float time_step_s;
  lm_ctrl_wifi_info_t info;
  lm_ctrl_machine_link_info_t machine_info;
  lm_ctrl_cloud_machine_t fleet[8];
  size_t fleet_count;
} lm_ctrl_setup_portal_view_t;

const char *lm_ctrl_setup_portal_history_target(const char *uri);
esp_err_t lm_ctrl_setup_portal_send_page(
  httpd_req_t *req,
  const lm_ctrl_setup_portal_view_t *view,
  const char *history_target
);

#ifdef __cplusplus
}
#endif
