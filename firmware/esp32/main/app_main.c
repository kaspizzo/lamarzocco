/**
 * Main firmware entrypoint and thin runtime bootstrap for the round controller.
 */
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "board_backlight.h"
#include "board_display.h"
#include "board_haptic.h"
#include "board_leds.h"
#include "board_power.h"
#include "controller_runtime.h"
#include "controller_ui.h"
#include "input.h"
#include "machine_link.h"
#include "wifi_setup.h"

static const char *TAG = "lm_ctrl";

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

  if (!push_ui_event(queue, event_type, focus, 0)) {
    ESP_LOGW(TAG, "UI event queue full, dropping touch action=%d", (int)action);
  }
}

static bool machine_link_get_binding(lm_ctrl_machine_binding_t *binding) {
  return lm_ctrl_wifi_get_machine_binding(binding);
}

static esp_err_t machine_link_execute_cloud_command(
  const char *command,
  const char *json_body,
  lm_ctrl_cloud_command_result_t *result,
  char *status_text,
  size_t status_text_size
) {
  return lm_ctrl_wifi_execute_machine_command(command, json_body, result, status_text, status_text_size);
}

static esp_err_t machine_link_fetch_dashboard_values(
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  lm_ctrl_machine_heat_info_t *heat_info
) {
  return lm_ctrl_wifi_fetch_dashboard_values(values, loaded_mask, feature_mask, heat_info);
}

void app_main(void) {
  lm_ctrl_runtime_t runtime;
  lm_ctrl_ui_t ui;
  lm_ctrl_ui_view_t ui_view = {0};
  lv_disp_t *display = NULL;
  QueueHandle_t input_queue = NULL;
  const lm_ctrl_machine_link_deps_t machine_link_deps = {
    .get_machine_binding = machine_link_get_binding,
    .execute_cloud_command = machine_link_execute_cloud_command,
    .fetch_dashboard_values = machine_link_fetch_dashboard_values,
  };
  ESP_LOGI(TAG, "La Marzocco controller firmware starting");

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
  ESP_ERROR_CHECK(lm_ctrl_machine_link_init(&machine_link_deps));
  ESP_ERROR_CHECK(lm_ctrl_power_init());
  if (lm_ctrl_leds_init() != ESP_OK) {
    ESP_LOGW(TAG, "LED ring init failed");
  }
  if (lm_ctrl_haptic_init(lm_ctrl_display_i2c_bus()) == ESP_OK) {
    (void)lm_ctrl_haptic_click();
  }

  lm_ctrl_runtime_init(&runtime);
  lm_ctrl_runtime_bootstrap(&runtime);
  lm_ctrl_runtime_build_ui_view(&runtime, &ui_view);

  ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
  ESP_ERROR_CHECK(lm_ctrl_ui_init(&ui, lm_ctrl_runtime_state(&runtime), &ui_view, ui_action_cb, input_queue));
  esp_lv_adapter_unlock();

  ESP_ERROR_CHECK(lm_ctrl_input_init(input_queue));

  while (1) {
    lm_ctrl_input_event_t event;
    bool needs_render = false;

    if (xQueueReceive(input_queue, &event, pdMS_TO_TICKS(200)) == pdTRUE) {
      lm_ctrl_runtime_handle_input_event(&runtime, &event, &needs_render);
    }

    lm_ctrl_runtime_handle_wifi_status_change(&runtime, &needs_render);
    lm_ctrl_runtime_handle_power_status_change(&runtime, &needs_render);
    lm_ctrl_runtime_handle_machine_status_change(&runtime, &needs_render);
    lm_ctrl_runtime_handle_preset_change(&runtime, &needs_render);
    lm_ctrl_runtime_tick(&runtime, &needs_render);

    if (needs_render && esp_lv_adapter_lock(-1) == ESP_OK) {
      lm_ctrl_runtime_build_ui_view(&runtime, &ui_view);
      lm_ctrl_ui_render(&ui, lm_ctrl_runtime_state(&runtime), &ui_view);
      esp_lv_adapter_unlock();
    }

    lm_ctrl_leds_tick();
  }
}
