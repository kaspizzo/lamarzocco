#include "board_backlight.h"

#include "driver/ledc.h"
#include "esp_log.h"

#include "board_config.h"

#define LM_CTRL_BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define LM_CTRL_BACKLIGHT_TIMER LEDC_TIMER_1

static const char *TAG = "lm_backlight";

esp_err_t lm_ctrl_backlight_init(void) {
  const ledc_timer_config_t timer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_10_BIT,
    .timer_num = LM_CTRL_BACKLIGHT_TIMER,
    .freq_hz = 20000,
    .clk_cfg = LEDC_AUTO_CLK,
  };

  const ledc_channel_config_t channel = {
    .gpio_num = LM_CTRL_LCD_BL,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = LM_CTRL_BACKLIGHT_CHANNEL,
    .intr_type = LEDC_INTR_DISABLE,
    .timer_sel = LM_CTRL_BACKLIGHT_TIMER,
    .duty = 0,
    .hpoint = 0,
  };

  ESP_ERROR_CHECK(ledc_timer_config(&timer));
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
  return ESP_OK;
}

esp_err_t lm_ctrl_backlight_set(int brightness_percent) {
  if (brightness_percent < 0) {
    brightness_percent = 0;
  }
  if (brightness_percent > 100) {
    brightness_percent = 100;
  }

  ESP_LOGI(TAG, "LCD brightness %d%%", brightness_percent);
  const uint32_t duty = (1023U * (uint32_t)brightness_percent) / 100U;
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LM_CTRL_BACKLIGHT_CHANNEL, duty));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LM_CTRL_BACKLIGHT_CHANNEL));
  return ESP_OK;
}

esp_err_t lm_ctrl_backlight_on(void) {
  return lm_ctrl_backlight_set(95);
}

esp_err_t lm_ctrl_backlight_off(void) {
  return lm_ctrl_backlight_set(0);
}
