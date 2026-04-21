/**
 * Runtime coordinator for controller state, sync scheduling, and UI view data.
 *
 * This module keeps the event loop logic out of app_main so bootstrapping stays
 * small and the runtime behaviour is easier to reason about and test.
 */
#include "controller_runtime.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "board_haptic.h"
#include "board_leds.h"
#include "board_power.h"
#include "controller_connectivity.h"
#include "machine_link.h"
#include "machine_link_policy.h"
#include "wifi_setup.h"

static const char *TAG = "lm_ctrl_runtime";
static const int64_t CLOUD_PROBE_INTERVAL_US = 60LL * 1000LL * 1000LL;
static const int64_t BLE_VALUE_REFRESH_INTERVAL_US = 15LL * 1000LL * 1000LL;
static const int64_t CLOUD_VALUE_REFRESH_INTERVAL_US = 60LL * 1000LL * 1000LL;
static const int64_t LOCAL_VALUE_HOLD_US = 20LL * 1000LL * 1000LL;
static const int64_t DELAYED_MACHINE_SEND_US = 800LL * 1000LL;
static const int64_t HEAT_REFRESH_INTERVAL_US = 5LL * 1000LL * 1000LL;
static const int64_t HEAT_REFRESH_INITIAL_DELAY_US = 2LL * 1000LL * 1000LL;
static const int64_t HEAT_REFRESH_TIMEOUT_US = 120LL * 1000LL * 1000LL;

static void reset_heat_state(lm_ctrl_runtime_heat_state_t *heat_state) {
  if (heat_state == NULL) {
    return;
  }

  *heat_state = (lm_ctrl_runtime_heat_state_t){
    .last_rendered_remaining_s = -1,
    .last_rendered_progress_permille = -1,
  };
}

static void reset_shot_timer_state(lm_ctrl_runtime_shot_timer_state_t *shot_timer_state) {
  lm_ctrl_shot_timer_reset(shot_timer_state);
}

static void clear_heat_refresh(lm_ctrl_runtime_heat_refresh_t *heat_refresh) {
  if (heat_refresh == NULL) {
    return;
  }

  heat_refresh->until_us = 0;
  heat_refresh->next_request_us = 0;
}

static void arm_heat_refresh(
  lm_ctrl_runtime_heat_refresh_t *heat_refresh,
  int64_t initial_delay_us
) {
  const int64_t now_us = esp_timer_get_time();

  if (heat_refresh == NULL) {
    return;
  }

  heat_refresh->until_us = now_us + HEAT_REFRESH_TIMEOUT_US;
  heat_refresh->next_request_us = now_us + initial_delay_us;
}

static bool should_show_heat_display(void) {
  lm_ctrl_wifi_info_t wifi_info = {0};

  lm_ctrl_wifi_get_info(&wifi_info);
  return wifi_info.heat_display_enabled;
}

static int64_t current_epoch_ms(void) {
  struct timeval tv = {0};

  if (gettimeofday(&tv, NULL) != 0) {
    return 0;
  }
  if (tv.tv_sec < 1700000000) {
    return 0;
  }

  return ((int64_t)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}

static int64_t heat_state_remaining_us(const lm_ctrl_runtime_heat_state_t *heat_state) {
  const int64_t now_us = esp_timer_get_time();

  if (heat_state == NULL || !heat_state->heating || heat_state->deadline_local_us <= 0) {
    return 0;
  }
  if (heat_state->deadline_local_us <= now_us) {
    return 0;
  }

  return heat_state->deadline_local_us - now_us;
}

static int32_t heat_state_remaining_seconds(const lm_ctrl_runtime_heat_state_t *heat_state) {
  int64_t remaining_us = heat_state_remaining_us(heat_state);
  return remaining_us > 0 ? (int32_t)((remaining_us + 999999LL) / 1000000LL) : 0;
}

static uint16_t heat_state_progress_permille(const lm_ctrl_runtime_heat_state_t *heat_state) {
  int64_t remaining_us;

  if (heat_state == NULL || !heat_state->heating || heat_state->duration_us <= 0) {
    return 0;
  }

  remaining_us = heat_state_remaining_us(heat_state);
  if (remaining_us <= 0) {
    return 0;
  }
  if (remaining_us >= heat_state->duration_us) {
    return 1000;
  }

  return (uint16_t)((remaining_us * 1000LL) / heat_state->duration_us);
}

static void maybe_refresh_shot_timer(lm_ctrl_runtime_t *runtime, bool *needs_render) {
  lm_ctrl_shot_timer_info_t shot_timer = {0};

  if (runtime == NULL) {
    return;
  }

  if (lm_ctrl_wifi_get_shot_timer_info(&shot_timer) &&
      lm_ctrl_shot_timer_update(&runtime->shot_timer_state, &shot_timer)) {
    if (needs_render != NULL) {
      *needs_render = true;
    }
  }
}

static bool initialize_heat_state_deadline(
  lm_ctrl_runtime_heat_state_t *heat_state,
  int64_t observed_epoch_ms,
  int64_t ready_epoch_ms
) {
  int64_t remaining_us = 0;
  int64_t now_epoch_ms = 0;
  int64_t now_us = 0;

  if (heat_state == NULL || ready_epoch_ms <= 0) {
    return false;
  }
  if (heat_state->deadline_local_us > 0) {
    return true;
  }

  if (observed_epoch_ms > 0 && ready_epoch_ms > observed_epoch_ms) {
    remaining_us = (ready_epoch_ms - observed_epoch_ms) * 1000LL;
  } else {
    now_epoch_ms = current_epoch_ms();
    if (now_epoch_ms > 0 && ready_epoch_ms > now_epoch_ms) {
      remaining_us = (ready_epoch_ms - now_epoch_ms) * 1000LL;
    }
  }
  if (remaining_us <= 0) {
    return false;
  }

  now_us = esp_timer_get_time();
  heat_state->deadline_local_us = now_us + remaining_us;
  heat_state->duration_us = remaining_us;
  heat_state->last_rendered_remaining_s = -1;
  heat_state->last_rendered_progress_permille = -1;
  return true;
}

static void sync_heat_state(lm_ctrl_runtime_t *runtime) {
  lm_ctrl_machine_heat_info_t heat_info = {0};
  bool have_machine_eta = false;

  if (runtime == NULL) {
    return;
  }
  if (!should_show_heat_display()) {
    reset_heat_state(&runtime->heat_state);
    clear_heat_refresh(&runtime->heat_refresh);
    return;
  }

  lm_ctrl_machine_link_get_heat_info(&heat_info);

  if (!heat_info.available || !heat_info.heating) {
    reset_heat_state(&runtime->heat_state);
    clear_heat_refresh(&runtime->heat_refresh);
    return;
  }

  runtime->heat_state.heating = heat_info.heating;
  if (heat_info.eta_available && heat_info.ready_epoch_ms > 0) {
    have_machine_eta = initialize_heat_state_deadline(
      &runtime->heat_state,
      heat_info.observed_epoch_ms,
      heat_info.ready_epoch_ms
    );
  }
  if (have_machine_eta) {
    clear_heat_refresh(&runtime->heat_refresh);
  } else if (runtime->heat_refresh.until_us == 0) {
    arm_heat_refresh(&runtime->heat_refresh, 0);
  }
}

static bool approx_equal(float a, float b) {
  float delta = a - b;
  if (delta < 0.0f) {
    delta = -delta;
  }
  return delta < 0.05f;
}

static bool should_defer_machine_send(uint32_t field_mask) {
  const uint32_t deferred_mask =
    LM_CTRL_MACHINE_FIELD_TEMPERATURE |
    LM_CTRL_MACHINE_FIELD_INFUSE |
    LM_CTRL_MACHINE_FIELD_PAUSE |
    LM_CTRL_MACHINE_FIELD_BBW_DOSE_1 |
    LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;

  return field_mask != LM_CTRL_MACHINE_FIELD_NONE &&
         (field_mask & deferred_mask) != 0 &&
         (field_mask & ~deferred_mask) == 0;
}

static bool has_local_ble_binding(lm_ctrl_machine_binding_t *binding) {
  lm_ctrl_machine_binding_t local_binding = {0};

  if (binding == NULL) {
    binding = &local_binding;
  }

  return lm_ctrl_wifi_get_machine_binding(binding) &&
         binding->configured &&
         binding->serial[0] != '\0' &&
         binding->communication_key[0] != '\0';
}

static void note_local_value_hold(
  lm_ctrl_runtime_local_value_hold_t *hold,
  const ctrl_values_t *values,
  uint32_t field_mask
) {
  if (hold == NULL || values == NULL || field_mask == LM_CTRL_MACHINE_FIELD_NONE) {
    return;
  }

  hold->values = *values;
  hold->mask |= field_mask;
  hold->expires_us = esp_timer_get_time() + LOCAL_VALUE_HOLD_US;
}

static void arm_delayed_machine_send(
  lm_ctrl_runtime_delayed_machine_send_t *delayed_send,
  uint32_t field_mask
) {
  if (delayed_send == NULL || field_mask == LM_CTRL_MACHINE_FIELD_NONE) {
    return;
  }

  delayed_send->mask |= field_mask;
  delayed_send->due_us = esp_timer_get_time() + DELAYED_MACHINE_SEND_US;
}

static void clear_delayed_machine_send_mask(
  lm_ctrl_runtime_delayed_machine_send_t *delayed_send,
  uint32_t field_mask
) {
  if (delayed_send == NULL || field_mask == LM_CTRL_MACHINE_FIELD_NONE) {
    return;
  }

  delayed_send->mask &= ~field_mask;
  if (delayed_send->mask == 0) {
    delayed_send->due_us = 0;
  }
}

static bool should_keep_local_float(
  float current_network,
  float desired_local,
  uint32_t field_mask,
  const lm_ctrl_runtime_local_value_hold_t *hold,
  int64_t now_us
) {
  if (hold == NULL || (hold->mask & field_mask) == 0 || now_us >= hold->expires_us) {
    return false;
  }
  return !approx_equal(current_network, desired_local);
}

static bool should_keep_local_bool(
  bool current_network,
  bool desired_local,
  uint32_t field_mask,
  const lm_ctrl_runtime_local_value_hold_t *hold,
  int64_t now_us
) {
  if (hold == NULL || (hold->mask & field_mask) == 0 || now_us >= hold->expires_us) {
    return false;
  }
  return current_network != desired_local;
}

static bool should_keep_local_int(
  int current_network,
  int desired_local,
  uint32_t field_mask,
  const lm_ctrl_runtime_local_value_hold_t *hold,
  int64_t now_us
) {
  if (hold == NULL || (hold->mask & field_mask) == 0 || now_us >= hold->expires_us) {
    return false;
  }
  return current_network != desired_local;
}

static void reconcile_local_value_hold(
  lm_ctrl_runtime_local_value_hold_t *hold,
  const ctrl_values_t *values,
  uint32_t loaded_mask
) {
  if (hold == NULL || hold->mask == 0 || values == NULL || loaded_mask == 0) {
    return;
  }

  if ((hold->mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      (loaded_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      approx_equal(values->temperature_c, hold->values.temperature_c)) {
    hold->mask &= ~LM_CTRL_MACHINE_FIELD_TEMPERATURE;
  }
  if ((hold->mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      (loaded_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      approx_equal(values->infuse_s, hold->values.infuse_s)) {
    hold->mask &= ~LM_CTRL_MACHINE_FIELD_INFUSE;
  }
  if ((hold->mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      (loaded_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      approx_equal(values->pause_s, hold->values.pause_s)) {
    hold->mask &= ~LM_CTRL_MACHINE_FIELD_PAUSE;
  }
  if ((hold->mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      (loaded_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      values->steam_level == hold->values.steam_level) {
    hold->mask &= ~LM_CTRL_MACHINE_FIELD_STEAM;
  }
  if ((hold->mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      (loaded_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      values->standby_on == hold->values.standby_on) {
    hold->mask &= ~LM_CTRL_MACHINE_FIELD_STANDBY;
  }
  if ((hold->mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      (loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      values->bbw_mode == hold->values.bbw_mode) {
    hold->mask &= ~LM_CTRL_MACHINE_FIELD_BBW_MODE;
  }
  if ((hold->mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      (loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      approx_equal(values->bbw_dose_1_g, hold->values.bbw_dose_1_g)) {
    hold->mask &= ~LM_CTRL_MACHINE_FIELD_BBW_DOSE_1;
  }
  if ((hold->mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      (loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      approx_equal(values->bbw_dose_2_g, hold->values.bbw_dose_2_g)) {
    hold->mask &= ~LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
  }
}

static void merge_loaded_values(
  ctrl_state_t *state,
  lm_ctrl_runtime_local_value_hold_t *hold
) {
  ctrl_values_t values;
  uint32_t loaded_mask = 0;
  uint32_t feature_mask = 0;
  const int64_t now_us = esp_timer_get_time();

  if (state == NULL || !lm_ctrl_machine_link_get_values(&values, &loaded_mask, &feature_mask)) {
    return;
  }

  reconcile_local_value_hold(hold, &values, loaded_mask);

  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      !should_keep_local_float(values.temperature_c, state->values.temperature_c, LM_CTRL_MACHINE_FIELD_TEMPERATURE, hold, now_us)) {
    state->values.temperature_c = values.temperature_c;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      !should_keep_local_float(values.infuse_s, state->values.infuse_s, LM_CTRL_MACHINE_FIELD_INFUSE, hold, now_us)) {
    state->values.infuse_s = values.infuse_s;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      !should_keep_local_float(values.pause_s, state->values.pause_s, LM_CTRL_MACHINE_FIELD_PAUSE, hold, now_us)) {
    state->values.pause_s = values.pause_s;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      !should_keep_local_int((int)values.steam_level, (int)state->values.steam_level, LM_CTRL_MACHINE_FIELD_STEAM, hold, now_us)) {
    state->values.steam_level = values.steam_level;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      !should_keep_local_bool(values.standby_on, state->values.standby_on, LM_CTRL_MACHINE_FIELD_STANDBY, hold, now_us)) {
    state->values.standby_on = values.standby_on;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      !should_keep_local_int((int)values.bbw_mode, (int)state->values.bbw_mode, LM_CTRL_MACHINE_FIELD_BBW_MODE, hold, now_us)) {
    state->values.bbw_mode = values.bbw_mode;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      !should_keep_local_float(values.bbw_dose_1_g, state->values.bbw_dose_1_g, LM_CTRL_MACHINE_FIELD_BBW_DOSE_1, hold, now_us)) {
    state->values.bbw_dose_1_g = values.bbw_dose_1_g;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      !should_keep_local_float(values.bbw_dose_2_g, state->values.bbw_dose_2_g, LM_CTRL_MACHINE_FIELD_BBW_DOSE_2, hold, now_us)) {
    state->values.bbw_dose_2_g = values.bbw_dose_2_g;
  }
  state->feature_mask = feature_mask;
  state->loaded_mask |= loaded_mask;
  if ((state->feature_mask & CTRL_FEATURE_BBW) == 0 && state->focus >= CTRL_FOCUS_BBW_MODE) {
    state->focus = CTRL_FOCUS_TEMPERATURE;
  }
}

static void maybe_flush_delayed_machine_send(
  const ctrl_state_t *state,
  lm_ctrl_runtime_delayed_machine_send_t *delayed_send,
  lm_ctrl_runtime_local_value_hold_t *hold
) {
  uint32_t field_mask;
  const int64_t now_us = esp_timer_get_time();

  if (state == NULL || delayed_send == NULL || delayed_send->mask == 0 || delayed_send->due_us == 0) {
    return;
  }
  if (now_us < delayed_send->due_us) {
    return;
  }

  field_mask = delayed_send->mask;
  if (lm_ctrl_machine_link_queue_values(&state->values, field_mask) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to queue delayed machine update fields=0x%02x", (unsigned)field_mask);
    delayed_send->due_us = now_us + (250LL * 1000LL);
    return;
  }

  note_local_value_hold(hold, &state->values, field_mask);
  delayed_send->mask = 0;
  delayed_send->due_us = 0;
}

static void log_state(const ctrl_state_t *state) {
  ESP_LOGI(
    TAG,
    "screen=%s focus=%s temp=%.1f inf=%.1f pause=%.1f steam=%d standby=%d preset=%u/%u steps=%.1f/%.1f",
    ctrl_screen_name(state->screen),
    ctrl_focus_name(state->focus),
    state->values.temperature_c,
    state->values.infuse_s,
    state->values.pause_s,
    (int)state->values.steam_level,
    state->values.standby_on,
    (unsigned)state->preset_index + 1U,
    (unsigned)state->preset_count,
    (double)state->temperature_step_c,
    (double)state->time_step_s
  );
}

static void set_status_from_action(const ctrl_state_t *state, ctrl_action_t action, char *status, size_t status_size) {
  lm_ctrl_wifi_info_t wifi_info = {0};
  ctrl_language_t language = CTRL_LANGUAGE_EN;

  if (state != NULL && state->screen == CTRL_SCREEN_SETUP) {
    lm_ctrl_wifi_format_status(status, status_size);
    return;
  }

  lm_ctrl_wifi_get_info(&wifi_info);
  language = wifi_info.language;

  switch (action.type) {
    case CTRL_ACTION_APPLY_FIELD:
      snprintf(
        status,
        status_size,
        language == CTRL_LANGUAGE_DE ? "%s aktualisiert." : "%s updated.",
        ctrl_focus_name_for_language(action.applied_focus, language)
      );
      break;
    case CTRL_ACTION_LOAD_PRESET:
      snprintf(
        status,
        status_size,
        language == CTRL_LANGUAGE_DE ? "Preset %d aktiviert." : "Preset %d loaded.",
        action.preset_slot + 1
      );
      break;
    case CTRL_ACTION_SAVE_PRESET:
      snprintf(
        status,
        status_size,
        language == CTRL_LANGUAGE_DE ? "Preset %d aus aktuellen Werten gesichert." : "Preset %d saved from the current values.",
        action.preset_slot + 1
      );
      break;
    case CTRL_ACTION_OPEN_SETUP:
      snprintf(
        status,
        status_size,
        "%s",
        language == CTRL_LANGUAGE_DE ? "Controller-Setup wird geladen." : "Controller setup is loading."
      );
      break;
    case CTRL_ACTION_CLEAR_WEB_PASSWORD:
      snprintf(
        status,
        status_size,
        "%s",
        language == CTRL_LANGUAGE_DE ? "Web-Passwort wird gelöscht." : "Web password is being cleared."
      );
      break;
    case CTRL_ACTION_RESET_NETWORK:
      snprintf(
        status,
        status_size,
        "%s",
        language == CTRL_LANGUAGE_DE ? "Netzwerk-Reset wird ausgeführt." : "Network reset in progress."
      );
      break;
    case CTRL_ACTION_NONE:
    default:
      status[0] = '\0';
      break;
  }
}

static lm_ctrl_led_status_t led_status_from_connectivity(
  const lm_ctrl_wifi_info_t *wifi_info,
  const lm_ctrl_machine_link_info_t *machine_info
) {
  if (wifi_info == NULL || machine_info == NULL) {
    return LM_CTRL_LED_STATUS_IDLE;
  }

  if (wifi_info->portal_running && !wifi_info->sta_connected) {
    return LM_CTRL_LED_STATUS_SETUP;
  }
  if (wifi_info->sta_connecting ||
      machine_info->pending_work ||
      (machine_info->connected && !machine_info->authenticated)) {
    return LM_CTRL_LED_STATUS_CONNECTING;
  }
  if (wifi_info->cloud_connected || machine_info->authenticated || wifi_info->sta_connected) {
    return LM_CTRL_LED_STATUS_CONNECTED;
  }
  return LM_CTRL_LED_STATUS_IDLE;
}

static void sync_led_status_from_connectivity(void) {
  lm_ctrl_wifi_info_t wifi_info;
  lm_ctrl_machine_link_info_t machine_info = {0};

  lm_ctrl_wifi_get_info(&wifi_info);
  lm_ctrl_machine_link_get_info(&machine_info);
  (void)lm_ctrl_leds_set_status(led_status_from_connectivity(&wifi_info, &machine_info));
}

static void maybe_request_cloud_probe(int64_t *last_request_us) {
  lm_ctrl_wifi_info_t wifi_info;
  const int64_t now_us = esp_timer_get_time();
  esp_err_t probe_ret = ESP_ERR_INVALID_STATE;
  esp_err_t live_ret = ESP_ERR_INVALID_STATE;

  if (last_request_us == NULL) {
    return;
  }

  lm_ctrl_wifi_get_info(&wifi_info);
  if (!wifi_info.sta_connected || !wifi_info.has_cloud_credentials) {
    *last_request_us = 0;
    return;
  }

  if (*last_request_us != 0 && (now_us - *last_request_us) < CLOUD_PROBE_INTERVAL_US) {
    return;
  }

  probe_ret = lm_ctrl_wifi_request_cloud_probe();
  live_ret = lm_ctrl_wifi_request_live_updates();
  if (probe_ret == ESP_OK || live_ret == ESP_OK) {
    *last_request_us = now_us;
  }
}

static void maybe_request_value_sync(const ctrl_state_t *state) {
  lm_ctrl_wifi_info_t wifi_info;
  lm_ctrl_machine_link_info_t machine_info = {0};
  lm_ctrl_machine_binding_t binding = {0};
  uint32_t wanted_mask;
  uint32_t sync_flags = LM_CTRL_MACHINE_SYNC_NONE;
  bool local_ble_available = false;

  if (state == NULL) {
    return;
  }

  wanted_mask = LM_CTRL_MACHINE_FIELD_TEMPERATURE |
                LM_CTRL_MACHINE_FIELD_STEAM |
                LM_CTRL_MACHINE_FIELD_STANDBY |
                LM_CTRL_MACHINE_FIELD_PREBREWING;
  if ((state->feature_mask & CTRL_FEATURE_BBW) != 0) {
    wanted_mask |= LM_CTRL_MACHINE_FIELD_BBW;
  }
  if ((state->loaded_mask & wanted_mask) == wanted_mask) {
    return;
  }

  lm_ctrl_wifi_get_info(&wifi_info);
  lm_ctrl_machine_link_get_info(&machine_info);
  local_ble_available = has_local_ble_binding(&binding);
  if (machine_info.sync_pending) {
    return;
  }

  if (local_ble_available || machine_info.authenticated) {
    sync_flags |= LM_CTRL_MACHINE_SYNC_BLE;
  }
  if (wifi_info.cloud_connected && wifi_info.has_machine_selection) {
    sync_flags |= LM_CTRL_MACHINE_SYNC_CLOUD;
  }
  if (sync_flags == LM_CTRL_MACHINE_SYNC_NONE) {
    return;
  }

  if (lm_ctrl_machine_link_request_sync_mode(sync_flags) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to request machine value sync");
  }
}

static void maybe_request_periodic_value_refresh(int64_t *last_ble_request_us, int64_t *last_cloud_request_us) {
  lm_ctrl_wifi_info_t wifi_info;
  lm_ctrl_machine_link_info_t machine_info = {0};
  lm_ctrl_machine_binding_t binding = {0};
  const int64_t now_us = esp_timer_get_time();
  bool local_ble_available;
  bool can_refresh_ble;
  bool can_refresh_cloud;

  if (last_ble_request_us == NULL || last_cloud_request_us == NULL) {
    return;
  }

  lm_ctrl_wifi_get_info(&wifi_info);
  lm_ctrl_machine_link_get_info(&machine_info);
  local_ble_available = has_local_ble_binding(&binding);
  can_refresh_ble = lm_ctrl_machine_should_request_periodic_ble_sync(
    local_ble_available,
    machine_info.authenticated
  );
  can_refresh_cloud = wifi_info.cloud_connected && wifi_info.has_machine_selection;

  if (!can_refresh_ble) {
    *last_ble_request_us = 0;
  } else if (*last_ble_request_us == 0 || (now_us - *last_ble_request_us) >= BLE_VALUE_REFRESH_INTERVAL_US) {
    if (lm_ctrl_machine_link_request_sync_mode(LM_CTRL_MACHINE_SYNC_BLE) == ESP_OK) {
      *last_ble_request_us = now_us;
    } else {
      ESP_LOGW(TAG, "Failed to request periodic BLE refresh");
    }
  }

  if (!can_refresh_cloud) {
    *last_cloud_request_us = 0;
  } else if (*last_cloud_request_us == 0 || (now_us - *last_cloud_request_us) >= CLOUD_VALUE_REFRESH_INTERVAL_US) {
    if (lm_ctrl_machine_link_request_sync_mode(LM_CTRL_MACHINE_SYNC_CLOUD) == ESP_OK) {
      *last_cloud_request_us = now_us;
    } else {
      ESP_LOGW(TAG, "Failed to request periodic cloud refresh");
    }
  }
}

static void maybe_request_fast_heat_refresh(lm_ctrl_runtime_t *runtime) {
  lm_ctrl_wifi_info_t wifi_info;
  lm_ctrl_machine_link_info_t machine_info = {0};
  const int64_t now_us = esp_timer_get_time();

  if (runtime == NULL) {
    return;
  }
  lm_ctrl_wifi_get_info(&wifi_info);
  if (!wifi_info.heat_display_enabled) {
    reset_heat_state(&runtime->heat_state);
    clear_heat_refresh(&runtime->heat_refresh);
    return;
  }
  if (runtime->state.values.standby_on || runtime->heat_state.deadline_local_us > 0) {
    clear_heat_refresh(&runtime->heat_refresh);
    return;
  }
  if (runtime->heat_refresh.until_us == 0) {
    return;
  }
  if (now_us >= runtime->heat_refresh.until_us) {
    clear_heat_refresh(&runtime->heat_refresh);
    return;
  }
  if (runtime->heat_refresh.next_request_us != 0 && now_us < runtime->heat_refresh.next_request_us) {
    return;
  }

  lm_ctrl_machine_link_get_info(&machine_info);
  if (!wifi_info.cloud_connected || !wifi_info.has_machine_selection || machine_info.sync_pending) {
    return;
  }

  if (lm_ctrl_machine_link_request_sync_mode(LM_CTRL_MACHINE_SYNC_CLOUD) == ESP_OK) {
    runtime->heat_refresh.next_request_us = now_us + HEAT_REFRESH_INTERVAL_US;
  }
}

void lm_ctrl_runtime_init(lm_ctrl_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  memset(runtime, 0, sizeof(*runtime));
  reset_heat_state(&runtime->heat_state);
  reset_shot_timer_state(&runtime->shot_timer_state);
  ctrl_state_init(&runtime->state);
  if (ctrl_state_load(&runtime->state) != ESP_OK) {
    ESP_LOGW(TAG, "Falling back to default controller values");
  }
  set_status_from_action(&runtime->state, (ctrl_action_t){.type = CTRL_ACTION_NONE}, runtime->status, sizeof(runtime->status));
  log_state(&runtime->state);
}

void lm_ctrl_runtime_bootstrap(lm_ctrl_runtime_t *runtime) {
  lm_ctrl_wifi_info_t wifi_info;

  if (runtime == NULL) {
    return;
  }

  lm_ctrl_wifi_get_info(&wifi_info);
  if (!wifi_info.has_credentials) {
    ctrl_open_setup(&runtime->state);
    if (!wifi_info.portal_running) {
      (void)lm_ctrl_wifi_start_portal();
    }
    lm_ctrl_wifi_format_status(runtime->status, sizeof(runtime->status));
  }

  runtime->last_wifi_status_version = lm_ctrl_wifi_status_version();
  runtime->last_power_status_version = lm_ctrl_power_status_version();
  runtime->last_machine_status_version = lm_ctrl_machine_link_status_version();
  runtime->last_preset_version = ctrl_state_preset_version();
  maybe_request_cloud_probe(&runtime->last_cloud_probe_request_us);
  maybe_request_value_sync(&runtime->state);
  maybe_request_periodic_value_refresh(&runtime->last_ble_refresh_request_us, &runtime->last_cloud_refresh_request_us);
  sync_heat_state(runtime);
  sync_led_status_from_connectivity();
}

void lm_ctrl_runtime_handle_input_event(
  lm_ctrl_runtime_t *runtime,
  const lm_ctrl_input_event_t *event,
  bool *needs_render
) {
  ctrl_action_t action = {
    .type = CTRL_ACTION_NONE,
    .applied_focus = CTRL_FOCUS_TEMPERATURE,
    .preset_slot = -1,
  };
  lm_ctrl_wifi_info_t wifi_info = {0};
  lm_ctrl_machine_link_info_t machine_info = {0};
  lm_ctrl_controller_access_t access = {0};
  bool should_persist_state = false;
  bool preserve_status = false;

  if (runtime == NULL || event == NULL) {
    return;
  }

  action.applied_focus = runtime->state.focus;
  lm_ctrl_wifi_get_info(&wifi_info);
  lm_ctrl_machine_link_get_info(&machine_info);
  lm_ctrl_controller_compute_access(&wifi_info, &machine_info, runtime->state.feature_mask, &access);

  switch (event->type) {
    case LM_CTRL_EVENT_ROTATE:
      if (runtime->state.screen == CTRL_SCREEN_MAIN &&
          !lm_ctrl_controller_field_is_editable(access.editable_mask, runtime->state.focus)) {
        break;
      }
      ctrl_rotate(&runtime->state, event->delta_steps);
      {
        const uint32_t field_mask = lm_ctrl_machine_field_for_focus(runtime->state.focus);
        if (field_mask != LM_CTRL_MACHINE_FIELD_NONE && should_defer_machine_send(field_mask)) {
          note_local_value_hold(&runtime->local_value_hold, &runtime->state.values, field_mask);
          arm_delayed_machine_send(&runtime->delayed_machine_send, field_mask);
        } else if (lm_ctrl_machine_link_queue_values(&runtime->state.values, field_mask) != ESP_OK &&
                   field_mask != LM_CTRL_MACHINE_FIELD_NONE) {
          ESP_LOGW(TAG, "Failed to queue BLE update for focus=%s", ctrl_focus_name(runtime->state.focus));
        } else if (field_mask != LM_CTRL_MACHINE_FIELD_NONE) {
          note_local_value_hold(&runtime->local_value_hold, &runtime->state.values, field_mask);
          clear_delayed_machine_send_mask(&runtime->delayed_machine_send, field_mask);
        }
      }
      should_persist_state = runtime->state.screen == CTRL_SCREEN_MAIN && (
        runtime->state.focus == CTRL_FOCUS_TEMPERATURE ||
        runtime->state.focus == CTRL_FOCUS_INFUSE ||
        runtime->state.focus == CTRL_FOCUS_PAUSE ||
        runtime->state.focus == CTRL_FOCUS_BBW_MODE ||
        runtime->state.focus == CTRL_FOCUS_BBW_DOSE_1 ||
        runtime->state.focus == CTRL_FOCUS_BBW_DOSE_2
      );
      (void)lm_ctrl_leds_indicate_rotation(event->delta_steps);
      (void)lm_ctrl_haptic_click();
      break;
    case LM_CTRL_EVENT_SELECT_FOCUS:
      ctrl_set_focus(&runtime->state, event->focus);
      (void)lm_ctrl_haptic_click();
      break;
    case LM_CTRL_EVENT_TOGGLE_FOCUS:
      if (runtime->state.screen == CTRL_SCREEN_MAIN &&
          !lm_ctrl_controller_field_is_editable(access.editable_mask, event->focus)) {
        break;
      }
      ctrl_toggle_focus(&runtime->state, event->focus);
      {
        const bool waking_from_standby =
          event->focus == CTRL_FOCUS_STANDBY &&
          runtime->state.values.standby_on == false;
        const uint32_t field_mask = lm_ctrl_machine_field_for_focus(event->focus);
        if (lm_ctrl_machine_link_queue_values(&runtime->state.values, field_mask) != ESP_OK &&
            field_mask != LM_CTRL_MACHINE_FIELD_NONE) {
          ESP_LOGW(TAG, "Failed to queue BLE toggle for focus=%s", ctrl_focus_name(event->focus));
        } else if (field_mask != LM_CTRL_MACHINE_FIELD_NONE) {
          note_local_value_hold(&runtime->local_value_hold, &runtime->state.values, field_mask);
          clear_delayed_machine_send_mask(&runtime->delayed_machine_send, field_mask);
        }
        if (event->focus == CTRL_FOCUS_STANDBY && runtime->state.values.standby_on) {
          reset_heat_state(&runtime->heat_state);
          clear_heat_refresh(&runtime->heat_refresh);
        } else if (waking_from_standby) {
          reset_heat_state(&runtime->heat_state);
          arm_heat_refresh(&runtime->heat_refresh, HEAT_REFRESH_INITIAL_DELAY_US);
        }
      }
      (void)lm_ctrl_haptic_click();
      break;
    case LM_CTRL_EVENT_OPEN_PRESETS:
      ctrl_open_presets(&runtime->state);
      (void)lm_ctrl_haptic_click();
      break;
    case LM_CTRL_EVENT_LOAD_PRESET:
      if (!access.preset_load_enabled) {
        break;
      }
      action = ctrl_load_preset(&runtime->state);
      if (action.type == CTRL_ACTION_LOAD_PRESET) {
        const uint32_t field_mask = lm_ctrl_machine_preset_field_mask(runtime->state.feature_mask);
        if (lm_ctrl_machine_link_queue_values(&runtime->state.values, field_mask) != ESP_OK) {
          ESP_LOGW(TAG, "Failed to queue BLE preset sync");
        } else {
          note_local_value_hold(&runtime->local_value_hold, &runtime->state.values, field_mask);
          clear_delayed_machine_send_mask(&runtime->delayed_machine_send, field_mask);
        }
      }
      should_persist_state = action.type == CTRL_ACTION_LOAD_PRESET;
      (void)lm_ctrl_haptic_click();
      break;
    case LM_CTRL_EVENT_SAVE_PRESET:
      action = ctrl_save_preset(&runtime->state);
      should_persist_state = action.type == CTRL_ACTION_SAVE_PRESET;
      (void)lm_ctrl_haptic_click();
      break;
    case LM_CTRL_EVENT_OPEN_SETUP:
      ctrl_open_setup(&runtime->state);
      if (lm_ctrl_wifi_start_portal() != ESP_OK) {
        snprintf(runtime->status, sizeof(runtime->status), "Could not start the setup portal.");
        preserve_status = true;
      }
      sync_led_status_from_connectivity();
      (void)lm_ctrl_haptic_click();
      break;
    case LM_CTRL_EVENT_OPEN_SETUP_RESET:
      ctrl_open_setup_reset(&runtime->state);
      (void)lm_ctrl_haptic_click();
      break;
    case LM_CTRL_EVENT_CANCEL_SETUP_RESET:
      ctrl_cancel_setup_reset(&runtime->state);
      (void)lm_ctrl_haptic_click();
      break;
    case LM_CTRL_EVENT_CONFIRM_SETUP_RESET:
      action = ctrl_confirm_setup_reset(&runtime->state);
      if (action.type == CTRL_ACTION_CLEAR_WEB_PASSWORD) {
        if (lm_ctrl_wifi_clear_web_admin_password() != ESP_OK) {
          snprintf(runtime->status, sizeof(runtime->status), "Could not clear the web password.");
          action.type = CTRL_ACTION_NONE;
          preserve_status = true;
        }
      } else if (action.type == CTRL_ACTION_RESET_NETWORK && lm_ctrl_wifi_reset_network() != ESP_OK) {
        snprintf(runtime->status, sizeof(runtime->status), "Could not reset network settings.");
        action.type = CTRL_ACTION_NONE;
        preserve_status = true;
      }
      (void)lm_ctrl_haptic_click();
      break;
    case LM_CTRL_EVENT_DISMISS_SHOT_TIMER:
      if (lm_ctrl_shot_timer_dismiss(&runtime->shot_timer_state)) {
        (void)lm_ctrl_haptic_click();
      }
      break;
    case LM_CTRL_EVENT_CLOSE_SCREEN:
      ctrl_close_overlay(&runtime->state);
      (void)lm_ctrl_haptic_click();
      break;
    default:
      break;
  }

  if (should_persist_state && ctrl_state_persist(&runtime->state) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to persist controller state");
  }

  if (!preserve_status) {
    set_status_from_action(&runtime->state, action, runtime->status, sizeof(runtime->status));
  }
  log_state(&runtime->state);
  if (needs_render != NULL) {
    *needs_render = true;
  }
}

void lm_ctrl_runtime_handle_wifi_status_change(lm_ctrl_runtime_t *runtime, bool *needs_render) {
  uint32_t wifi_status_version;

  if (runtime == NULL) {
    return;
  }

  wifi_status_version = lm_ctrl_wifi_status_version();
  if (wifi_status_version == runtime->last_wifi_status_version) {
    return;
  }

  runtime->last_wifi_status_version = wifi_status_version;
  sync_led_status_from_connectivity();
  maybe_request_cloud_probe(&runtime->last_cloud_probe_request_us);
  maybe_request_value_sync(&runtime->state);
  maybe_request_periodic_value_refresh(&runtime->last_ble_refresh_request_us, &runtime->last_cloud_refresh_request_us);
  sync_heat_state(runtime);
  if (runtime->state.screen == CTRL_SCREEN_SETUP) {
    lm_ctrl_wifi_format_status(runtime->status, sizeof(runtime->status));
  }
  if (needs_render != NULL) {
    *needs_render = true;
  }
}

void lm_ctrl_runtime_handle_power_status_change(lm_ctrl_runtime_t *runtime, bool *needs_render) {
  uint32_t power_status_version;

  if (runtime == NULL) {
    return;
  }

  power_status_version = lm_ctrl_power_status_version();
  if (power_status_version == runtime->last_power_status_version) {
    return;
  }

  runtime->last_power_status_version = power_status_version;
  if (needs_render != NULL) {
    *needs_render = true;
  }
}

void lm_ctrl_runtime_handle_machine_status_change(lm_ctrl_runtime_t *runtime, bool *needs_render) {
  uint32_t machine_status_version;

  if (runtime == NULL) {
    return;
  }

  machine_status_version = lm_ctrl_machine_link_status_version();
  if (machine_status_version == runtime->last_machine_status_version) {
    return;
  }

  runtime->last_machine_status_version = machine_status_version;
  sync_led_status_from_connectivity();
  merge_loaded_values(&runtime->state, &runtime->local_value_hold);
  sync_heat_state(runtime);
  maybe_request_value_sync(&runtime->state);
  maybe_request_periodic_value_refresh(&runtime->last_ble_refresh_request_us, &runtime->last_cloud_refresh_request_us);
  if (needs_render != NULL) {
    *needs_render = true;
  }
}

void lm_ctrl_runtime_handle_preset_change(lm_ctrl_runtime_t *runtime, bool *needs_render) {
  uint32_t preset_version;

  if (runtime == NULL) {
    return;
  }

  preset_version = ctrl_state_preset_version();
  if (preset_version == runtime->last_preset_version) {
    return;
  }

  runtime->last_preset_version = preset_version;
  if (ctrl_state_refresh_presets(&runtime->state) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to refresh preset definitions");
  } else if (needs_render != NULL) {
    *needs_render = true;
  }
}

void lm_ctrl_runtime_tick(lm_ctrl_runtime_t *runtime, bool *needs_render) {
  int32_t remaining_s;
  int32_t progress_permille;

  if (runtime == NULL) {
    return;
  }

  maybe_request_cloud_probe(&runtime->last_cloud_probe_request_us);
  maybe_request_value_sync(&runtime->state);
  maybe_request_periodic_value_refresh(&runtime->last_ble_refresh_request_us, &runtime->last_cloud_refresh_request_us);
  maybe_request_fast_heat_refresh(runtime);
  maybe_flush_delayed_machine_send(&runtime->state, &runtime->delayed_machine_send, &runtime->local_value_hold);
  remaining_s = heat_state_remaining_seconds(&runtime->heat_state);
  progress_permille = (int32_t)heat_state_progress_permille(&runtime->heat_state);
  if (runtime->heat_state.heating &&
      runtime->heat_state.deadline_local_us > 0 &&
      (remaining_s != runtime->heat_state.last_rendered_remaining_s ||
       progress_permille != runtime->heat_state.last_rendered_progress_permille)) {
    runtime->heat_state.last_rendered_remaining_s = remaining_s;
    runtime->heat_state.last_rendered_progress_permille = progress_permille;
    if (needs_render != NULL) {
      *needs_render = true;
    }
  }
  maybe_refresh_shot_timer(runtime, needs_render);
}

const ctrl_state_t *lm_ctrl_runtime_state(const lm_ctrl_runtime_t *runtime) {
  return runtime != NULL ? &runtime->state : NULL;
}

const char *lm_ctrl_runtime_status(const lm_ctrl_runtime_t *runtime) {
  return runtime != NULL ? runtime->status : "";
}

void lm_ctrl_runtime_build_ui_view(const lm_ctrl_runtime_t *runtime, lm_ctrl_ui_view_t *view) {
  lm_ctrl_wifi_info_t wifi_info = {0};
  lm_ctrl_power_info_t power_info = {0};
  lm_ctrl_machine_link_info_t machine_info = {0};
  lm_ctrl_controller_access_t access = {0};

  if (runtime == NULL || view == NULL) {
    return;
  }

  memset(view, 0, sizeof(*view));
  lm_ctrl_wifi_get_info(&wifi_info);
  lm_ctrl_power_get_info(&power_info);
  lm_ctrl_machine_link_get_info(&machine_info);
  lm_ctrl_controller_compute_access(&wifi_info, &machine_info, runtime->state.feature_mask, &access);

  view->language = wifi_info.language;
  view->remote_path_state = access.remote_path_state;
  view->usb_visible = power_info.usb_connected;
  view->battery_visible = power_info.low || power_info.charging;
  view->battery_charging = power_info.charging;
  view->battery_low = power_info.low;
  view->heat_visible =
    wifi_info.heat_display_enabled &&
    runtime->heat_state.heating &&
    heat_state_remaining_us(&runtime->heat_state) > 0;
  view->heat_arc_visible =
    wifi_info.heat_display_enabled &&
    runtime->heat_state.duration_us > 0 &&
    view->heat_visible;
  view->water_alert_visible = machine_info.water_status.available && machine_info.water_status.no_water;
  view->ble_visible = machine_info.connected || machine_info.authenticated;
  view->ble_authenticated = machine_info.authenticated;
  view->readable_mask = access.readable_mask;
  view->editable_mask = access.editable_mask;
  view->preset_load_enabled = access.preset_load_enabled;
  view->heat_progress_permille = heat_state_progress_permille(&runtime->heat_state);
  view->custom_logo = wifi_info.has_custom_logo ? lm_ctrl_wifi_get_custom_logo() : NULL;
  view->shot_timer_visible = lm_ctrl_shot_timer_visible(&runtime->shot_timer_state);
  view->shot_timer_dismissable = lm_ctrl_shot_timer_dismissable(&runtime->shot_timer_state);
  if (view->shot_timer_visible) {
    snprintf(
      view->shot_timer_text,
      sizeof(view->shot_timer_text),
      "%u:%02u",
      (unsigned int)(runtime->shot_timer_state.seconds / 60U),
      (unsigned int)(runtime->shot_timer_state.seconds % 60U)
    );
  }
  if (view->heat_arc_visible) {
    int32_t remaining_s = heat_state_remaining_seconds(&runtime->heat_state);
    snprintf(
      view->heat_eta_text,
      sizeof(view->heat_eta_text),
      "%d:%02d",
      (int)(remaining_s / 60),
      (int)(remaining_s % 60)
    );
  }
  snprintf(view->setup_status_text, sizeof(view->setup_status_text), "%s", runtime->status);
  lm_ctrl_wifi_get_setup_qr_payload(view->setup_qr_payload, sizeof(view->setup_qr_payload));
}
