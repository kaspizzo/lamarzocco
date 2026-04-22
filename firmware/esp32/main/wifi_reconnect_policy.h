#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LM_CTRL_WIFI_RECONNECT_BASE_DELAY_MS 1000U
#define LM_CTRL_WIFI_RECONNECT_MAX_DELAY_MS 30000U
#define LM_CTRL_WIFI_RECONNECT_AP_FALLBACK_THRESHOLD 4U
#define LM_CTRL_WIFI_RECONNECT_MAX_DISCONNECT_COUNT 8U

typedef struct {
  uint8_t disconnect_count;
  uint32_t retry_delay_ms;
  bool should_enable_setup_ap;
} lm_ctrl_wifi_reconnect_plan_t;

/** Compute the next reconnect attempt for a disconnected STA path. */
lm_ctrl_wifi_reconnect_plan_t lm_ctrl_wifi_reconnect_plan_next(uint8_t previous_disconnect_count, bool portal_running);

#ifdef __cplusplus
}
#endif
