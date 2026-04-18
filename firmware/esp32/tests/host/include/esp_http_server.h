#pragma once

#include "esp_err.h"

typedef struct httpd_req httpd_req_t;

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *req, const char *str);
