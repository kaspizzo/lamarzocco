#include "cloud_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#include "cloud_auth_internal.h"
#include "cloud_live_updates.h"

static const char *TAG = "lm_cloud_auth";

static void append_text(char *buffer, size_t buffer_size, const char *text) {
  size_t used;

  if (buffer == NULL || buffer_size == 0 || text == NULL || text[0] == '\0') {
    return;
  }

  used = strnlen(buffer, buffer_size);
  if (used >= buffer_size - 1) {
    return;
  }

  snprintf(buffer + used, buffer_size - used, "%s", text);
}

static void mark_cloud_http_request_started(void) {
  lock_state();
  if (s_state.cloud_http_requests_in_flight < UINT8_MAX) {
    s_state.cloud_http_requests_in_flight++;
  }
  unlock_state();
}

static void mark_cloud_http_request_finished(void) {
  bool should_reconsider_websocket = false;

  lock_state();
  if (s_state.cloud_http_requests_in_flight > 0) {
    s_state.cloud_http_requests_in_flight--;
  }
  should_reconsider_websocket = s_state.cloud_http_requests_in_flight == 0;
  unlock_state();

  if (should_reconsider_websocket) {
    (void)lm_ctrl_cloud_live_updates_ensure_task();
  }
}

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
  int *status_code
) {
  esp_err_t ret;

  ESP_LOGI(
    TAG,
    "HTTP request: %s%s heap=%u internal=%u largest_internal=%u",
    host,
    path,
    (unsigned)esp_get_free_heap_size(),
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
  );
  mark_cloud_http_request_started();
  ret = lm_ctrl_cloud_http_request(
    host,
    path,
    port,
    method,
    headers,
    header_count,
    body,
    timeout_ms,
    response_body,
    status_code
  );
  mark_cloud_http_request_finished();
  if (ret != ESP_OK) {
    ESP_LOGE(
      TAG,
      "HTTP request failed for %s%s heap=%u internal=%u largest_internal=%u: %s",
      host,
      path,
      (unsigned)esp_get_free_heap_size(),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      esp_err_to_name(ret)
    );
  }

  return ret;
}

static esp_err_t base64_decode_bytes(const char *input, uint8_t *output, size_t output_size, size_t *output_len) {
  int ret;

  if (input == NULL || output == NULL || output_len == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *output_len = 0;
  ret = mbedtls_base64_decode(output, output_size, output_len, (const unsigned char *)input, strlen(input));
  return ret == 0 ? ESP_OK : ESP_FAIL;
}

static bool parse_cloud_access_token_jwt_times(
  const char *access_token,
  int64_t *expiry_epoch_ms,
  int64_t *issued_epoch_ms,
  int64_t *not_before_epoch_ms
) {
  const char *payload_start;
  const char *payload_end;
  size_t payload_len;
  size_t normalized_len;
  size_t decoded_len = 0;
  char normalized[768];
  unsigned char decoded[512];
  cJSON *root = NULL;
  cJSON *exp_item;
  cJSON *iat_item;
  cJSON *nbf_item;
  bool parsed = false;

  if (access_token == NULL || expiry_epoch_ms == NULL || issued_epoch_ms == NULL || not_before_epoch_ms == NULL) {
    return false;
  }

  *expiry_epoch_ms = 0;
  *issued_epoch_ms = 0;
  *not_before_epoch_ms = 0;

  payload_start = strchr(access_token, '.');
  if (payload_start == NULL) {
    return false;
  }
  payload_start++;
  payload_end = strchr(payload_start, '.');
  if (payload_end == NULL || payload_end <= payload_start) {
    return false;
  }

  payload_len = (size_t)(payload_end - payload_start);
  if (payload_len == 0 || payload_len > sizeof(normalized) - 5) {
    return false;
  }

  for (size_t i = 0; i < payload_len; ++i) {
    char ch = payload_start[i];

    if (ch == '-') {
      normalized[i] = '+';
    } else if (ch == '_') {
      normalized[i] = '/';
    } else {
      normalized[i] = ch;
    }
  }
  normalized_len = payload_len;
  while ((normalized_len % 4U) != 0U) {
    normalized[normalized_len++] = '=';
  }
  normalized[normalized_len] = '\0';

  if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len, (const unsigned char *)normalized, normalized_len) != 0 ||
      decoded_len == 0) {
    return false;
  }
  decoded[decoded_len] = '\0';

  root = cJSON_Parse((const char *)decoded);
  if (root == NULL) {
    return false;
  }

  exp_item = cJSON_GetObjectItemCaseSensitive(root, "exp");
  iat_item = cJSON_GetObjectItemCaseSensitive(root, "iat");
  nbf_item = cJSON_GetObjectItemCaseSensitive(root, "nbf");
  if (cJSON_IsNumber(exp_item)) {
    *expiry_epoch_ms = (int64_t)exp_item->valuedouble * 1000LL;
    parsed = true;
  }
  if (cJSON_IsNumber(iat_item)) {
    *issued_epoch_ms = (int64_t)iat_item->valuedouble * 1000LL;
  }
  if (cJSON_IsNumber(nbf_item)) {
    *not_before_epoch_ms = (int64_t)nbf_item->valuedouble * 1000LL;
  }

  cJSON_Delete(root);
  return parsed;
}

static int64_t compute_cloud_access_token_cache_until_us(const char *access_token) {
  int64_t expiry_epoch_ms = 0;
  int64_t issued_epoch_ms = 0;
  int64_t not_before_epoch_ms = 0;
  int64_t remaining_ms = 0;
  int64_t now_epoch_ms = current_epoch_ms();
  int64_t now_us = esp_timer_get_time();

  if (!parse_cloud_access_token_jwt_times(access_token, &expiry_epoch_ms, &issued_epoch_ms, &not_before_epoch_ms)) {
    ESP_LOGI(TAG, "Cloud access token cache fallback: 30s");
    return now_us + LM_CTRL_CLOUD_ACCESS_TOKEN_CACHE_FALLBACK_US;
  }

  if (now_epoch_ms > 0 && expiry_epoch_ms > now_epoch_ms) {
    remaining_ms = expiry_epoch_ms - now_epoch_ms;
  } else if (expiry_epoch_ms > 0 && issued_epoch_ms > 0 && expiry_epoch_ms > issued_epoch_ms) {
    remaining_ms = expiry_epoch_ms - issued_epoch_ms;
    ESP_LOGI(TAG, "Cloud access token wall clock unavailable; using JWT lifetime from iat/exp");
  } else if (expiry_epoch_ms > 0 && not_before_epoch_ms > 0 && expiry_epoch_ms > not_before_epoch_ms) {
    remaining_ms = expiry_epoch_ms - not_before_epoch_ms;
    ESP_LOGI(TAG, "Cloud access token wall clock unavailable; using JWT lifetime from nbf/exp");
  } else {
    ESP_LOGI(TAG, "Cloud access token JWT exp found, but no usable lifetime claims; using 30s fallback");
    return now_us + LM_CTRL_CLOUD_ACCESS_TOKEN_CACHE_FALLBACK_US;
  }

  if (remaining_ms <= LM_CTRL_CLOUD_ACCESS_TOKEN_EXP_SAFETY_MS) {
    ESP_LOGW(TAG, "Cloud access token JWT exp is too close; using 30s fallback");
    return now_us + LM_CTRL_CLOUD_ACCESS_TOKEN_CACHE_FALLBACK_US;
  }

  remaining_ms -= LM_CTRL_CLOUD_ACCESS_TOKEN_EXP_SAFETY_MS;
  ESP_LOGI(TAG, "Cloud access token cached for %llds from JWT exp", (long long)(remaining_ms / 1000LL));
  return now_us + (remaining_ms * 1000LL);
}

void store_cached_cloud_access_token(const char *access_token) {
  int64_t valid_until_us;

  if (access_token == NULL || access_token[0] == '\0') {
    return;
  }

  valid_until_us = compute_cloud_access_token_cache_until_us(access_token);
  lock_state();
  copy_text(s_state.cloud_access_token, sizeof(s_state.cloud_access_token), access_token);
  s_state.cloud_access_token_valid_until_us = valid_until_us;
  unlock_state();
}

bool copy_cached_cloud_access_token(char *buffer, size_t buffer_size) {
  bool valid = false;
  int64_t now_us = esp_timer_get_time();

  if (buffer == NULL || buffer_size == 0) {
    return false;
  }

  lock_state();
  valid = s_state.cloud_access_token[0] != '\0' &&
          s_state.cloud_access_token_valid_until_us != 0 &&
          now_us < s_state.cloud_access_token_valid_until_us;
  if (valid) {
    copy_text(buffer, buffer_size, s_state.cloud_access_token);
  }
  unlock_state();

  return valid;
}

static esp_err_t derive_installation_material(
  const char *installation_id,
  const uint8_t *private_key_der,
  size_t private_key_der_len,
  char *public_key_b64,
  size_t public_key_b64_size,
  char *base_string,
  size_t base_string_size
) {
  return lm_ctrl_cloud_derive_installation_material(
    installation_id,
    private_key_der,
    private_key_der_len,
    public_key_b64,
    public_key_b64_size,
    base_string,
    base_string_size
  );
}

static esp_err_t generate_request_proof_text(
  const char *base_string,
  const uint8_t secret[32],
  char *proof,
  size_t proof_size
) {
  return lm_ctrl_cloud_generate_request_proof_text(base_string, secret, proof, proof_size);
}

static esp_err_t set_cloud_installation_registration(bool registered) {
  lock_state();
  s_state.cloud_installation_registered = registered;
  unlock_state();
  return ESP_OK;
}

static esp_err_t ensure_cloud_installation(void) {
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t secret_len = 0;
  size_t key_der_len = 0;
  esp_err_t ret;

  lock_state();
  if (s_state.cloud_installation_ready) {
    unlock_state();
    return ESP_OK;
  }
  unlock_state();

  ret = base64_decode_bytes(LM_CTRL_STATIC_INSTALLATION_SECRET_B64, secret, sizeof(secret), &secret_len);
  if (ret != ESP_OK || secret_len != LM_CTRL_CLOUD_SECRET_LEN) {
    ESP_LOGE(TAG, "Failed to decode static cloud secret");
    return ESP_FAIL;
  }

  ret = base64_decode_bytes(LM_CTRL_STATIC_PRIVATE_KEY_B64, key_der, sizeof(key_der), &key_der_len);
  if (ret != ESP_OK || key_der_len == 0) {
    ESP_LOGE(TAG, "Failed to decode static cloud private key");
    return ret;
  }

  lock_state();
  copy_text(s_state.cloud_installation_id, sizeof(s_state.cloud_installation_id), LM_CTRL_STATIC_INSTALLATION_ID);
  memcpy(s_state.cloud_secret, secret, sizeof(secret));
  memcpy(s_state.cloud_private_key_der, key_der, key_der_len);
  s_state.cloud_private_key_der_len = key_der_len;
  s_state.cloud_installation_registered = false;
  s_state.cloud_installation_ready = true;
  unlock_state();

  return ESP_OK;
}

static esp_err_t build_signed_request_headers(
  const char *installation_id,
  const uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN],
  const uint8_t *private_key_der,
  size_t private_key_der_len,
  char *timestamp,
  size_t timestamp_size,
  char *nonce,
  size_t nonce_size,
  char *signature_b64,
  size_t signature_b64_size
) {
  lm_ctrl_cloud_installation_t installation = {0};

  if (installation_id == NULL ||
      secret == NULL ||
      private_key_der == NULL ||
      private_key_der_len == 0 ||
      timestamp == NULL ||
      nonce == NULL ||
      signature_b64 == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  copy_text(installation.installation_id, sizeof(installation.installation_id), installation_id);
  memcpy(installation.secret, secret, sizeof(installation.secret));
  memcpy(installation.private_key_der, private_key_der, private_key_der_len);
  installation.private_key_der_len = private_key_der_len;

  return lm_ctrl_cloud_build_signed_request_headers(
    &installation,
    timestamp,
    timestamp_size,
    nonce,
    nonce_size,
    signature_b64,
    signature_b64_size
  );
}

static esp_err_t parse_cloud_access_token(
  const char *response_body,
  char *access_token,
  size_t access_token_size
) {
  return lm_ctrl_cloud_parse_access_token(response_body, access_token, access_token_size);
}

static esp_err_t ensure_cloud_registration(char *error_text, size_t error_text_size) {
  char installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t private_key_der_len = 0;
  char public_key_b64[256];
  char base_string[128];
  char request_proof[64];
  char *request_body = NULL;
  char *response_body = NULL;
  int status_code = 0;
  cJSON *body_root = NULL;
  lm_ctrl_cloud_http_header_t headers[3];
  esp_err_t ret;

  if (error_text != NULL && error_text_size > 0) {
    error_text[0] = '\0';
  }

  ret = ensure_cloud_installation();
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud installation setup failed.");
    }
    return ret;
  }

  lock_state();
  if (s_state.cloud_installation_registered) {
    unlock_state();
    return ESP_OK;
  }
  copy_text(installation_id, sizeof(installation_id), s_state.cloud_installation_id);
  memcpy(secret, s_state.cloud_secret, sizeof(secret));
  memcpy(private_key_der, s_state.cloud_private_key_der, s_state.cloud_private_key_der_len);
  private_key_der_len = s_state.cloud_private_key_der_len;
  unlock_state();

  ret = derive_installation_material(
    installation_id,
    private_key_der,
    private_key_der_len,
    public_key_b64,
    sizeof(public_key_b64),
    base_string,
    sizeof(base_string)
  );
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud key material could not be prepared.");
    }
    return ret;
  }

  ret = generate_request_proof_text(base_string, secret, request_proof, sizeof(request_proof));
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud registration proof failed.");
    }
    return ret;
  }

  body_root = cJSON_CreateObject();
  if (body_root == NULL) {
    return ESP_ERR_NO_MEM;
  }
  if (!cJSON_AddStringToObject(body_root, "pk", public_key_b64)) {
    cJSON_Delete(body_root);
    return ESP_ERR_NO_MEM;
  }
  request_body = cJSON_PrintUnformatted(body_root);
  cJSON_Delete(body_root);
  if (request_body == NULL) {
    return ESP_ERR_NO_MEM;
  }

  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "Content-Type", .value = "application/json" };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "X-App-Installation-Id", .value = installation_id };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "X-Request-Proof", .value = request_proof };

  ret = lm_ctrl_cloud_auth_http_request_capture(
    LM_CTRL_CLOUD_HOST,
    LM_CTRL_CLOUD_AUTH_INIT_PATH,
    LM_CTRL_CLOUD_PORT,
    HTTP_METHOD_POST,
    headers,
    sizeof(headers) / sizeof(headers[0]),
    request_body,
    12000,
    &response_body,
    &status_code
  );
  free(request_body);
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud registration request failed.");
    }
    return ret;
  }

  if (status_code < 200 || status_code >= 300) {
    free(response_body);
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud registration failed with status %d.", status_code);
    }
    return ESP_FAIL;
  }

  free(response_body);
  return set_cloud_installation_registration(true);
}

static esp_err_t fetch_cloud_access_token(
  const char *username,
  const char *password,
  char *access_token,
  size_t access_token_size,
  char *error_text,
  size_t error_text_size
) {
  char installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t private_key_der_len = 0;
  char timestamp[24];
  char nonce[LM_CTRL_INSTALLATION_ID_LEN];
  char signature_b64[256];
  char *request_body = NULL;
  char *response_body = NULL;
  cJSON *body_root = NULL;
  int status_code = 0;
  esp_err_t ret;

  if (error_text != NULL && error_text_size > 0) {
    error_text[0] = '\0';
  }

  for (int attempt = 0; attempt < 2; ++attempt) {
    lm_ctrl_cloud_http_header_t headers[5];

    ret = ensure_cloud_registration(error_text, error_text_size);
    if (ret != ESP_OK) {
      return ret;
    }

    lock_state();
    copy_text(installation_id, sizeof(installation_id), s_state.cloud_installation_id);
    memcpy(secret, s_state.cloud_secret, sizeof(secret));
    memcpy(private_key_der, s_state.cloud_private_key_der, s_state.cloud_private_key_der_len);
    private_key_der_len = s_state.cloud_private_key_der_len;
    unlock_state();

    ret = build_signed_request_headers(
      installation_id,
      secret,
      private_key_der,
      private_key_der_len,
      timestamp,
      sizeof(timestamp),
      nonce,
      sizeof(nonce),
      signature_b64,
      sizeof(signature_b64)
    );
    if (ret != ESP_OK) {
      if (error_text != NULL && error_text_size > 0) {
        snprintf(error_text, error_text_size, "Cloud request signing failed.");
      }
      return ret;
    }

    body_root = cJSON_CreateObject();
    if (body_root == NULL) {
      return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(body_root, "username", username) ||
        !cJSON_AddStringToObject(body_root, "password", password)) {
      cJSON_Delete(body_root);
      return ESP_ERR_NO_MEM;
    }
    request_body = cJSON_PrintUnformatted(body_root);
    cJSON_Delete(body_root);
    if (request_body == NULL) {
      return ESP_ERR_NO_MEM;
    }

    headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "Content-Type", .value = "application/json" };
    headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "X-App-Installation-Id", .value = installation_id };
    headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "X-Timestamp", .value = timestamp };
    headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "X-Nonce", .value = nonce };
    headers[4] = (lm_ctrl_cloud_http_header_t){ .name = "X-Request-Signature", .value = signature_b64 };

    ret = lm_ctrl_cloud_auth_http_request_capture(
      LM_CTRL_CLOUD_HOST,
      LM_CTRL_CLOUD_AUTH_SIGNIN_PATH,
      LM_CTRL_CLOUD_PORT,
      HTTP_METHOD_POST,
      headers,
      sizeof(headers) / sizeof(headers[0]),
      request_body,
      12000,
      &response_body,
      &status_code
    );
    free(request_body);
    request_body = NULL;

    if (ret != ESP_OK) {
      set_cloud_connected(false);
      if (error_text != NULL && error_text_size > 0) {
        snprintf(error_text, error_text_size, "Cloud login request failed.");
      }
      return ret;
    }

    if (status_code == 401) {
      free(response_body);
      set_cloud_connected(false);
      if (error_text != NULL && error_text_size > 0) {
        snprintf(error_text, error_text_size, "Cloud login failed. Check e-mail and password.");
      }
      return ESP_ERR_INVALID_RESPONSE;
    }
    if (status_code == 412 && attempt == 0) {
      free(response_body);
      response_body = NULL;
      set_cloud_installation_registration(false);
      continue;
    }
    if (status_code < 200 || status_code >= 300) {
      free(response_body);
      set_cloud_connected(false);
      if (error_text != NULL && error_text_size > 0) {
        snprintf(error_text, error_text_size, "Cloud login failed with status %d.", status_code);
      }
      return ESP_FAIL;
    }

    ret = parse_cloud_access_token(response_body, access_token, access_token_size);
    free(response_body);
    set_cloud_connected(ret == ESP_OK);
    if (ret == ESP_OK) {
      store_cached_cloud_access_token(access_token);
    }
    if (ret != ESP_OK && error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud login response could not be parsed.");
    }
    return ret;
  }

  if (error_text != NULL && error_text_size > 0) {
    snprintf(error_text, error_text_size, "Cloud installation registration retry failed.");
  }
  set_cloud_connected(false);
  return ESP_FAIL;
}

esp_err_t lm_ctrl_cloud_session_fetch_access_token_cached(
  const char *username,
  const char *password,
  char *access_token,
  size_t access_token_size,
  char *error_text,
  size_t error_text_size
) {
  SemaphoreHandle_t auth_lock = NULL;
  esp_err_t ret;

  if (access_token == NULL || access_token_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (copy_cached_cloud_access_token(access_token, access_token_size)) {
    if (error_text != NULL && error_text_size > 0) {
      error_text[0] = '\0';
    }
    return ESP_OK;
  }

  lock_state();
  auth_lock = s_state.cloud_auth_lock;
  unlock_state();

  if (auth_lock != NULL) {
    xSemaphoreTake(auth_lock, portMAX_DELAY);
  }

  if (copy_cached_cloud_access_token(access_token, access_token_size)) {
    if (auth_lock != NULL) {
      xSemaphoreGive(auth_lock);
    }
    if (error_text != NULL && error_text_size > 0) {
      error_text[0] = '\0';
    }
    return ESP_OK;
  }

  ret = fetch_cloud_access_token(username, password, access_token, access_token_size, error_text, error_text_size);

  if (auth_lock != NULL) {
    xSemaphoreGive(auth_lock);
  }

  return ret;
}

esp_err_t lm_ctrl_cloud_auth_prepare_request_auth(
  const char *username,
  const char *password,
  lm_ctrl_cloud_request_auth_t *auth,
  char *error_text,
  size_t error_text_size
) {
  char access_token[LM_CTRL_CLOUD_WS_TOKEN_LEN];
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t private_key_der_len = 0;
  esp_err_t ret;

  if (auth == NULL || username == NULL || password == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(auth, 0, sizeof(*auth));
  ret = lm_ctrl_cloud_session_fetch_access_token_cached(
    username,
    password,
    access_token,
    sizeof(access_token),
    error_text,
    error_text_size
  );
  if (ret != ESP_OK) {
    return ret;
  }

  ret = ensure_cloud_installation();
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud installation data is missing.");
    }
    return ret;
  }

  lock_state();
  copy_text(auth->installation_id, sizeof(auth->installation_id), s_state.cloud_installation_id);
  memcpy(secret, s_state.cloud_secret, sizeof(secret));
  memcpy(private_key_der, s_state.cloud_private_key_der, s_state.cloud_private_key_der_len);
  private_key_der_len = s_state.cloud_private_key_der_len;
  unlock_state();

  ret = build_signed_request_headers(
    auth->installation_id,
    secret,
    private_key_der,
    private_key_der_len,
    auth->timestamp,
    sizeof(auth->timestamp),
    auth->nonce,
    sizeof(auth->nonce),
    auth->signature_b64,
    sizeof(auth->signature_b64)
  );
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud request signing failed.");
    }
    return ret;
  }

  snprintf(auth->auth_header, sizeof(auth->auth_header), "Bearer %s", access_token);
  return ESP_OK;
}

static void append_ws_header_line(char *buffer, size_t buffer_size, const char *name, const char *value) {
  if (buffer == NULL || buffer_size == 0 || name == NULL || value == NULL) {
    return;
  }
  if (buffer[0] != '\0') {
    append_text(buffer, buffer_size, "\r\n");
  }
  append_text(buffer, buffer_size, name);
  append_text(buffer, buffer_size, ": ");
  append_text(buffer, buffer_size, value);
}

esp_err_t lm_ctrl_cloud_session_build_websocket_headers(char *buffer, size_t buffer_size) {
  lm_ctrl_cloud_request_auth_t auth = {0};
  char username[96];
  char password[128];

  if (buffer == NULL || buffer_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  lock_state();
  copy_text(username, sizeof(username), s_state.cloud_username);
  copy_text(password, sizeof(password), s_state.cloud_password);
  unlock_state();

  buffer[0] = '\0';
  if (username[0] == '\0' || password[0] == '\0') {
    return ESP_ERR_INVALID_STATE;
  }

  {
    uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
    uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
    size_t private_key_der_len = 0;
    esp_err_t ret = ensure_cloud_installation();

    if (ret != ESP_OK) {
      return ret;
    }

    lock_state();
    copy_text(auth.installation_id, sizeof(auth.installation_id), s_state.cloud_installation_id);
    memcpy(secret, s_state.cloud_secret, sizeof(secret));
    memcpy(private_key_der, s_state.cloud_private_key_der, s_state.cloud_private_key_der_len);
    private_key_der_len = s_state.cloud_private_key_der_len;
    unlock_state();

    ret = build_signed_request_headers(
      auth.installation_id,
      secret,
      private_key_der,
      private_key_der_len,
      auth.timestamp,
      sizeof(auth.timestamp),
      auth.nonce,
      sizeof(auth.nonce),
      auth.signature_b64,
      sizeof(auth.signature_b64)
    );
    if (ret != ESP_OK) {
      return ret;
    }
  }

  append_ws_header_line(buffer, buffer_size, "X-App-Installation-Id", auth.installation_id);
  append_ws_header_line(buffer, buffer_size, "X-Timestamp", auth.timestamp);
  append_ws_header_line(buffer, buffer_size, "X-Nonce", auth.nonce);
  append_ws_header_line(buffer, buffer_size, "X-Request-Signature", auth.signature_b64);
  return ESP_OK;
}
