#include "input.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"

static const char *TAG = "lm_input";

#if LM_CTRL_ENCODER_ENABLED
#define LM_CTRL_KNOB_POLL_INTERVAL_US 3000
#define LM_CTRL_KNOB_DEBOUNCE_TICKS 2

static QueueHandle_t s_event_queue = NULL;
static esp_timer_handle_t s_knob_timer = NULL;
static uint8_t s_encoder_a_level = 1;
static uint8_t s_encoder_b_level = 1;
static uint8_t s_debounce_a_cnt = 0;
static uint8_t s_debounce_b_cnt = 0;
static int s_count_value = 0;

static void push_event(lm_ctrl_input_event_type_t type, int delta_steps) {
  if (s_event_queue == NULL) {
    return;
  }

  const lm_ctrl_input_event_t event = {
    .type = type,
    .delta_steps = delta_steps,
    .focus = CTRL_FOCUS_TEMPERATURE,
  };
  if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Input queue full, dropping event type=%d", (int)type);
  }
}

static void process_knob_channel(uint8_t current_level, uint8_t *prev_level, uint8_t *debounce_cnt, int delta_steps) {
  if (current_level == 0) {
    if (current_level != *prev_level) {
      *debounce_cnt = 0;
    } else {
      (*debounce_cnt)++;
    }
  } else {
    if (current_level != *prev_level && ++(*debounce_cnt) >= LM_CTRL_KNOB_DEBOUNCE_TICKS) {
      *debounce_cnt = 0;
      const int adjusted_delta = delta_steps * LM_CTRL_KNOB_DIRECTION;
      s_count_value += adjusted_delta;
      push_event(LM_CTRL_EVENT_ROTATE, adjusted_delta);
      ESP_LOGD(TAG, "Ring step delta=%d count=%d levels=(%u,%u)", adjusted_delta, s_count_value, s_encoder_a_level, s_encoder_b_level);
    } else {
      *debounce_cnt = 0;
    }
  }

  *prev_level = current_level;
}

static void knob_poll_cb(void *arg) {
  (void)arg;

  const uint8_t pha_value = (uint8_t)gpio_get_level(LM_CTRL_KNOB_A);
  const uint8_t phb_value = (uint8_t)gpio_get_level(LM_CTRL_KNOB_B);

  process_knob_channel(pha_value, &s_encoder_a_level, &s_debounce_a_cnt, +1);
  process_knob_channel(phb_value, &s_encoder_b_level, &s_debounce_b_cnt, -1);
}

static esp_err_t init_knob_gpio(gpio_num_t gpio_num) {
  const gpio_config_t gpio_cfg = {
    .pin_bit_mask = (1ULL << gpio_num),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  return gpio_config(&gpio_cfg);
}

esp_err_t lm_ctrl_input_init(QueueHandle_t event_queue) {
  if (event_queue == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (s_knob_timer != NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  s_event_queue = event_queue;
  s_count_value = 0;
  s_debounce_a_cnt = 0;
  s_debounce_b_cnt = 0;

  ESP_RETURN_ON_ERROR(init_knob_gpio(LM_CTRL_KNOB_A), TAG, "Encoder A gpio init failed");
  ESP_RETURN_ON_ERROR(init_knob_gpio(LM_CTRL_KNOB_B), TAG, "Encoder B gpio init failed");

  s_encoder_a_level = (uint8_t)gpio_get_level(LM_CTRL_KNOB_A);
  s_encoder_b_level = (uint8_t)gpio_get_level(LM_CTRL_KNOB_B);

  const esp_timer_create_args_t timer_args = {
    .callback = knob_poll_cb,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "lm_knob",
  };
  ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_knob_timer), TAG, "Failed to create knob timer");
  ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_knob_timer, LM_CTRL_KNOB_POLL_INTERVAL_US), TAG, "Failed to start knob timer");

  ESP_LOGI(TAG, "Vendor-style ring input initialized on A=%d B=%d", LM_CTRL_KNOB_A, LM_CTRL_KNOB_B);
  return ESP_OK;
}
#else
esp_err_t lm_ctrl_input_init(QueueHandle_t event_queue) {
  (void)event_queue;
  ESP_LOGW(TAG, "Encoder input disabled for touch-only hardware variant");
  return ESP_OK;
}
#endif
