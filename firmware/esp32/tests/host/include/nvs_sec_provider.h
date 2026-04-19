#pragma once

#include "esp_err.h"
#include "nvs_flash.h"

typedef struct nvs_sec_scheme_s {
  int unused;
} nvs_sec_scheme_t;

typedef struct {
  int hmac_key_id;
} nvs_sec_config_hmac_t;

esp_err_t nvs_sec_provider_register_hmac(const nvs_sec_config_hmac_t *cfg, nvs_sec_scheme_t **out_scheme);
void nvs_sec_provider_deregister(nvs_sec_scheme_t *scheme);
esp_err_t nvs_flash_generate_keys_v2(nvs_sec_scheme_t *scheme, nvs_sec_cfg_t *cfg);
esp_err_t nvs_flash_read_security_cfg_v2(nvs_sec_scheme_t *scheme, nvs_sec_cfg_t *cfg);
