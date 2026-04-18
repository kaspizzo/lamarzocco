#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void test_nvs_reset(void);
esp_err_t test_nvs_seed_blob(const char *namespace_name, const char *key, const void *value, size_t length);
esp_err_t test_nvs_seed_u8(const char *namespace_name, const char *key, uint8_t value);
bool test_nvs_has_key(const char *namespace_name, const char *key);
