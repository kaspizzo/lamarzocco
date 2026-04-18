#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lm_ctrl_setup_portal_parse_form_value(const char *body, const char *key, char *dst, size_t dst_size);
esp_err_t lm_ctrl_setup_portal_send_json_result(httpd_req_t *req, const char *status, bool ok, const char *message);

#ifdef __cplusplus
}
#endif
