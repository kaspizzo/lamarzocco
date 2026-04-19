#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "esp_err.h"

typedef struct {
  char name[64];
  char value[256];
} test_httpd_header_t;

typedef struct httpd_req {
  const char *uri;
  int content_len;
  char *request_body;
  size_t request_body_size;
  size_t request_body_offset;
  test_httpd_header_t request_headers[16];
  size_t request_header_count;

  char *status;
  char *type;
  char *body;
  size_t body_len;
  size_t body_capacity;
  test_httpd_header_t response_headers[16];
  size_t response_header_count;
} httpd_req_t;

typedef void *httpd_handle_t;

typedef enum {
  HTTP_GET = 0,
  HTTP_POST = 1,
} httpd_method_t;

typedef struct {
  const char *uri;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t *req);
  void *user_ctx;
} httpd_uri_t;

typedef struct {
  int max_uri_handlers;
  int stack_size;
  bool (*uri_match_fn)(const char *template_uri, const char *requested_uri, size_t requested_uri_len);
} httpd_config_t;

#define HTTPD_400_BAD_REQUEST "400 Bad Request"
#define HTTPD_401_UNAUTHORIZED "401 Unauthorized"
#define HTTPD_403_FORBIDDEN "403 Forbidden"
#define HTTPD_404_NOT_FOUND "404 Not Found"
#define HTTPD_500_INTERNAL_SERVER_ERROR "500 Internal Server Error"
#define HTTPD_501_NOT_IMPLEMENTED "501 Not Implemented"

#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){.max_uri_handlers = 8, .stack_size = 4096, .uri_match_fn = NULL})

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value);
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *str);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *req, const char *str);
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_err(httpd_req_t *req, const char *status, const char *message);

esp_err_t httpd_req_get_cookie_val(httpd_req_t *req, const char *name, char *buf, size_t *buf_len);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *buf, size_t buf_len);
int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len);

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);
bool httpd_uri_match_wildcard(const char *template_uri, const char *requested_uri, size_t requested_uri_len);
