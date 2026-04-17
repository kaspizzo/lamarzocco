#include "board_leds.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "board_config.h"

static const char *TAG = "lm_leds";

#define LM_CTRL_LED_RING_RES_HZ (10 * 1000 * 1000)
#define LM_CTRL_LED_ROTATION_TRAIL_US (350 * 1000)

typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
} lm_ctrl_led_rgb_t;

static led_strip_handle_t s_strip = NULL;
static lm_ctrl_led_status_t s_status = LM_CTRL_LED_STATUS_IDLE;
static int s_motion_index = 0;
static int64_t s_motion_until_us = 0;

static lm_ctrl_led_rgb_t color_for_status(lm_ctrl_led_status_t status) {
  switch (status) {
    case LM_CTRL_LED_STATUS_SETUP:
      return (lm_ctrl_led_rgb_t){.r = 28, .g = 0, .b = 0};
    case LM_CTRL_LED_STATUS_CONNECTING:
      return (lm_ctrl_led_rgb_t){.r = 22, .g = 2, .b = 0};
    case LM_CTRL_LED_STATUS_CONNECTED:
      return (lm_ctrl_led_rgb_t){.r = 32, .g = 0, .b = 0};
    case LM_CTRL_LED_STATUS_IDLE:
    default:
      return (lm_ctrl_led_rgb_t){.r = 10, .g = 0, .b = 0};
  }
}

static int wrap_index(int index) {
  const int count = LM_CTRL_LED_RING_COUNT;
  int wrapped = index % count;
  if (wrapped < 0) {
    wrapped += count;
  }
  return wrapped;
}

static esp_err_t render_leds(void) {
  const int64_t now_us = esp_timer_get_time();
  const bool motion_active = now_us < s_motion_until_us;
  const lm_ctrl_led_rgb_t base_color = color_for_status(s_status);

  if (s_strip == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  for (int i = 0; i < LM_CTRL_LED_RING_COUNT; ++i) {
    lm_ctrl_led_rgb_t pixel = base_color;

    if (motion_active) {
      const int prev_index = wrap_index(s_motion_index - 1);
      const int next_index = wrap_index(s_motion_index + 1);

      if (i == s_motion_index) {
        pixel = (lm_ctrl_led_rgb_t){.r = 255, .g = 32, .b = 24};
      } else if (i == prev_index || i == next_index) {
        pixel = (lm_ctrl_led_rgb_t){.r = 72, .g = 8, .b = 8};
      }
    }

    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, i, pixel.r, pixel.g, pixel.b), TAG, "Failed to set LED pixel");
  }

  ESP_RETURN_ON_ERROR(led_strip_refresh(s_strip), TAG, "Failed to refresh LED strip");
  return ESP_OK;
}

esp_err_t lm_ctrl_leds_init(void) {
  led_strip_config_t strip_config = {
    .strip_gpio_num = LM_CTRL_LED_RING_GPIO,
    .max_leds = LM_CTRL_LED_RING_COUNT,
    .led_model = LED_MODEL_WS2812,
    .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    .flags = {
      .invert_out = false,
    },
  };
  led_strip_rmt_config_t rmt_config = {
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = LM_CTRL_LED_RING_RES_HZ,
    .mem_block_symbols = 0,
    .flags = {
      .with_dma = false,
    },
  };

  if (s_strip != NULL) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG, "Failed to init LED ring");
  s_status = LM_CTRL_LED_STATUS_IDLE;
  s_motion_index = 0;
  s_motion_until_us = 0;
  ESP_RETURN_ON_ERROR(render_leds(), TAG, "Failed to render LED ring");
  ESP_LOGI(TAG, "LED ring initialized on GPIO %d", LM_CTRL_LED_RING_GPIO);
  return ESP_OK;
}

esp_err_t lm_ctrl_leds_set_status(lm_ctrl_led_status_t status) {
  if (s_strip == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  s_status = status;
  return render_leds();
}

esp_err_t lm_ctrl_leds_indicate_rotation(int delta_steps) {
  if (s_strip == NULL || delta_steps == 0) {
    return s_strip != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
  }

  s_motion_index = wrap_index(s_motion_index + (delta_steps > 0 ? 1 : -1));
  s_motion_until_us = esp_timer_get_time() + LM_CTRL_LED_ROTATION_TRAIL_US;
  return render_leds();
}

void lm_ctrl_leds_tick(void) {
  static bool motion_was_active = false;
  const bool motion_active = esp_timer_get_time() < s_motion_until_us;

  if (s_strip == NULL) {
    return;
  }

  if (motion_was_active && !motion_active) {
    s_motion_until_us = 0;
    (void)render_leds();
  }
  motion_was_active = motion_active;
}
