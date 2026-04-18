/**
 * Main firmware entrypoint and runtime loop for the round controller.
 *
 * This module wires together the board drivers, UI, input queue, Wi-Fi/cloud
 * services, and machine link worker. It owns the live controller state and is
 * responsible for scheduling background refreshes and UI updates.
 */
#include "controller_state.h"

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

#include "board_backlight.h"
#include "board_display.h"
#include "board_haptic.h"
#include "board_leds.h"
#include "controller_ui.h"
#include "input.h"
#include "machine_link.h"
#include "wifi_setup.h"

static const char *TAG = "lm_ctrl";
static const int64_t CLOUD_PROBE_INTERVAL_US = 60LL * 1000LL * 1000LL;
static const int64_t BLE_VALUE_REFRESH_INTERVAL_US = 15LL * 1000LL * 1000LL;
static const int64_t CLOUD_VALUE_REFRESH_INTERVAL_US = 60LL * 1000LL * 1000LL;
static const int64_t LOCAL_VALUE_HOLD_US = 20LL * 1000LL * 1000LL;
static const int64_t DELAYED_MACHINE_SEND_US = 800LL * 1000LL;

typedef struct {
  ctrl_values_t values;
  uint32_t mask;
  int64_t expires_us;
} local_value_hold_t;

typedef struct {
  uint32_t mask;
  int64_t due_us;
} delayed_machine_send_t;

static void note_local_value_hold(local_value_hold_t *hold, const ctrl_values_t *values, uint32_t field_mask);

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

static void arm_delayed_machine_send(delayed_machine_send_t *delayed_send, uint32_t field_mask) {
  if (delayed_send == NULL || field_mask == LM_CTRL_MACHINE_FIELD_NONE) {
    return;
  }

  delayed_send->mask |= field_mask;
  delayed_send->due_us = esp_timer_get_time() + DELAYED_MACHINE_SEND_US;
}

static void clear_delayed_machine_send_mask(delayed_machine_send_t *delayed_send, uint32_t field_mask) {
  if (delayed_send == NULL || field_mask == LM_CTRL_MACHINE_FIELD_NONE) {
    return;
  }

  delayed_send->mask &= ~field_mask;
  if (delayed_send->mask == 0) {
    delayed_send->due_us = 0;
  }
}

static void maybe_flush_delayed_machine_send(
  const ctrl_state_t *state,
  delayed_machine_send_t *delayed_send,
  local_value_hold_t *hold
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

static bool push_ui_event(QueueHandle_t queue, lm_ctrl_input_event_type_t type, ctrl_focus_t focus, int delta_steps) {
  const lm_ctrl_input_event_t event = {
    .type = type,
    .delta_steps = delta_steps,
    .focus = focus,
  };
  return xQueueSend(queue, &event, 0) == pdTRUE;
}

static void ui_action_cb(lm_ctrl_ui_action_t action, ctrl_focus_t focus, void *user_data) {
  QueueHandle_t queue = (QueueHandle_t)user_data;
  lm_ctrl_input_event_type_t event_type = LM_CTRL_EVENT_SELECT_FOCUS;
  int delta_steps = 0;

  switch (action) {
    case LM_CTRL_UI_ACTION_SELECT_FOCUS:
      event_type = LM_CTRL_EVENT_SELECT_FOCUS;
      break;
    case LM_CTRL_UI_ACTION_TOGGLE_FOCUS:
      event_type = LM_CTRL_EVENT_TOGGLE_FOCUS;
      break;
    case LM_CTRL_UI_ACTION_OPEN_PRESETS:
      event_type = LM_CTRL_EVENT_OPEN_PRESETS;
      break;
    case LM_CTRL_UI_ACTION_LOAD_PRESET:
      event_type = LM_CTRL_EVENT_LOAD_PRESET;
      break;
    case LM_CTRL_UI_ACTION_SAVE_PRESET:
      event_type = LM_CTRL_EVENT_SAVE_PRESET;
      break;
    case LM_CTRL_UI_ACTION_OPEN_SETUP:
      event_type = LM_CTRL_EVENT_OPEN_SETUP;
      break;
    case LM_CTRL_UI_ACTION_CLOSE_SCREEN:
      event_type = LM_CTRL_EVENT_CLOSE_SCREEN;
      break;
    case LM_CTRL_UI_ACTION_OPEN_SETUP_RESET:
      event_type = LM_CTRL_EVENT_OPEN_SETUP_RESET;
      break;
    case LM_CTRL_UI_ACTION_CANCEL_SETUP_RESET:
      event_type = LM_CTRL_EVENT_CANCEL_SETUP_RESET;
      break;
    case LM_CTRL_UI_ACTION_CONFIRM_SETUP_RESET:
      event_type = LM_CTRL_EVENT_CONFIRM_SETUP_RESET;
      break;
    default:
      return;
  }

  if (!push_ui_event(queue, event_type, focus, delta_steps)) {
    ESP_LOGW(TAG, "UI event queue full, dropping touch action=%d", (int)action);
  }
}

static void log_state(const ctrl_state_t *state) {
  ESP_LOGI(
    TAG,
    "screen=%s focus=%s temp=%.1f inf=%.1f pause=%.1f steam=%d standby=%d preset=%u",
    ctrl_screen_name(state->screen),
    ctrl_focus_name(state->focus),
    state->values.temperature_c,
    state->values.infuse_s,
    state->values.pause_s,
    (int)state->values.steam_level,
    state->values.standby_on,
    (unsigned)state->preset_index + 1U
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
    case CTRL_ACTION_RESET_NETWORK:
      snprintf(
        status,
        status_size,
        "%s",
        language == CTRL_LANGUAGE_DE ? "Netzwerk-Reset wird ausgefuehrt." : "Network reset in progress."
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
  if (
    wifi_info->sta_connecting ||
    machine_info->pending_work ||
    (machine_info->connected && !machine_info->authenticated)
  ) {
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

  if (lm_ctrl_wifi_request_cloud_probe() == ESP_OK) {
    *last_request_us = now_us;
  }
}

static bool should_keep_local_float(float current_network, float desired_local, uint32_t field_mask, const local_value_hold_t *hold, int64_t now_us) {
  if (hold == NULL || (hold->mask & field_mask) == 0) {
    return false;
  }
  if (now_us >= hold->expires_us) {
    return false;
  }
  return !approx_equal(current_network, desired_local);
}

static bool should_keep_local_bool(bool current_network, bool desired_local, uint32_t field_mask, const local_value_hold_t *hold, int64_t now_us) {
  if (hold == NULL || (hold->mask & field_mask) == 0) {
    return false;
  }
  if (now_us >= hold->expires_us) {
    return false;
  }
  return current_network != desired_local;
}

static bool should_keep_local_int(int current_network, int desired_local, uint32_t field_mask, const local_value_hold_t *hold, int64_t now_us) {
  if (hold == NULL || (hold->mask & field_mask) == 0) {
    return false;
  }
  if (now_us >= hold->expires_us) {
    return false;
  }
  return current_network != desired_local;
}

static void note_local_value_hold(local_value_hold_t *hold, const ctrl_values_t *values, uint32_t field_mask) {
  if (hold == NULL || values == NULL || field_mask == LM_CTRL_MACHINE_FIELD_NONE) {
    return;
  }

  hold->values = *values;
  hold->mask |= field_mask;
  hold->expires_us = esp_timer_get_time() + LOCAL_VALUE_HOLD_US;
}

static void reconcile_local_value_hold(local_value_hold_t *hold, const ctrl_values_t *values, uint32_t loaded_mask) {
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

static void merge_loaded_values(ctrl_state_t *state, local_value_hold_t *hold) {
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
  if ((state->feature_mask & CTRL_FEATURE_BBW) == 0 &&
      state->focus >= CTRL_FOCUS_BBW_MODE) {
    state->focus = CTRL_FOCUS_TEMPERATURE;
  }
}

static void maybe_request_value_sync(const ctrl_state_t *state) {
  lm_ctrl_wifi_info_t wifi_info;
  lm_ctrl_machine_link_info_t machine_info = {0};
  uint32_t wanted_mask;

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
  if (machine_info.sync_pending) {
    return;
  }

  if ((machine_info.authenticated || (wifi_info.cloud_connected && wifi_info.has_machine_selection)) &&
      lm_ctrl_machine_link_request_sync_mode(LM_CTRL_MACHINE_SYNC_ALL) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to request machine value sync");
  }
}

static void maybe_request_periodic_value_refresh(int64_t *last_ble_request_us, int64_t *last_cloud_request_us) {
  lm_ctrl_wifi_info_t wifi_info;
  lm_ctrl_machine_link_info_t machine_info = {0};
  const int64_t now_us = esp_timer_get_time();
  bool can_refresh_ble;
  bool can_refresh_cloud;

  if (last_ble_request_us == NULL || last_cloud_request_us == NULL) {
    return;
  }

  lm_ctrl_wifi_get_info(&wifi_info);
  lm_ctrl_machine_link_get_info(&machine_info);
  can_refresh_ble = machine_info.authenticated;
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

static uint32_t supported_machine_fields_for_focus(ctrl_focus_t focus) {
  switch (focus) {
    case CTRL_FOCUS_TEMPERATURE:
      return LM_CTRL_MACHINE_FIELD_TEMPERATURE;
    case CTRL_FOCUS_INFUSE:
      return LM_CTRL_MACHINE_FIELD_INFUSE;
    case CTRL_FOCUS_PAUSE:
      return LM_CTRL_MACHINE_FIELD_PAUSE;
    case CTRL_FOCUS_STEAM:
      return LM_CTRL_MACHINE_FIELD_STEAM;
    case CTRL_FOCUS_STANDBY:
      return LM_CTRL_MACHINE_FIELD_STANDBY;
    case CTRL_FOCUS_BBW_MODE:
      return LM_CTRL_MACHINE_FIELD_BBW_MODE;
    case CTRL_FOCUS_BBW_DOSE_1:
      return LM_CTRL_MACHINE_FIELD_BBW_DOSE_1;
    case CTRL_FOCUS_BBW_DOSE_2:
      return LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
    default:
      return LM_CTRL_MACHINE_FIELD_NONE;
  }
}

void app_main(void) {
  ctrl_state_t state;
  lm_ctrl_ui_t ui;
  lv_disp_t *display = NULL;
  QueueHandle_t input_queue = NULL;
  char status[256];

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ctrl_state_init(&state);
  if (ctrl_state_load(&state) != ESP_OK) {
    ESP_LOGW(TAG, "Falling back to default controller values");
  }
  set_status_from_action(&state, (ctrl_action_t){.type = CTRL_ACTION_NONE}, status, sizeof(status));

  ESP_LOGI(TAG, "La Marzocco controller firmware starting");
  log_state(&state);

  input_queue = xQueueCreate(16, sizeof(lm_ctrl_input_event_t));
  if (input_queue == NULL) {
    ESP_LOGE(TAG, "Failed to allocate input queue");
    return;
  }

  ESP_ERROR_CHECK(lm_ctrl_display_init(&display));
  (void)display;
  ESP_ERROR_CHECK(lm_ctrl_backlight_init());
  ESP_ERROR_CHECK(lm_ctrl_backlight_on());
  ESP_ERROR_CHECK(lm_ctrl_wifi_init());
  ESP_ERROR_CHECK(lm_ctrl_machine_link_init());
  if (lm_ctrl_leds_init() == ESP_OK) {
    sync_led_status_from_connectivity();
  }
  if (lm_ctrl_haptic_init(lm_ctrl_display_i2c_bus()) == ESP_OK) {
    (void)lm_ctrl_haptic_click();
  }

  {
    lm_ctrl_wifi_info_t wifi_info;

    lm_ctrl_wifi_get_info(&wifi_info);
    if (!wifi_info.has_credentials) {
      ctrl_open_setup(&state);
      if (!wifi_info.portal_running) {
        (void)lm_ctrl_wifi_start_portal();
      }
      lm_ctrl_wifi_format_status(status, sizeof(status));
    }
  }

  ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
  ESP_ERROR_CHECK(lm_ctrl_ui_init(&ui, &state, status, ui_action_cb, input_queue));
  esp_lv_adapter_unlock();

  ESP_ERROR_CHECK(lm_ctrl_input_init(input_queue));

  uint32_t last_wifi_status_version = lm_ctrl_wifi_status_version();
  uint32_t last_machine_status_version = lm_ctrl_machine_link_status_version();
  uint32_t last_preset_version = ctrl_state_preset_version();
  int64_t last_cloud_probe_request_us = 0;
  int64_t last_ble_refresh_request_us = 0;
  int64_t last_cloud_refresh_request_us = 0;
  local_value_hold_t local_value_hold = {0};
  delayed_machine_send_t delayed_machine_send = {0};

  maybe_request_cloud_probe(&last_cloud_probe_request_us);
  maybe_request_value_sync(&state);
  maybe_request_periodic_value_refresh(&last_ble_refresh_request_us, &last_cloud_refresh_request_us);

  while (1) {
    lm_ctrl_input_event_t event;
    bool needs_render = false;

    if (xQueueReceive(input_queue, &event, pdMS_TO_TICKS(200)) == pdTRUE) {
      ctrl_action_t action = {
        .type = CTRL_ACTION_NONE,
        .applied_focus = state.focus,
        .preset_slot = -1,
      };
      bool should_persist_state = false;

      switch (event.type) {
        case LM_CTRL_EVENT_ROTATE:
          ctrl_rotate(&state, event.delta_steps);
          {
            const uint32_t field_mask = supported_machine_fields_for_focus(state.focus);
            if (field_mask != LM_CTRL_MACHINE_FIELD_NONE && should_defer_machine_send(field_mask)) {
              note_local_value_hold(&local_value_hold, &state.values, field_mask);
              arm_delayed_machine_send(&delayed_machine_send, field_mask);
            } else if (lm_ctrl_machine_link_queue_values(&state.values, field_mask) != ESP_OK &&
                       field_mask != LM_CTRL_MACHINE_FIELD_NONE) {
              ESP_LOGW(TAG, "Failed to queue BLE update for focus=%s", ctrl_focus_name(state.focus));
            } else if (field_mask != LM_CTRL_MACHINE_FIELD_NONE) {
              note_local_value_hold(&local_value_hold, &state.values, field_mask);
              clear_delayed_machine_send_mask(&delayed_machine_send, field_mask);
            }
          }
          should_persist_state = state.screen == CTRL_SCREEN_MAIN && (
            state.focus == CTRL_FOCUS_TEMPERATURE ||
            state.focus == CTRL_FOCUS_INFUSE ||
            state.focus == CTRL_FOCUS_PAUSE ||
            state.focus == CTRL_FOCUS_BBW_MODE ||
            state.focus == CTRL_FOCUS_BBW_DOSE_1 ||
            state.focus == CTRL_FOCUS_BBW_DOSE_2
          );
          (void)lm_ctrl_leds_indicate_rotation(event.delta_steps);
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        case LM_CTRL_EVENT_SELECT_FOCUS:
          ctrl_set_focus(&state, event.focus);
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        case LM_CTRL_EVENT_TOGGLE_FOCUS:
          ctrl_toggle_focus(&state, event.focus);
          {
            const uint32_t field_mask = supported_machine_fields_for_focus(event.focus);
            if (lm_ctrl_machine_link_queue_values(&state.values, field_mask) != ESP_OK &&
                field_mask != LM_CTRL_MACHINE_FIELD_NONE) {
              ESP_LOGW(TAG, "Failed to queue BLE toggle for focus=%s", ctrl_focus_name(event.focus));
            } else if (field_mask != LM_CTRL_MACHINE_FIELD_NONE) {
              note_local_value_hold(&local_value_hold, &state.values, field_mask);
              clear_delayed_machine_send_mask(&delayed_machine_send, field_mask);
            }
          }
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        case LM_CTRL_EVENT_OPEN_PRESETS:
          ctrl_open_presets(&state);
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        case LM_CTRL_EVENT_LOAD_PRESET:
          action = ctrl_load_preset(&state);
          if (action.type == CTRL_ACTION_LOAD_PRESET) {
            const uint32_t field_mask =
              LM_CTRL_MACHINE_FIELD_TEMPERATURE |
              LM_CTRL_MACHINE_FIELD_PREBREWING |
              (((state.feature_mask & CTRL_FEATURE_BBW) != 0) ? LM_CTRL_MACHINE_FIELD_BBW : 0);
            if (lm_ctrl_machine_link_queue_values(&state.values, field_mask) != ESP_OK) {
              ESP_LOGW(TAG, "Failed to queue BLE preset sync");
            } else {
              note_local_value_hold(&local_value_hold, &state.values, field_mask);
              clear_delayed_machine_send_mask(&delayed_machine_send, field_mask);
            }
          }
          should_persist_state = action.type == CTRL_ACTION_LOAD_PRESET;
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        case LM_CTRL_EVENT_SAVE_PRESET:
          action = ctrl_save_preset(&state);
          should_persist_state = action.type == CTRL_ACTION_SAVE_PRESET;
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        case LM_CTRL_EVENT_OPEN_SETUP:
          ctrl_open_setup(&state);
          if (lm_ctrl_wifi_start_portal() != ESP_OK) {
            snprintf(status, sizeof(status), "Could not start the setup portal.");
          }
          sync_led_status_from_connectivity();
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        case LM_CTRL_EVENT_OPEN_SETUP_RESET:
          ctrl_open_setup_reset(&state);
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        case LM_CTRL_EVENT_CANCEL_SETUP_RESET:
          ctrl_cancel_setup_reset(&state);
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        case LM_CTRL_EVENT_CONFIRM_SETUP_RESET:
          action = ctrl_confirm_setup_reset(&state);
          if (action.type == CTRL_ACTION_RESET_NETWORK) {
            if (lm_ctrl_wifi_reset_network() != ESP_OK) {
              snprintf(status, sizeof(status), "Could not reset network settings.");
            }
          }
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        case LM_CTRL_EVENT_CLOSE_SCREEN:
          ctrl_close_overlay(&state);
          (void)lm_ctrl_haptic_click();
          needs_render = true;
          break;
        default:
          break;
      }

      if (should_persist_state && ctrl_state_persist(&state) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist controller state");
      }

      set_status_from_action(&state, action, status, sizeof(status));
      log_state(&state);
    }

    {
      uint32_t wifi_status_version = lm_ctrl_wifi_status_version();
      if (wifi_status_version != last_wifi_status_version) {
        last_wifi_status_version = wifi_status_version;
        sync_led_status_from_connectivity();
        maybe_request_cloud_probe(&last_cloud_probe_request_us);
        maybe_request_value_sync(&state);
        maybe_request_periodic_value_refresh(&last_ble_refresh_request_us, &last_cloud_refresh_request_us);
        if (state.screen == CTRL_SCREEN_SETUP) {
          lm_ctrl_wifi_format_status(status, sizeof(status));
        }
        needs_render = true;
      }
    }

    {
      uint32_t machine_status_version = lm_ctrl_machine_link_status_version();
      if (machine_status_version != last_machine_status_version) {
        last_machine_status_version = machine_status_version;
        sync_led_status_from_connectivity();
        merge_loaded_values(&state, &local_value_hold);
        maybe_request_value_sync(&state);
        maybe_request_periodic_value_refresh(&last_ble_refresh_request_us, &last_cloud_refresh_request_us);
        needs_render = true;
      }
    }

    {
      uint32_t preset_version = ctrl_state_preset_version();
      if (preset_version != last_preset_version) {
        last_preset_version = preset_version;
        if (ctrl_state_refresh_presets(&state) != ESP_OK) {
          ESP_LOGW(TAG, "Failed to refresh preset definitions");
        } else if (state.screen == CTRL_SCREEN_PRESETS) {
          needs_render = true;
        }
      }
    }

    maybe_request_cloud_probe(&last_cloud_probe_request_us);
    maybe_request_value_sync(&state);
    maybe_request_periodic_value_refresh(&last_ble_refresh_request_us, &last_cloud_refresh_request_us);
    maybe_flush_delayed_machine_send(&state, &delayed_machine_send, &local_value_hold);

    if (needs_render && esp_lv_adapter_lock(-1) == ESP_OK) {
      lm_ctrl_ui_render(&ui, &state, status);
      esp_lv_adapter_unlock();
    }

    lm_ctrl_leds_tick();
  }
}
