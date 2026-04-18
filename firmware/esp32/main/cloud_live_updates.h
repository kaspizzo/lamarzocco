#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "wifi_setup_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start an async cloud reachability probe if Wi-Fi and cloud credentials are ready. */
esp_err_t lm_ctrl_cloud_live_updates_request_probe(void);
/** Request the cloud websocket path when the runtime conditions allow it. */
esp_err_t lm_ctrl_cloud_live_updates_request_start(void);
/** Re-evaluate whether the websocket worker should run and start it if needed. */
esp_err_t lm_ctrl_cloud_live_updates_ensure_task(void);
/** Stop the websocket worker and optionally wait for its task to exit. */
void lm_ctrl_cloud_live_updates_stop(bool wait_for_stop);
/** Return whether the websocket path is starting or already connected. */
bool lm_ctrl_cloud_live_updates_active(void);
/** Return whether the websocket is fully connected and subscribed. */
bool lm_ctrl_cloud_live_updates_connected(void);
/** Read the latest websocket-derived shot timer snapshot. */
bool lm_ctrl_cloud_live_updates_get_shot_timer_info(lm_ctrl_shot_timer_info_t *info);

#ifdef __cplusplus
}
#endif
