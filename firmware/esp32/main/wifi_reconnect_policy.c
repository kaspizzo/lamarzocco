#include "wifi_reconnect_policy.h"

lm_ctrl_wifi_reconnect_plan_t lm_ctrl_wifi_reconnect_plan_next(uint8_t previous_disconnect_count, bool portal_running) {
  lm_ctrl_wifi_reconnect_plan_t plan = {0};
  uint32_t delay_ms = LM_CTRL_WIFI_RECONNECT_BASE_DELAY_MS;

  if (previous_disconnect_count >= LM_CTRL_WIFI_RECONNECT_MAX_DISCONNECT_COUNT) {
    plan.disconnect_count = LM_CTRL_WIFI_RECONNECT_MAX_DISCONNECT_COUNT;
  } else {
    plan.disconnect_count = (uint8_t)(previous_disconnect_count + 1U);
  }

  for (uint8_t attempt = 1; attempt < plan.disconnect_count; ++attempt) {
    if (delay_ms >= LM_CTRL_WIFI_RECONNECT_MAX_DELAY_MS) {
      delay_ms = LM_CTRL_WIFI_RECONNECT_MAX_DELAY_MS;
      break;
    }
    delay_ms *= 2U;
    if (delay_ms > LM_CTRL_WIFI_RECONNECT_MAX_DELAY_MS) {
      delay_ms = LM_CTRL_WIFI_RECONNECT_MAX_DELAY_MS;
    }
  }

  plan.retry_delay_ms = delay_ms;
  plan.should_enable_setup_ap =
    !portal_running && plan.disconnect_count >= LM_CTRL_WIFI_RECONNECT_AP_FALLBACK_THRESHOLD;
  return plan;
}
