#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the setup page view model and send the rendered portal HTML response. */
esp_err_t lm_ctrl_setup_portal_send_response(httpd_req_t *req, const char *banner, const char *csrf_token);

#ifdef __cplusplus
}
#endif
