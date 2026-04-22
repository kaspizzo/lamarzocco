#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef enum {
  HTTP_EVENT_ERROR = 0,
  HTTP_EVENT_ON_HEADER = 1,
  HTTP_EVENT_ON_DATA = 2,
} esp_http_client_event_id_t;

typedef struct esp_http_client_event {
  esp_http_client_event_id_t event_id;
  void *user_data;
  const char *header_key;
  const char *header_value;
  const void *data;
  int data_len;
} esp_http_client_event_t;

typedef esp_err_t (*esp_http_client_event_cb_t)(esp_http_client_event_t *event);

typedef struct {
  const char *host;
  const char *path;
  int port;
  int transport_type;
  int method;
  int timeout_ms;
  esp_http_client_event_cb_t event_handler;
  esp_err_t (*crt_bundle_attach)(void *config);
  int addr_type;
  int buffer_size;
  int buffer_size_tx;
  void *user_data;
} esp_http_client_config_t;

typedef struct test_esp_http_client *esp_http_client_handle_t;

#define HTTP_TRANSPORT_OVER_SSL 1
#define HTTP_ADDR_TYPE_INET 1

typedef struct {
  esp_http_client_event_id_t event_id;
  const char *header_key;
  const char *header_value;
  const void *data;
  int data_len;
} test_http_client_event_spec_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
void esp_http_client_cleanup(esp_http_client_handle_t client);

void test_http_client_reset(void);
void test_http_client_set_status_code(int status_code);
void test_http_client_set_perform_result(esp_err_t result);
void test_http_client_set_response_events(const test_http_client_event_spec_t *events, size_t event_count);
