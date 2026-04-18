#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the setup portal HTTP server and register all portal and captive routes. */
esp_err_t lm_ctrl_setup_portal_start_http_server(void);

#ifdef __cplusplus
}
#endif
