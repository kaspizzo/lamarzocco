#pragma once

#include "esp_err.h"

typedef struct {
  unsigned char eky[32];
  unsigned char tky[32];
} nvs_sec_cfg_t;

esp_err_t nvs_flash_init(void);
void nvs_flash_deinit(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_flash_secure_init(nvs_sec_cfg_t *cfg);
