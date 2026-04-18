#pragma once

#include "esp_http_server.h"

httpd_req_t *test_httpd_request_create(void);
void test_httpd_request_destroy(httpd_req_t *req);
const char *test_httpd_request_body(const httpd_req_t *req);
const char *test_httpd_request_type(const httpd_req_t *req);
