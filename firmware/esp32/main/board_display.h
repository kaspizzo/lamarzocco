#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"

/**
 * Initialize the display, touch stack, and LVGL display driver.
 *
 * @param[out] out_display Optional LVGL display handle for the active panel.
 */
esp_err_t lm_ctrl_display_init(lv_disp_t **out_display);
/** Return the shared I2C bus handle used for touch and haptics. */
i2c_master_bus_handle_t lm_ctrl_display_i2c_bus(void);
