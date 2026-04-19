#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_client.h"

#include "cloud_api.h"
#include "wifi_setup_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Signed request headers and bearer token prepared for a cloud REST call. */
typedef struct {
  char installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  char auth_header[LM_CTRL_CLOUD_WS_TOKEN_LEN + 16];
  char timestamp[24];
  char nonce[LM_CTRL_INSTALLATION_ID_LEN];
  char signature_b64[256];
} lm_ctrl_cloud_request_auth_t;

/** Execute a cloud HTTP request while tracking in-flight request state. */
esp_err_t lm_ctrl_cloud_auth_http_request_capture(
  const char *host,
  const char *path,
  int port,
  esp_http_client_method_t method,
  const lm_ctrl_cloud_http_header_t *headers,
  size_t header_count,
  const char *body,
  int timeout_ms,
  char **response_body,
  int *status_code,
  lm_ctrl_cloud_http_response_meta_t *response_meta
);

/** Prepare bearer and signed headers for a cloud REST call. */
esp_err_t lm_ctrl_cloud_auth_prepare_request_auth(
  const char *username,
  const char *password,
  lm_ctrl_cloud_request_auth_t *auth,
  char *error_text,
  size_t error_text_size
);

#ifdef __cplusplus
}
#endif
