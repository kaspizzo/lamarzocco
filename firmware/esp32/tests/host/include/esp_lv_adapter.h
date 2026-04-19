#pragma once

#include "esp_err.h"

esp_err_t esp_lv_adapter_lock(int timeout_ms);
void esp_lv_adapter_unlock(void);
