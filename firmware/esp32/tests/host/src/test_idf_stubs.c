#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/private/sha256.h"

#include <stdlib.h>
#include <string.h>

struct test_esp_http_client {
  esp_http_client_config_t config;
  int status_code;
};

const char *esp_err_to_name(esp_err_t err) {
  switch (err) {
    case ESP_OK:
      return "ESP_OK";
    case ESP_FAIL:
      return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
      return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
      return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
      return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
      return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:
      return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:
      return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:
      return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE:
      return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_VERSION:
      return "ESP_ERR_INVALID_VERSION";
    case ESP_ERR_NVS_NOT_FOUND:
      return "ESP_ERR_NVS_NOT_FOUND";
    default:
      return "ESP_ERR_UNKNOWN";
  }
}

esp_err_t esp_crt_bundle_attach(void *config) {
  (void)config;
  return ESP_OK;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
  struct test_esp_http_client *client;

  if (config == NULL) {
    return NULL;
  }

  client = calloc(1, sizeof(*client));
  if (client == NULL) {
    return NULL;
  }

  client->config = *config;
  client->status_code = 0;
  return client;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value) {
  (void)client;
  (void)name;
  (void)value;
  return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len) {
  (void)client;
  (void)data;
  (void)len;
  return ESP_OK;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client) {
  (void)client;
  return ESP_FAIL;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client) {
  if (client == NULL) {
    return 0;
  }

  return client->status_code;
}

void esp_http_client_cleanup(esp_http_client_handle_t client) {
  free(client);
}

void esp_fill_random(void *buffer, size_t length) {
  unsigned char *bytes = buffer;

  for (size_t i = 0; i < length; ++i) {
    bytes[i] = (unsigned char)(0xA0U + (unsigned char)(i & 0x1FU));
  }
}

int64_t esp_timer_get_time(void) {
  return 1700000000000LL;
}

int mbedtls_base64_encode(
  unsigned char *dst,
  size_t dlen,
  size_t *olen,
  const unsigned char *src,
  size_t slen
) {
  size_t required = slen + 1U;

  if (olen != NULL) {
    *olen = 0;
  }
  if (dst == NULL || dlen < required) {
    return -1;
  }

  memcpy(dst, src, slen);
  dst[slen] = '\0';
  if (olen != NULL) {
    *olen = slen;
  }
  return 0;
}

void mbedtls_pk_init(mbedtls_pk_context *ctx) {
  if (ctx != NULL) {
    ctx->unused = 0;
  }
}

void mbedtls_pk_free(mbedtls_pk_context *ctx) {
  (void)ctx;
}

int mbedtls_pk_parse_key(
  mbedtls_pk_context *ctx,
  const unsigned char *key,
  size_t keylen,
  const unsigned char *pwd,
  size_t pwdlen
) {
  (void)ctx;
  (void)key;
  (void)keylen;
  (void)pwd;
  (void)pwdlen;
  return 0;
}

int mbedtls_pk_write_pubkey_der(mbedtls_pk_context *ctx, unsigned char *buf, size_t size) {
  (void)ctx;

  if (buf == NULL || size == 0) {
    return -1;
  }

  buf[size - 1] = 0x42;
  return 1;
}

int mbedtls_pk_sign(
  mbedtls_pk_context *ctx,
  int md_alg,
  const unsigned char *hash,
  size_t hash_len,
  unsigned char *sig,
  size_t sig_size,
  size_t *sig_len
) {
  size_t copy_len = hash_len < sig_size ? hash_len : sig_size;

  (void)ctx;
  (void)md_alg;
  if (sig == NULL || sig_len == NULL) {
    return -1;
  }

  if (hash != NULL && copy_len > 0) {
    memcpy(sig, hash, copy_len);
  }
  *sig_len = copy_len;
  return 0;
}

int mbedtls_sha256(
  const unsigned char *input,
  size_t ilen,
  unsigned char output[32],
  int is224
) {
  (void)is224;

  if (output == NULL) {
    return -1;
  }

  memset(output, 0, 32);
  for (size_t i = 0; i < ilen; ++i) {
    output[i % 32] ^= input[i];
  }
  return 0;
}
