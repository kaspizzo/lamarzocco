/**
 * Stateless helpers for La Marzocco cloud signing, HTTP transport, and dashboard parsing.
 *
 * The higher-level Wi-Fi setup module owns credentials, NVS, portal UI, and
 * background tasks. This module only provides reusable cloud request and parse
 * helpers so the controller can keep those concerns separate.
 */
#include "cloud_api.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/private/sha256.h"
#include "machine_link_types.h"
#include "psa/crypto.h"

static const char *TAG = "lm_cloud_api";

typedef struct {
  char *data;
  size_t len;
  size_t capacity;
  esp_err_t append_error;
  int64_t server_epoch_ms;
} lm_ctrl_http_buffer_t;

static int month_from_http_date(const char *month) {
  static const char *MONTHS[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };

  if (month == NULL) {
    return 0;
  }

  for (size_t i = 0; i < sizeof(MONTHS) / sizeof(MONTHS[0]); ++i) {
    if (strcmp(month, MONTHS[i]) == 0) {
      return (int)i + 1;
    }
  }

  return 0;
}

static int64_t days_from_civil(int year, unsigned int month, unsigned int day) {
  const int adjusted_year = year - (month <= 2U ? 1 : 0);
  const int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
  const unsigned int year_of_era = (unsigned int)(adjusted_year - era * 400);
  const int shifted_month = (int)month + (month > 2U ? -3 : 9);
  const unsigned int day_of_year = (153U * (unsigned int)shifted_month + 2U) / 5U + day - 1U;
  const unsigned int day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;

  return (int64_t)era * 146097LL + (int64_t)day_of_era - 719468LL;
}

static bool parse_http_date_epoch_ms(const char *value, int64_t *epoch_ms) {
  char weekday[4] = {0};
  char month_text[4] = {0};
  char timezone[4] = {0};
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int month = 0;
  int64_t days = 0;

  if (value == NULL || epoch_ms == NULL) {
    return false;
  }

  if (sscanf(
        value,
        "%3[^,], %d %3s %d %d:%d:%d %3s",
        weekday,
        &day,
        month_text,
        &year,
        &hour,
        &minute,
        &second,
        timezone
      ) != 8) {
    return false;
  }

  month = month_from_http_date(month_text);
  if (month == 0 ||
      strcmp(timezone, "GMT") != 0 ||
      day < 1 || day > 31 ||
      hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 ||
      second < 0 || second > 60) {
    return false;
  }

  days = days_from_civil(year, (unsigned int)month, (unsigned int)day);
  *epoch_ms = (days * 86400LL + (int64_t)hour * 3600LL + (int64_t)minute * 60LL + (int64_t)second) * 1000LL;
  return true;
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
  size_t len;

  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  len = strnlen(src, dst_size - 1);
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static void secure_zero(void *ptr, size_t len) {
  volatile uint8_t *cursor = (volatile uint8_t *)ptr;

  while (cursor != NULL && len > 0) {
    *cursor++ = 0;
    --len;
  }
}

static esp_err_t sha256_bytes(const uint8_t *data, size_t data_len, uint8_t output[32]) {
  if (data == NULL || output == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  return mbedtls_sha256(data, data_len, output, 0) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t base64_encode_bytes(const uint8_t *data, size_t data_len, char *output, size_t output_size) {
  size_t output_len = 0;
  int ret;

  if (data == NULL || output == NULL || output_size < 2) {
    return ESP_ERR_INVALID_ARG;
  }

  ret = mbedtls_base64_encode((unsigned char *)output, output_size - 1, &output_len, data, data_len);
  if (ret != 0) {
    output[0] = '\0';
    return ESP_FAIL;
  }

  output[output_len] = '\0';
  return ESP_OK;
}

static void format_uuid_v4(char buffer[37]) {
  uint8_t random_bytes[16] = {0};

  if (buffer == NULL) {
    return;
  }

  esp_fill_random(random_bytes, sizeof(random_bytes));
  random_bytes[6] = (uint8_t)((random_bytes[6] & 0x0FU) | 0x40U);
  random_bytes[8] = (uint8_t)((random_bytes[8] & 0x3FU) | 0x80U);
  snprintf(
    buffer,
    37,
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    random_bytes[0], random_bytes[1], random_bytes[2], random_bytes[3],
    random_bytes[4], random_bytes[5],
    random_bytes[6], random_bytes[7],
    random_bytes[8], random_bytes[9],
    random_bytes[10], random_bytes[11], random_bytes[12], random_bytes[13], random_bytes[14], random_bytes[15]
  );
  secure_zero(random_bytes, sizeof(random_bytes));
}

esp_err_t lm_ctrl_cloud_generate_installation_secret(
  const char *installation_id,
  const uint8_t *public_key_der,
  size_t public_key_der_len,
  uint8_t secret[32]
) {
  uint8_t installation_hash[32];
  char public_key_b64[256];
  char installation_hash_b64[64];
  char secret_input[384];

  if (installation_id == NULL ||
      installation_id[0] == '\0' ||
      public_key_der == NULL ||
      public_key_der_len == 0 ||
      secret == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (sha256_bytes((const uint8_t *)installation_id, strlen(installation_id), installation_hash) != ESP_OK) {
    return ESP_FAIL;
  }
  if (base64_encode_bytes(public_key_der, public_key_der_len, public_key_b64, sizeof(public_key_b64)) != ESP_OK ||
      base64_encode_bytes(installation_hash, sizeof(installation_hash), installation_hash_b64, sizeof(installation_hash_b64)) != ESP_OK) {
    secure_zero(installation_hash, sizeof(installation_hash));
    secure_zero(public_key_b64, sizeof(public_key_b64));
    secure_zero(installation_hash_b64, sizeof(installation_hash_b64));
    return ESP_FAIL;
  }

  snprintf(
    secret_input,
    sizeof(secret_input),
    "%s.%s.%s",
    installation_id,
    public_key_b64,
    installation_hash_b64
  );
  if (sha256_bytes((const uint8_t *)secret_input, strlen(secret_input), secret) != ESP_OK) {
    secure_zero(installation_hash, sizeof(installation_hash));
    secure_zero(public_key_b64, sizeof(public_key_b64));
    secure_zero(installation_hash_b64, sizeof(installation_hash_b64));
    secure_zero(secret_input, sizeof(secret_input));
    return ESP_FAIL;
  }

  secure_zero(installation_hash, sizeof(installation_hash));
  secure_zero(public_key_b64, sizeof(public_key_b64));
  secure_zero(installation_hash_b64, sizeof(installation_hash_b64));
  secure_zero(secret_input, sizeof(secret_input));
  return ESP_OK;
}

esp_err_t lm_ctrl_cloud_generate_installation(lm_ctrl_cloud_installation_t *installation) {
  mbedtls_pk_context pk;
  psa_key_attributes_t key_attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t key_id = 0;
  unsigned char private_key_der[sizeof(installation->private_key_der)];
  unsigned char public_key_der[160];
  const unsigned char *private_key_ptr;
  const unsigned char *public_key_ptr;
  int private_key_der_len = 0;
  int public_key_der_len = 0;
  psa_status_t psa_status = PSA_SUCCESS;
  esp_err_t ret = ESP_FAIL;

  if (installation == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(installation, 0, sizeof(*installation));
  format_uuid_v4(installation->installation_id);

  psa_status = psa_crypto_init();
  if (psa_status != PSA_SUCCESS) {
    ret = ESP_FAIL;
    goto exit;
  }

  mbedtls_pk_init(&pk);
  psa_set_key_usage_flags(&key_attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
  psa_set_key_algorithm(&key_attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
  psa_set_key_type(&key_attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&key_attributes, 256);

  psa_status = psa_generate_key(&key_attributes, &key_id);
  if (psa_status != PSA_SUCCESS) {
    ret = ESP_FAIL;
    goto exit;
  }
  if (mbedtls_pk_wrap_psa(&pk, key_id) != 0) {
    ret = ESP_FAIL;
    goto exit;
  }

  private_key_der_len = mbedtls_pk_write_key_der(&pk, private_key_der, sizeof(private_key_der));
  if (private_key_der_len <= 0 || (size_t)private_key_der_len > sizeof(private_key_der)) {
    ret = ESP_FAIL;
    goto exit;
  }
  private_key_ptr = private_key_der + sizeof(private_key_der) - (size_t)private_key_der_len;
  memcpy(installation->private_key_der, private_key_ptr, (size_t)private_key_der_len);
  installation->private_key_der_len = (size_t)private_key_der_len;

  public_key_der_len = mbedtls_pk_write_pubkey_der(&pk, public_key_der, sizeof(public_key_der));
  if (public_key_der_len <= 0 || (size_t)public_key_der_len > sizeof(public_key_der)) {
    ret = ESP_FAIL;
    goto exit;
  }
  public_key_ptr = public_key_der + sizeof(public_key_der) - (size_t)public_key_der_len;
  ret = lm_ctrl_cloud_generate_installation_secret(
    installation->installation_id,
    public_key_ptr,
    (size_t)public_key_der_len,
    installation->secret
  );

exit:
  psa_reset_key_attributes(&key_attributes);
  mbedtls_pk_free(&pk);
  if (key_id != 0) {
    (void)psa_destroy_key(key_id);
  }
  secure_zero(private_key_der, sizeof(private_key_der));
  secure_zero(public_key_der, sizeof(public_key_der));
  if (ret != ESP_OK) {
    secure_zero(installation, sizeof(*installation));
  }
  return ret;
}

static esp_err_t http_buffer_append(lm_ctrl_http_buffer_t *buffer, const char *data, size_t data_len) {
  char *new_data;
  size_t required;

  if (buffer == NULL || data == NULL || data_len == 0) {
    return ESP_OK;
  }

  required = buffer->len + data_len + 1;
  if (required > buffer->capacity) {
    size_t new_capacity = buffer->capacity == 0 ? 512 : buffer->capacity;

    while (new_capacity < required) {
      new_capacity *= 2;
    }

    new_data = realloc(buffer->data, new_capacity);
    if (new_data == NULL) {
      buffer->append_error = ESP_ERR_NO_MEM;
      return ESP_ERR_NO_MEM;
    }

    buffer->data = new_data;
    buffer->capacity = new_capacity;
  }

  memcpy(buffer->data + buffer->len, data, data_len);
  buffer->len += data_len;
  buffer->data[buffer->len] = '\0';
  return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event) {
  lm_ctrl_http_buffer_t *buffer = event != NULL ? (lm_ctrl_http_buffer_t *)event->user_data : NULL;

  if (buffer == NULL) {
    return ESP_OK;
  }

  if (buffer->append_error != ESP_OK) {
    return buffer->append_error;
  }

  if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL && event->data_len > 0) {
    return http_buffer_append(buffer, (const char *)event->data, event->data_len);
  }
  if (event->event_id == HTTP_EVENT_ON_HEADER &&
      event->header_key != NULL &&
      event->header_value != NULL &&
      strcasecmp(event->header_key, "Date") == 0) {
    int64_t server_epoch_ms = 0;

    if (parse_http_date_epoch_ms(event->header_value, &server_epoch_ms)) {
      buffer->server_epoch_ms = server_epoch_ms;
    }
  }

  return ESP_OK;
}

esp_err_t lm_ctrl_cloud_derive_installation_material(
  const char *installation_id,
  const uint8_t *private_key_der,
  size_t private_key_der_len,
  char *public_key_b64,
  size_t public_key_b64_size,
  char *base_string,
  size_t base_string_size
) {
  mbedtls_pk_context pk;
  unsigned char public_key_der[160];
  uint8_t public_key_hash[32];
  char public_key_hash_b64[64];
  int public_key_der_len;
  const unsigned char *public_key_ptr;
  esp_err_t err = ESP_OK;

  if (
    installation_id == NULL ||
    private_key_der == NULL ||
    private_key_der_len == 0 ||
    public_key_b64 == NULL ||
    base_string == NULL
  ) {
    return ESP_ERR_INVALID_ARG;
  }

  mbedtls_pk_init(&pk);
  if (mbedtls_pk_parse_key(&pk, private_key_der, private_key_der_len, NULL, 0) != 0) {
    mbedtls_pk_free(&pk);
    return ESP_FAIL;
  }

  public_key_der_len = mbedtls_pk_write_pubkey_der(&pk, public_key_der, sizeof(public_key_der));
  if (public_key_der_len <= 0 || (size_t)public_key_der_len > sizeof(public_key_der)) {
    err = ESP_FAIL;
    goto exit;
  }

  public_key_ptr = public_key_der + sizeof(public_key_der) - (size_t)public_key_der_len;
  if (
    sha256_bytes(public_key_ptr, (size_t)public_key_der_len, public_key_hash) != ESP_OK ||
    base64_encode_bytes(public_key_hash, sizeof(public_key_hash), public_key_hash_b64, sizeof(public_key_hash_b64)) != ESP_OK ||
    base64_encode_bytes(public_key_ptr, (size_t)public_key_der_len, public_key_b64, public_key_b64_size) != ESP_OK
  ) {
    err = ESP_FAIL;
    goto exit;
  }

  snprintf(base_string, base_string_size, "%s.%s", installation_id, public_key_hash_b64);

exit:
  mbedtls_pk_free(&pk);
  return err;
}

esp_err_t lm_ctrl_cloud_generate_request_proof_text(
  const char *base_string,
  const uint8_t secret[32],
  char *proof,
  size_t proof_size
) {
  uint8_t work[32];
  uint8_t proof_hash[32];

  if (base_string == NULL || secret == NULL || proof == NULL || proof_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  memcpy(work, secret, sizeof(work));
  for (size_t i = 0; base_string[i] != '\0'; ++i) {
    uint8_t byte_value = (uint8_t)base_string[i];
    size_t idx = byte_value % sizeof(work);
    size_t shift_idx = (idx + 1U) % sizeof(work);
    uint8_t shift_amount = work[shift_idx] & 0x07U;
    uint8_t xor_result = (uint8_t)(byte_value ^ work[idx]);
    uint8_t rotated = (uint8_t)(((unsigned int)xor_result << shift_amount) | ((unsigned int)xor_result >> (8U - shift_amount)));
    work[idx] = rotated;
  }

  if (sha256_bytes(work, sizeof(work), proof_hash) != ESP_OK) {
    return ESP_FAIL;
  }

  return base64_encode_bytes(proof_hash, sizeof(proof_hash), proof, proof_size);
}

esp_err_t lm_ctrl_cloud_http_request(
  const char *host,
  const char *path,
  int port,
  int method,
  const lm_ctrl_cloud_http_header_t *headers,
  size_t header_count,
  const char *body,
  int timeout_ms,
  char **response_body,
  int *status_code,
  lm_ctrl_cloud_http_response_meta_t *response_meta
) {
  esp_http_client_config_t config = {
    .host = host,
    .path = path,
    .port = port,
    .transport_type = HTTP_TRANSPORT_OVER_SSL,
    .method = method,
    .timeout_ms = timeout_ms,
    .event_handler = http_event_handler,
    .crt_bundle_attach = esp_crt_bundle_attach,
    .addr_type = HTTP_ADDR_TYPE_INET,
    .buffer_size = 4096,
    .buffer_size_tx = 2048,
  };
  esp_http_client_handle_t client;
  lm_ctrl_http_buffer_t buffer = {0};
  esp_err_t ret;

  if (host == NULL || path == NULL || response_body == NULL || status_code == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *response_body = NULL;
  *status_code = 0;
  if (response_meta != NULL) {
    *response_meta = (lm_ctrl_cloud_http_response_meta_t){0};
  }
  config.user_data = &buffer;

  client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "HTTP client init failed for %s%s", host, path);
    return ESP_FAIL;
  }

  if (headers != NULL) {
    for (size_t i = 0; i < header_count; ++i) {
      if (headers[i].name != NULL && headers[i].value != NULL) {
        esp_http_client_set_header(client, headers[i].name, headers[i].value);
      }
    }
  }
  if (body != NULL) {
    esp_http_client_set_post_field(client, body, strlen(body));
  }

  ret = esp_http_client_perform(client);
  *status_code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (ret != ESP_OK) {
    free(buffer.data);
    return ret;
  }
  if (buffer.append_error != ESP_OK) {
    free(buffer.data);
    return buffer.append_error;
  }
  if (buffer.data == NULL) {
    buffer.data = calloc(1, 1);
    if (buffer.data == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }

  if (response_meta != NULL) {
    response_meta->server_epoch_ms = buffer.server_epoch_ms;
  }
  *response_body = buffer.data;
  return ESP_OK;
}

esp_err_t lm_ctrl_cloud_build_signed_request_headers(
  const lm_ctrl_cloud_installation_t *installation,
  char *timestamp,
  size_t timestamp_size,
  char *nonce,
  size_t nonce_size,
  char *signature_b64,
  size_t signature_b64_size
) {
  char proof_input[160];
  char request_proof[64];
  char signature_input[256];
  uint8_t signature_hash[32];
  unsigned char signature_der[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
  size_t signature_der_len = 0;
  mbedtls_pk_context pk;
  esp_err_t err = ESP_OK;

  if (
    installation == NULL ||
    installation->installation_id[0] == '\0' ||
    installation->private_key_der_len == 0 ||
    timestamp == NULL ||
    nonce == NULL ||
    signature_b64 == NULL
  ) {
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t random_bytes[16];
  esp_fill_random(random_bytes, sizeof(random_bytes));
  random_bytes[6] = (random_bytes[6] & 0x0F) | 0x40;
  random_bytes[8] = (random_bytes[8] & 0x3F) | 0x80;
  snprintf(
    nonce,
    nonce_size,
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    random_bytes[0], random_bytes[1], random_bytes[2], random_bytes[3],
    random_bytes[4], random_bytes[5],
    random_bytes[6], random_bytes[7],
    random_bytes[8], random_bytes[9],
    random_bytes[10], random_bytes[11], random_bytes[12], random_bytes[13], random_bytes[14], random_bytes[15]
  );
  snprintf(timestamp, timestamp_size, "%llu", (unsigned long long)(esp_timer_get_time() / 1000ULL));
  snprintf(proof_input, sizeof(proof_input), "%s.%s.%s", installation->installation_id, nonce, timestamp);
  err = lm_ctrl_cloud_generate_request_proof_text(proof_input, installation->secret, request_proof, sizeof(request_proof));
  if (err != ESP_OK) {
    return err;
  }

  snprintf(signature_input, sizeof(signature_input), "%s.%s", proof_input, request_proof);
  err = sha256_bytes((const uint8_t *)signature_input, strlen(signature_input), signature_hash);
  if (err != ESP_OK) {
    return err;
  }

  mbedtls_pk_init(&pk);
  if (mbedtls_pk_parse_key(&pk, installation->private_key_der, installation->private_key_der_len, NULL, 0) != 0) {
    mbedtls_pk_free(&pk);
    return ESP_FAIL;
  }
  if (mbedtls_pk_sign(
        &pk,
        MBEDTLS_MD_SHA256,
        signature_hash,
        sizeof(signature_hash),
        signature_der,
        sizeof(signature_der),
        &signature_der_len
      ) != 0) {
    mbedtls_pk_free(&pk);
    return ESP_FAIL;
  }
  mbedtls_pk_free(&pk);

  return base64_encode_bytes(signature_der, signature_der_len, signature_b64, signature_b64_size);
}

esp_err_t lm_ctrl_cloud_parse_access_token(
  const char *response_body,
  char *access_token,
  size_t access_token_size
) {
  cJSON *root;
  cJSON *token_item;

  if (response_body == NULL || access_token == NULL || access_token_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  root = cJSON_Parse(response_body);
  if (root == NULL) {
    return ESP_ERR_INVALID_RESPONSE;
  }

  token_item = cJSON_GetObjectItemCaseSensitive(root, "accessToken");
  if (!cJSON_IsString(token_item) || token_item->valuestring == NULL) {
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }

  copy_text(access_token, access_token_size, token_item->valuestring);
  cJSON_Delete(root);
  return ESP_OK;
}

static bool parse_machine_status_fields(cJSON *root, lm_ctrl_cloud_machine_status_t *status) {
  cJSON *connected_item;
  cJSON *offline_mode_item;

  if (!cJSON_IsObject(root) || status == NULL) {
    return false;
  }

  memset(status, 0, sizeof(*status));
  connected_item = cJSON_GetObjectItemCaseSensitive(root, "connected");
  offline_mode_item = cJSON_GetObjectItemCaseSensitive(root, "offlineMode");
  if (cJSON_IsBool(connected_item)) {
    status->connected_known = true;
    status->connected = cJSON_IsTrue(connected_item);
  }
  if (cJSON_IsBool(offline_mode_item)) {
    status->offline_mode_known = true;
    status->offline_mode = cJSON_IsTrue(offline_mode_item);
  }

  return lm_ctrl_cloud_machine_status_is_known(status);
}

esp_err_t lm_ctrl_cloud_parse_customer_fleet(
  const char *response_body,
  lm_ctrl_cloud_machine_t *machines,
  size_t max_machines,
  size_t *machine_count
) {
  cJSON *root;
  cJSON *entry;
  size_t count = 0;

  if (response_body == NULL || machines == NULL || machine_count == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *machine_count = 0;
  root = cJSON_Parse(response_body);
  if (root == NULL) {
    return ESP_ERR_INVALID_RESPONSE;
  }
  if (!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }

  cJSON_ArrayForEach(entry, root) {
    cJSON *serial_item;
    cJSON *name_item;
    cJSON *model_item;
    cJSON *ble_token_item;
    lm_ctrl_cloud_machine_status_t machine_status = {0};

    if (count >= max_machines) {
      break;
    }

    serial_item = cJSON_GetObjectItemCaseSensitive(entry, "serialNumber");
    if (!cJSON_IsString(serial_item) || serial_item->valuestring == NULL) {
      continue;
    }

    name_item = cJSON_GetObjectItemCaseSensitive(entry, "name");
    model_item = cJSON_GetObjectItemCaseSensitive(entry, "modelName");
    ble_token_item = cJSON_GetObjectItemCaseSensitive(entry, "bleAuthToken");

    memset(&machines[count], 0, sizeof(machines[count]));
    copy_text(machines[count].serial, sizeof(machines[count].serial), serial_item->valuestring);
    if (cJSON_IsString(name_item) && name_item->valuestring != NULL) {
      copy_text(machines[count].name, sizeof(machines[count].name), name_item->valuestring);
    }
    if (cJSON_IsString(model_item) && model_item->valuestring != NULL) {
      copy_text(machines[count].model, sizeof(machines[count].model), model_item->valuestring);
    }
    if (cJSON_IsString(ble_token_item) && ble_token_item->valuestring != NULL) {
      copy_text(machines[count].communication_key, sizeof(machines[count].communication_key), ble_token_item->valuestring);
    }
    if (parse_machine_status_fields(entry, &machine_status)) {
      machines[count].cloud_status = machine_status;
    }
    count++;
  }

  cJSON_Delete(root);
  *machine_count = count;
  return count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool lm_ctrl_cloud_parse_dashboard_machine_status(
  cJSON *root,
  lm_ctrl_cloud_machine_status_t *status
) {
  return parse_machine_status_fields(root, status);
}

bool lm_ctrl_cloud_parse_dashboard_water_status(
  cJSON *root,
  lm_ctrl_machine_water_status_t *status
) {
  cJSON *widgets;
  cJSON *widget;

  if (status == NULL) {
    return false;
  }

  *status = (lm_ctrl_machine_water_status_t){0};
  if (root == NULL) {
    return false;
  }

  widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
  if (!cJSON_IsArray(widgets)) {
    return false;
  }

  cJSON_ArrayForEach(widget, widgets) {
    cJSON *code_item;
    cJSON *output;
    cJSON *alarm_item;

    code_item = cJSON_GetObjectItemCaseSensitive(widget, "code");
    output = cJSON_GetObjectItemCaseSensitive(widget, "output");
    if (!cJSON_IsString(code_item) ||
        code_item->valuestring == NULL ||
        strcmp(code_item->valuestring, "CMNoWater") != 0 ||
        !cJSON_IsObject(output)) {
      continue;
    }

    alarm_item = cJSON_GetObjectItemCaseSensitive(output, "allarm");
    if (!cJSON_IsBool(alarm_item)) {
      alarm_item = cJSON_GetObjectItemCaseSensitive(output, "alarm");
    }
    if (!cJSON_IsBool(alarm_item)) {
      continue;
    }

    status->available = true;
    status->no_water = cJSON_IsTrue(alarm_item);
    return true;
  }

  return false;
}

static bool parse_prebrew_widget_values(cJSON *widget, float *seconds_in, float *seconds_out) {
  cJSON *code_item;
  cJSON *output;

  if (widget == NULL || seconds_in == NULL || seconds_out == NULL) {
    return false;
  }

  code_item = cJSON_GetObjectItemCaseSensitive(widget, "code");
  output = cJSON_GetObjectItemCaseSensitive(widget, "output");
  if (!cJSON_IsString(code_item) || code_item->valuestring == NULL || !cJSON_IsObject(output)) {
    return false;
  }

  if (strcmp(code_item->valuestring, "CMPreBrewing") == 0) {
    cJSON *times = cJSON_GetObjectItemCaseSensitive(output, "times");
    cJSON *prebrewing = times != NULL ? cJSON_GetObjectItemCaseSensitive(times, "PreBrewing") : NULL;
    cJSON *entry = cJSON_IsArray(prebrewing) ? cJSON_GetArrayItem(prebrewing, 0) : NULL;
    cJSON *seconds = entry != NULL ? cJSON_GetObjectItemCaseSensitive(entry, "seconds") : NULL;
    cJSON *in_item = seconds != NULL ? cJSON_GetObjectItemCaseSensitive(seconds, "In") : NULL;
    cJSON *out_item = seconds != NULL ? cJSON_GetObjectItemCaseSensitive(seconds, "Out") : NULL;

    if (cJSON_IsNumber(in_item) && cJSON_IsNumber(out_item)) {
      *seconds_in = (float)in_item->valuedouble;
      *seconds_out = (float)out_item->valuedouble;
      return true;
    }
  }

  if (strcmp(code_item->valuestring, "CMPreExtraction") == 0) {
    cJSON *in_obj = cJSON_GetObjectItemCaseSensitive(output, "In");
    cJSON *out_obj = cJSON_GetObjectItemCaseSensitive(output, "Out");
    cJSON *in_seconds = in_obj != NULL ? cJSON_GetObjectItemCaseSensitive(in_obj, "seconds") : NULL;
    cJSON *out_seconds = out_obj != NULL ? cJSON_GetObjectItemCaseSensitive(out_obj, "seconds") : NULL;
    cJSON *in_prebrew = in_seconds != NULL ? cJSON_GetObjectItemCaseSensitive(in_seconds, "PreBrewing") : NULL;
    cJSON *out_prebrew = out_seconds != NULL ? cJSON_GetObjectItemCaseSensitive(out_seconds, "PreBrewing") : NULL;

    if (cJSON_IsNumber(in_prebrew) && cJSON_IsNumber(out_prebrew)) {
      *seconds_in = (float)in_prebrew->valuedouble;
      *seconds_out = (float)out_prebrew->valuedouble;
      return true;
    }
  }

  return false;
}

bool lm_ctrl_cloud_parse_prebrew_widget_values(cJSON *widget, float *seconds_in, float *seconds_out) {
  return parse_prebrew_widget_values(widget, seconds_in, seconds_out);
}

static bool parse_bbw_widget_values(cJSON *widget, ctrl_values_t *values, uint32_t *loaded_mask, uint32_t *feature_mask) {
  cJSON *code_item;
  cJSON *output;
  cJSON *mode_item;
  cJSON *doses;
  cJSON *dose_1;
  cJSON *dose_2;
  cJSON *dose_1_value;
  cJSON *dose_2_value;
  bool updated = false;

  if (widget == NULL || values == NULL || loaded_mask == NULL || feature_mask == NULL) {
    return false;
  }

  code_item = cJSON_GetObjectItemCaseSensitive(widget, "code");
  output = cJSON_GetObjectItemCaseSensitive(widget, "output");
  if (!cJSON_IsString(code_item) || code_item->valuestring == NULL || !cJSON_IsObject(output)) {
    return false;
  }

  if (strcmp(code_item->valuestring, "CMBrewByWeightDoses") != 0) {
    return false;
  }

  *feature_mask |= LM_CTRL_MACHINE_FEATURE_BBW;
  mode_item = cJSON_GetObjectItemCaseSensitive(output, "mode");
  if (cJSON_IsString(mode_item) && mode_item->valuestring != NULL) {
    values->bbw_mode = ctrl_bbw_mode_from_cloud_code(mode_item->valuestring);
    *loaded_mask |= LM_CTRL_MACHINE_FIELD_BBW_MODE;
    updated = true;
  }

  doses = cJSON_GetObjectItemCaseSensitive(output, "doses");
  dose_1 = doses != NULL ? cJSON_GetObjectItemCaseSensitive(doses, "Dose1") : NULL;
  dose_2 = doses != NULL ? cJSON_GetObjectItemCaseSensitive(doses, "Dose2") : NULL;
  dose_1_value = dose_1 != NULL ? cJSON_GetObjectItemCaseSensitive(dose_1, "dose") : NULL;
  dose_2_value = dose_2 != NULL ? cJSON_GetObjectItemCaseSensitive(dose_2, "dose") : NULL;
  if (cJSON_IsNumber(dose_1_value)) {
    values->bbw_dose_1_g = (float)dose_1_value->valuedouble;
    *loaded_mask |= LM_CTRL_MACHINE_FIELD_BBW_DOSE_1;
    updated = true;
  }
  if (cJSON_IsNumber(dose_2_value)) {
    values->bbw_dose_2_g = (float)dose_2_value->valuedouble;
    *loaded_mask |= LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
    updated = true;
  }

  return updated;
}

static bool parse_machine_status_widget(
  cJSON *output,
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  bool *brew_active,
  int64_t *brew_start_epoch_ms
) {
  cJSON *status_item;
  cJSON *mode_item;
  cJSON *brewing_start_item;
  bool active = false;

  if (output == NULL || values == NULL || loaded_mask == NULL) {
    return false;
  }

  status_item = cJSON_GetObjectItemCaseSensitive(output, "status");
  mode_item = cJSON_GetObjectItemCaseSensitive(output, "mode");
  brewing_start_item = cJSON_GetObjectItemCaseSensitive(output, "brewingStartTime");

  if (cJSON_IsString(mode_item) && mode_item->valuestring != NULL) {
    values->standby_on = strcmp(mode_item->valuestring, "StandBy") == 0;
    *loaded_mask |= LM_CTRL_MACHINE_FIELD_STANDBY;
  }
  if (cJSON_IsString(status_item) && status_item->valuestring != NULL) {
    active = strcmp(status_item->valuestring, "Brewing") == 0;
  }
  if (cJSON_IsNumber(brewing_start_item) && brewing_start_item->valuedouble > 0) {
    active = true;
    if (brew_start_epoch_ms != NULL) {
      *brew_start_epoch_ms = (int64_t)brewing_start_item->valuedouble;
    }
  } else if (brew_start_epoch_ms != NULL && !active) {
    *brew_start_epoch_ms = 0;
  }
  if (brew_active != NULL) {
    *brew_active = active;
  }

  return cJSON_IsString(status_item) || cJSON_IsString(mode_item) || cJSON_IsNumber(brewing_start_item);
}

static bool parse_epoch_ms_item(cJSON *item, int64_t *epoch_ms) {
  char *end = NULL;
  long long parsed = 0;

  if (item == NULL || epoch_ms == NULL) {
    return false;
  }

  if (cJSON_IsNumber(item) && item->valuedouble > 0.0) {
    *epoch_ms = (int64_t)item->valuedouble;
    return true;
  }
  if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
    return false;
  }

  parsed = strtoll(item->valuestring, &end, 10);
  if (end == item->valuestring || (end != NULL && *end != '\0') || parsed <= 0) {
    return false;
  }

  *epoch_ms = (int64_t)parsed;
  return true;
}

static void update_heat_info_from_widget(
  const char *code,
  cJSON *output,
  lm_ctrl_machine_heat_info_t *heat_info
) {
  cJSON *status_item;
  cJSON *ready_start_item;
  int64_t ready_epoch_ms = 0;
  bool is_coffee = false;
  bool is_steam = false;

  if (code == NULL || output == NULL || heat_info == NULL) {
    return;
  }
  is_coffee = strcmp(code, "CMCoffeeBoiler") == 0;
  is_steam = strcmp(code, "CMSteamBoilerLevel") == 0 || strcmp(code, "CMSteamBoilerTemperature") == 0;
  if (!is_coffee && !is_steam) {
    return;
  }

  heat_info->available = true;
  status_item = cJSON_GetObjectItemCaseSensitive(output, "status");
  if (!cJSON_IsString(status_item) ||
      status_item->valuestring == NULL ||
      strcmp(status_item->valuestring, "HeatingUp") != 0) {
    return;
  }

  if (is_coffee) {
    heat_info->coffee_heating = true;
  }
  if (is_steam) {
    heat_info->steam_heating = true;
  }
  heat_info->heating = heat_info->coffee_heating || heat_info->steam_heating;
  ready_start_item = cJSON_GetObjectItemCaseSensitive(output, "readyStartTime");
  if (parse_epoch_ms_item(ready_start_item, &ready_epoch_ms)) {
    if (is_coffee) {
      heat_info->coffee_eta_available = true;
      if (ready_epoch_ms > heat_info->coffee_ready_epoch_ms) {
        heat_info->coffee_ready_epoch_ms = ready_epoch_ms;
      }
    }
    if (is_steam) {
      heat_info->steam_eta_available = true;
      if (ready_epoch_ms > heat_info->steam_ready_epoch_ms) {
        heat_info->steam_ready_epoch_ms = ready_epoch_ms;
      }
    }
    heat_info->eta_available = heat_info->coffee_eta_available || heat_info->steam_eta_available;
    if (ready_epoch_ms > heat_info->ready_epoch_ms) {
      heat_info->ready_epoch_ms = ready_epoch_ms;
    }
  }
}

static bool parse_dashboard_widget_values(
  cJSON *widget,
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  lm_ctrl_machine_heat_info_t *heat_info,
  bool *brew_active,
  int64_t *brew_start_epoch_ms
) {
  cJSON *code_item;
  cJSON *output;

  if (widget == NULL || values == NULL || loaded_mask == NULL || feature_mask == NULL) {
    return false;
  }

  code_item = cJSON_GetObjectItemCaseSensitive(widget, "code");
  output = cJSON_GetObjectItemCaseSensitive(widget, "output");
  if (!cJSON_IsString(code_item) || code_item->valuestring == NULL || !cJSON_IsObject(output)) {
    return false;
  }

  update_heat_info_from_widget(code_item->valuestring, output, heat_info);

  if (strcmp(code_item->valuestring, "CMMachineStatus") == 0) {
    return parse_machine_status_widget(output, values, loaded_mask, brew_active, brew_start_epoch_ms);
  }

  if (strcmp(code_item->valuestring, "CMCoffeeBoiler") == 0) {
    cJSON *target_temperature = cJSON_GetObjectItemCaseSensitive(output, "targetTemperature");

    if (cJSON_IsNumber(target_temperature)) {
      values->temperature_c = (float)target_temperature->valuedouble;
      *loaded_mask |= LM_CTRL_MACHINE_FIELD_TEMPERATURE;
      return true;
    }
  }

  if (strcmp(code_item->valuestring, "CMSteamBoilerLevel") == 0 ||
      strcmp(code_item->valuestring, "CMSteamBoilerTemperature") == 0) {
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(output, "enabled");
    cJSON *target_level = cJSON_GetObjectItemCaseSensitive(output, "targetLevel");
    ctrl_steam_level_t steam_level = values->steam_level;

    if (cJSON_IsBool(enabled)) {
      if (!cJSON_IsTrue(enabled)) {
        values->steam_level = CTRL_STEAM_LEVEL_OFF;
      } else if (cJSON_IsString(target_level) &&
                 target_level->valuestring != NULL &&
                 ctrl_steam_level_from_cloud_code(target_level->valuestring, &steam_level)) {
        values->steam_level = steam_level;
      } else if (!ctrl_steam_level_enabled(steam_level)) {
        values->steam_level = CTRL_STEAM_LEVEL_2;
      }
      *loaded_mask |= LM_CTRL_MACHINE_FIELD_STEAM;
      return true;
    }
  }

  if (parse_prebrew_widget_values(widget, &values->infuse_s, &values->pause_s)) {
    *loaded_mask |= LM_CTRL_MACHINE_FIELD_PREBREWING;
    return true;
  }

  return parse_bbw_widget_values(widget, values, loaded_mask, feature_mask);
}

esp_err_t lm_ctrl_cloud_parse_dashboard_root_values(
  cJSON *root,
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  lm_ctrl_machine_heat_info_t *heat_info,
  bool *brew_active,
  int64_t *brew_start_epoch_ms
) {
  cJSON *widgets;
  cJSON *widget;
  ctrl_values_t local_values = {0};
  uint32_t local_loaded_mask = 0;
  uint32_t local_feature_mask = 0;
  lm_ctrl_machine_heat_info_t local_heat_info = {0};
  bool local_brew_active = false;
  int64_t local_brew_start_epoch_ms = 0;

  if (root == NULL || values == NULL || loaded_mask == NULL || feature_mask == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  local_values = *values;

  widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
  if (!cJSON_IsArray(widgets)) {
    return ESP_FAIL;
  }

  cJSON_ArrayForEach(widget, widgets) {
    (void)parse_dashboard_widget_values(
      widget,
      &local_values,
      &local_loaded_mask,
      &local_feature_mask,
      heat_info != NULL ? &local_heat_info : NULL,
      brew_active != NULL ? &local_brew_active : NULL,
      brew_start_epoch_ms != NULL ? &local_brew_start_epoch_ms : NULL
    );
  }

  *values = local_values;
  *loaded_mask = local_loaded_mask;
  *feature_mask = local_feature_mask;
  if (heat_info != NULL) {
    *heat_info = local_heat_info;
  }
  if (brew_active != NULL) {
    *brew_active = local_brew_active;
  }
  if (brew_start_epoch_ms != NULL) {
    *brew_start_epoch_ms = local_brew_start_epoch_ms;
  }

  return (local_loaded_mask != 0 ||
          local_feature_mask != 0 ||
          local_heat_info.available ||
          local_brew_active ||
          local_brew_start_epoch_ms != 0)
    ? ESP_OK
    : ESP_ERR_NOT_FOUND;
}
