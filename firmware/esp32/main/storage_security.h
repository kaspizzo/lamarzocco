#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize encrypted default NVS and migrate legacy plaintext settings on first hardened boot. */
esp_err_t lm_ctrl_secure_storage_init(void);

#ifdef __cplusplus
}
#endif
