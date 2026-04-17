#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/** Initialize the DRV2605L haptics driver on the shared I2C bus. */
esp_err_t lm_ctrl_haptic_init(i2c_master_bus_handle_t bus);
/** Trigger a short click effect if haptics are available. */
esp_err_t lm_ctrl_haptic_click(void);
/** Return true if the haptics driver initialized successfully. */
bool lm_ctrl_haptic_available(void);
