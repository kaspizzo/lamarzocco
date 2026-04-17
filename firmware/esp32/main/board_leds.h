#pragma once

#include "esp_err.h"

/** High-level LED ring modes used by the controller runtime. */
typedef enum {
  LM_CTRL_LED_STATUS_IDLE = 0,
  LM_CTRL_LED_STATUS_SETUP,
  LM_CTRL_LED_STATUS_CONNECTING,
  LM_CTRL_LED_STATUS_CONNECTED,
} lm_ctrl_led_status_t;

/** Initialize the LED ring driver and clear the strip. */
esp_err_t lm_ctrl_leds_init(void);
/** Apply a persistent LED ring status pattern. */
esp_err_t lm_ctrl_leds_set_status(lm_ctrl_led_status_t status);
/** Show a short rotational feedback animation for the outer ring. */
esp_err_t lm_ctrl_leds_indicate_rotation(int delta_steps);
/** Advance time-based LED animations from the main loop. */
void lm_ctrl_leds_tick(void);
