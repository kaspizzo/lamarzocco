#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool available;
  bool charging;
  bool low;
  bool usb_connected;
  uint8_t level_percent;
} lm_ctrl_power_info_t;

esp_err_t lm_ctrl_power_init(void);
void lm_ctrl_power_get_info(lm_ctrl_power_info_t *info);
uint32_t lm_ctrl_power_status_version(void);

#ifdef __cplusplus
}
#endif
