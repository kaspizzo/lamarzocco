#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cloud_machine_status.h"
#include "controller_state.h"
#include "machine_link_types.h"

typedef struct cJSON cJSON;

/** Simple name/value HTTP header pair used by the internal cloud client. */
typedef struct {
  const char *name;
  const char *value;
} lm_ctrl_cloud_http_header_t;

/** Optional metadata captured from an HTTP response. */
typedef struct {
  int64_t server_epoch_ms;
} lm_ctrl_cloud_http_response_meta_t;

/** Machine metadata returned by the La Marzocco customer fleet endpoint. */
typedef struct {
  char serial[32];
  char name[64];
  char model[32];
  char communication_key[128];
  lm_ctrl_cloud_machine_status_t cloud_status;
} lm_ctrl_cloud_machine_t;

/** Installation credentials needed to sign La Marzocco cloud requests. */
typedef struct {
  char installation_id[37];
  uint8_t secret[32];
  uint8_t private_key_der[256];
  size_t private_key_der_len;
} lm_ctrl_cloud_installation_t;

/** Build a public-key registration payload from the stored installation key material. */
esp_err_t lm_ctrl_cloud_derive_installation_material(
  const char *installation_id,
  const uint8_t *private_key_der,
  size_t private_key_der_len,
  char *public_key_b64,
  size_t public_key_b64_size,
  char *base_string,
  size_t base_string_size
);

/** Derive the 32-byte installation secret from installation id and public key DER bytes. */
esp_err_t lm_ctrl_cloud_generate_installation_secret(
  const char *installation_id,
  const uint8_t *public_key_der,
  size_t public_key_der_len,
  uint8_t secret[32]
);

/** Generate per-device cloud installation key material compatible with the pylamarzocco flow. */
esp_err_t lm_ctrl_cloud_generate_installation(lm_ctrl_cloud_installation_t *installation);

/** Generate the request proof text used during cloud installation registration. */
esp_err_t lm_ctrl_cloud_generate_request_proof_text(
  const char *base_string,
  const uint8_t secret[32],
  char *proof,
  size_t proof_size
);

/** Execute a TLS HTTP request and capture the full response body. */
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
);

/** Build the signed request headers required by the current cloud API. */
esp_err_t lm_ctrl_cloud_build_signed_request_headers(
  const lm_ctrl_cloud_installation_t *installation,
  char *timestamp,
  size_t timestamp_size,
  char *nonce,
  size_t nonce_size,
  char *signature_b64,
  size_t signature_b64_size
);

/** Parse the access token returned from the cloud sign-in response. */
esp_err_t lm_ctrl_cloud_parse_access_token(
  const char *response_body,
  char *access_token,
  size_t access_token_size
);

/** Parse the customer fleet payload returned from the cloud things endpoint. */
esp_err_t lm_ctrl_cloud_parse_customer_fleet(
  const char *response_body,
  lm_ctrl_cloud_machine_t *machines,
  size_t max_machines,
  size_t *machine_count
);
/** Extract the selected-machine connectivity metadata from a dashboard JSON root object. */
bool lm_ctrl_cloud_parse_dashboard_machine_status(
  cJSON *root,
  lm_ctrl_cloud_machine_status_t *status
);
/** Extract the dashboard no-water alarm state when present. */
bool lm_ctrl_cloud_parse_dashboard_water_status(
  cJSON *root,
  lm_ctrl_machine_water_status_t *status
);

/** Extract controller-facing values and feature flags from a dashboard JSON root object. */
esp_err_t lm_ctrl_cloud_parse_dashboard_root_values(
  cJSON *root,
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  lm_ctrl_machine_heat_info_t *heat_info,
  bool *brew_active,
  int64_t *brew_start_epoch_ms
);

/** Parse cloud prebrewing widget timings from one dashboard widget entry. */
bool lm_ctrl_cloud_parse_prebrew_widget_values(cJSON *widget, float *seconds_in, float *seconds_out);
