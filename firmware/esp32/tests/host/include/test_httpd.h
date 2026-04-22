#pragma once

#include "esp_http_server.h"

httpd_req_t *test_httpd_request_create(void);
void test_httpd_request_destroy(httpd_req_t *req);
void test_httpd_request_set_uri(httpd_req_t *req, const char *uri);
esp_err_t test_httpd_request_set_body(httpd_req_t *req, const char *body);
esp_err_t test_httpd_request_set_header(httpd_req_t *req, const char *name, const char *value);
esp_err_t test_httpd_request_set_cookie(httpd_req_t *req, const char *cookie_header);
const char *test_httpd_request_body(const httpd_req_t *req);
const char *test_httpd_request_type(const httpd_req_t *req);
const char *test_httpd_request_status(const httpd_req_t *req);
const char *test_httpd_response_header(const httpd_req_t *req, const char *name);
void test_httpd_reset_server_config(void);
const httpd_config_t *test_httpd_last_config(void);
