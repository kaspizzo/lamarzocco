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

static const char *TAG = "lm_cloud_api";

typedef struct {
  char *data;
  size_t len;
  size_t capacity;
  esp_err_t append_error;
} lm_ctrl_http_buffer_t;

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
  int *status_code
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
    count++;
  }

  cJSON_Delete(root);
  *machine_count = count;
  return count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
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
  cJSON *mode_item;
  cJSON *brewing_start_item;
  bool active = false;

  if (output == NULL || values == NULL || loaded_mask == NULL) {
    return false;
  }

  mode_item = cJSON_GetObjectItemCaseSensitive(output, "mode");
  brewing_start_item = cJSON_GetObjectItemCaseSensitive(output, "brewingStartTime");

  if (cJSON_IsString(mode_item) && mode_item->valuestring != NULL) {
    values->standby_on = strcmp(mode_item->valuestring, "StandBy") == 0;
    *loaded_mask |= LM_CTRL_MACHINE_FIELD_STANDBY;
    active = strcmp(mode_item->valuestring, "BrewingMode") == 0;
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

  return cJSON_IsString(mode_item) || cJSON_IsNumber(brewing_start_item);
}

static bool parse_dashboard_widget_values(
  cJSON *widget,
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
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
  bool *brew_active,
  int64_t *brew_start_epoch_ms
) {
  cJSON *widgets;
  cJSON *widget;
  ctrl_values_t local_values = {0};
  uint32_t local_loaded_mask = 0;
  uint32_t local_feature_mask = 0;
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
      brew_active != NULL ? &local_brew_active : NULL,
      brew_start_epoch_ms != NULL ? &local_brew_start_epoch_ms : NULL
    );
  }

  *values = local_values;
  *loaded_mask = local_loaded_mask;
  *feature_mask = local_feature_mask;
  if (brew_active != NULL) {
    *brew_active = local_brew_active;
  }
  if (brew_start_epoch_ms != NULL) {
    *brew_start_epoch_ms = local_brew_start_epoch_ms;
  }

  return (local_loaded_mask != 0 || local_feature_mask != 0 || local_brew_active || local_brew_start_epoch_ms != 0)
    ? ESP_OK
    : ESP_ERR_NOT_FOUND;
}
