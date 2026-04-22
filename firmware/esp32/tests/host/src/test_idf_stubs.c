#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "nvs_sec_provider.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/private/sha256.h"
#include "psa/crypto.h"
#include "test_psa.h"

#include <stdlib.h>
#include <string.h>

struct test_esp_http_client {
  esp_http_client_config_t config;
  int status_code;
};

static const unsigned char BASE64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const uint32_t SHA256_K[64] = {
  0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
  0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
  0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
  0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
  0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
  0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
  0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
  0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
  0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
  0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
  0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
  0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
  0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
  0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
  0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
  0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

typedef struct {
  int in_use;
  unsigned char private_key[64];
  size_t private_key_len;
} test_psa_key_slot_t;

static test_psa_key_slot_t s_psa_keys[8];
static unsigned int s_psa_next_key_id = 1;
static unsigned int s_psa_generate_count = 0;
static unsigned int s_psa_destroy_count = 0;
static int s_psa_wrap_result = 0;
static unsigned int s_random_counter = 0;
static test_http_client_event_spec_t s_http_client_events[16];
static size_t s_http_client_event_count = 0;
static int s_http_client_status_code = 0;
static esp_err_t s_http_client_perform_result = ESP_FAIL;

void test_psa_reset(void) {
  memset(s_psa_keys, 0, sizeof(s_psa_keys));
  s_psa_next_key_id = 1;
  s_psa_generate_count = 0;
  s_psa_destroy_count = 0;
  s_psa_wrap_result = 0;
}

void test_psa_set_wrap_result(int result) {
  s_psa_wrap_result = result;
}

unsigned int test_psa_generate_count(void) {
  return s_psa_generate_count;
}

unsigned int test_psa_destroy_count(void) {
  return s_psa_destroy_count;
}

size_t test_psa_active_key_count(void) {
  size_t count = 0;

  for (size_t i = 0; i < sizeof(s_psa_keys) / sizeof(s_psa_keys[0]); ++i) {
    if (s_psa_keys[i].in_use) {
      ++count;
    }
  }
  return count;
}

static uint32_t rotr32(uint32_t value, uint32_t shift) {
  return (value >> shift) | (value << (32U - shift));
}

static void sha256_compute(const unsigned char *input, size_t ilen, unsigned char output[32]) {
  uint32_t state[8] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  unsigned char block[64];
  uint64_t bit_length = (uint64_t)ilen * 8ULL;
  size_t processed = 0;

  while (processed + 64U <= ilen) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    for (size_t i = 0; i < 16U; ++i) {
      size_t offset = processed + (i * 4U);
      w[i] =
        ((uint32_t)input[offset + 0U] << 24) |
        ((uint32_t)input[offset + 1U] << 16) |
        ((uint32_t)input[offset + 2U] << 8) |
        ((uint32_t)input[offset + 3U]);
    }
    for (size_t i = 16U; i < 64U; ++i) {
      uint32_t s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
      uint32_t s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
      w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    for (size_t i = 0; i < 64U; ++i) {
      uint32_t s1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t temp1 = h + s1 + ch + SHA256_K[i] + w[i];
      uint32_t s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
    processed += 64U;
  }

  memset(block, 0, sizeof(block));
  if (processed < ilen) {
    memcpy(block, input + processed, ilen - processed);
  }
  block[ilen - processed] = 0x80U;

  if ((ilen - processed) >= 56U) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    memset(w, 0, sizeof(w));
    for (size_t i = 0; i < 16U; ++i) {
      size_t offset = i * 4U;
      w[i] =
        ((uint32_t)block[offset + 0U] << 24) |
        ((uint32_t)block[offset + 1U] << 16) |
        ((uint32_t)block[offset + 2U] << 8) |
        ((uint32_t)block[offset + 3U]);
    }
    for (size_t i = 16U; i < 64U; ++i) {
      uint32_t s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
      uint32_t s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
      w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];
    for (size_t i = 0; i < 64U; ++i) {
      uint32_t s1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t temp1 = h + s1 + ch + SHA256_K[i] + w[i];
      uint32_t s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
    memset(block, 0, sizeof(block));
  }

  block[56] = (unsigned char)((bit_length >> 56) & 0xFFU);
  block[57] = (unsigned char)((bit_length >> 48) & 0xFFU);
  block[58] = (unsigned char)((bit_length >> 40) & 0xFFU);
  block[59] = (unsigned char)((bit_length >> 32) & 0xFFU);
  block[60] = (unsigned char)((bit_length >> 24) & 0xFFU);
  block[61] = (unsigned char)((bit_length >> 16) & 0xFFU);
  block[62] = (unsigned char)((bit_length >> 8) & 0xFFU);
  block[63] = (unsigned char)(bit_length & 0xFFU);

  {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    memset(w, 0, sizeof(w));
    for (size_t i = 0; i < 16U; ++i) {
      size_t offset = i * 4U;
      w[i] =
        ((uint32_t)block[offset + 0U] << 24) |
        ((uint32_t)block[offset + 1U] << 16) |
        ((uint32_t)block[offset + 2U] << 8) |
        ((uint32_t)block[offset + 3U]);
    }
    for (size_t i = 16U; i < 64U; ++i) {
      uint32_t s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
      uint32_t s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
      w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];
    for (size_t i = 0; i < 64U; ++i) {
      uint32_t s1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t temp1 = h + s1 + ch + SHA256_K[i] + w[i];
      uint32_t s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  for (size_t i = 0; i < 8U; ++i) {
    output[i * 4U + 0U] = (unsigned char)((state[i] >> 24) & 0xFFU);
    output[i * 4U + 1U] = (unsigned char)((state[i] >> 16) & 0xFFU);
    output[i * 4U + 2U] = (unsigned char)((state[i] >> 8) & 0xFFU);
    output[i * 4U + 3U] = (unsigned char)(state[i] & 0xFFU);
  }
}

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
    case ESP_ERR_NVS_INVALID_LENGTH:
      return "ESP_ERR_NVS_INVALID_LENGTH";
    case ESP_ERR_NVS_NO_FREE_PAGES:
      return "ESP_ERR_NVS_NO_FREE_PAGES";
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
      return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    case ESP_ERR_NVS_SEC_HMAC_KEY_NOT_FOUND:
      return "ESP_ERR_NVS_SEC_HMAC_KEY_NOT_FOUND";
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
  client->status_code = s_http_client_status_code;
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
  if (client == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_http_client_perform_result != ESP_OK) {
    return s_http_client_perform_result;
  }

  for (size_t i = 0; i < s_http_client_event_count; ++i) {
    esp_http_client_event_t event = {
      .event_id = s_http_client_events[i].event_id,
      .user_data = client->config.user_data,
      .header_key = s_http_client_events[i].header_key,
      .header_value = s_http_client_events[i].header_value,
      .data = s_http_client_events[i].data,
      .data_len = s_http_client_events[i].data_len,
    };
    esp_err_t ret = ESP_OK;

    if (client->config.event_handler == NULL) {
      continue;
    }
    ret = client->config.event_handler(&event);
    if (ret != ESP_OK) {
      return ret;
    }
  }

  return ESP_OK;
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

void test_http_client_reset(void) {
  memset(s_http_client_events, 0, sizeof(s_http_client_events));
  s_http_client_event_count = 0;
  s_http_client_status_code = 0;
  s_http_client_perform_result = ESP_FAIL;
}

void test_http_client_set_status_code(int status_code) {
  s_http_client_status_code = status_code;
}

void test_http_client_set_perform_result(esp_err_t result) {
  s_http_client_perform_result = result;
}

void test_http_client_set_response_events(const test_http_client_event_spec_t *events, size_t event_count) {
  if (events == NULL || event_count == 0) {
    memset(s_http_client_events, 0, sizeof(s_http_client_events));
    s_http_client_event_count = 0;
    return;
  }

  if (event_count > (sizeof(s_http_client_events) / sizeof(s_http_client_events[0]))) {
    event_count = sizeof(s_http_client_events) / sizeof(s_http_client_events[0]);
  }

  memcpy(s_http_client_events, events, event_count * sizeof(s_http_client_events[0]));
  if (event_count < (sizeof(s_http_client_events) / sizeof(s_http_client_events[0]))) {
    memset(
      s_http_client_events + event_count,
      0,
      (sizeof(s_http_client_events) / sizeof(s_http_client_events[0]) - event_count) * sizeof(s_http_client_events[0])
    );
  }
  s_http_client_event_count = event_count;
}

void esp_fill_random(void *buffer, size_t length) {
  unsigned char *bytes = buffer;

  for (size_t i = 0; i < length; ++i) {
    bytes[i] = (unsigned char)(0xA0U + (unsigned char)((s_random_counter + i) & 0x1FU));
  }
  s_random_counter += (unsigned int)length;
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
  size_t required = ((slen + 2U) / 3U) * 4U + 1U;
  size_t out = 0;

  if (olen != NULL) {
    *olen = 0;
  }
  if (dst == NULL || dlen < required) {
    return -1;
  }

  for (size_t i = 0; i < slen; i += 3U) {
    unsigned value = (unsigned)src[i] << 16;
    size_t remaining = slen - i;

    if (remaining > 1U) {
      value |= (unsigned)src[i + 1U] << 8;
    }
    if (remaining > 2U) {
      value |= (unsigned)src[i + 2U];
    }

    dst[out++] = BASE64_TABLE[(value >> 18) & 0x3FU];
    dst[out++] = BASE64_TABLE[(value >> 12) & 0x3FU];
    dst[out++] = remaining > 1U ? BASE64_TABLE[(value >> 6) & 0x3FU] : '=';
    dst[out++] = remaining > 2U ? BASE64_TABLE[value & 0x3FU] : '=';
  }
  dst[out] = '\0';
  if (olen != NULL) {
    *olen = out;
  }
  return 0;
}

int mbedtls_base64_decode(
  unsigned char *dst,
  size_t dlen,
  size_t *olen,
  const unsigned char *src,
  size_t slen
) {
  size_t out = 0;
  unsigned accumulator = 0;
  int bits_collected = 0;

  if (olen != NULL) {
    *olen = 0;
  }
  if (dst == NULL) {
    return -1;
  }

  for (size_t i = 0; i < slen; ++i) {
    unsigned char ch = src[i];
    int value = -1;

    if (ch >= 'A' && ch <= 'Z') {
      value = ch - 'A';
    } else if (ch >= 'a' && ch <= 'z') {
      value = ch - 'a' + 26;
    } else if (ch >= '0' && ch <= '9') {
      value = ch - '0' + 52;
    } else if (ch == '+') {
      value = 62;
    } else if (ch == '/') {
      value = 63;
    } else if (ch == '=') {
      break;
    } else if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
      continue;
    } else {
      return -1;
    }

    accumulator = (accumulator << 6) | (unsigned)value;
    bits_collected += 6;
    if (bits_collected >= 8) {
      bits_collected -= 8;
      if (out >= dlen) {
        return -1;
      }
      dst[out++] = (unsigned char)((accumulator >> bits_collected) & 0xFFU);
    }
  }

  if (olen != NULL) {
    *olen = out;
  }
  return 0;
}

void mbedtls_pk_init(mbedtls_pk_context *ctx) {
  if (ctx != NULL) {
    memset(ctx, 0, sizeof(*ctx));
  }
}

void mbedtls_pk_free(mbedtls_pk_context *ctx) {
  if (ctx != NULL) {
    memset(ctx, 0, sizeof(*ctx));
  }
}

int mbedtls_pk_wrap_psa(mbedtls_pk_context *ctx, const mbedtls_svc_key_id_t key) {
  size_t slot_index = key == 0 ? 0U : (size_t)(key - 1U);

  if (s_psa_wrap_result != 0) {
    return s_psa_wrap_result;
  }

  if (ctx == NULL ||
      key == 0 ||
      slot_index >= (sizeof(s_psa_keys) / sizeof(s_psa_keys[0])) ||
      !s_psa_keys[slot_index].in_use ||
      s_psa_keys[slot_index].private_key_len == 0 ||
      s_psa_keys[slot_index].private_key_len > sizeof(ctx->private_key)) {
    return -1;
  }

  memset(ctx, 0, sizeof(*ctx));
  memcpy(ctx->private_key, s_psa_keys[slot_index].private_key, s_psa_keys[slot_index].private_key_len);
  ctx->private_key_len = s_psa_keys[slot_index].private_key_len;
  ctx->setup = 1;
  return 0;
}

const mbedtls_pk_info_t *mbedtls_pk_info_from_type(int pk_type) {
  static mbedtls_pk_info_t info = {0};

  info.type = pk_type;
  return pk_type == MBEDTLS_PK_ECKEY ? &info : NULL;
}

int mbedtls_pk_setup(mbedtls_pk_context *ctx, const mbedtls_pk_info_t *info) {
  if (ctx == NULL || info == NULL || info->type != MBEDTLS_PK_ECKEY) {
    return -1;
  }

  ctx->setup = 1;
  return 0;
}

int mbedtls_pk_parse_key(
  mbedtls_pk_context *ctx,
  const unsigned char *key,
  size_t keylen,
  const unsigned char *pwd,
  size_t pwdlen
) {
  if (ctx == NULL || key == NULL || keylen == 0 || keylen > sizeof(ctx->private_key)) {
    return -1;
  }

  memset(ctx, 0, sizeof(*ctx));
  memcpy(ctx->private_key, key, keylen);
  ctx->private_key_len = keylen;
  ctx->setup = 1;
  (void)pwd;
  (void)pwdlen;
  return 0;
}

int mbedtls_pk_write_key_der(mbedtls_pk_context *ctx, unsigned char *buf, size_t size) {
  if (ctx == NULL || buf == NULL || ctx->private_key_len == 0 || ctx->private_key_len > size) {
    return -1;
  }

  memcpy(buf + size - ctx->private_key_len, ctx->private_key, ctx->private_key_len);
  return (int)ctx->private_key_len;
}

int mbedtls_pk_write_pubkey_der(mbedtls_pk_context *ctx, unsigned char *buf, size_t size) {
  unsigned char derived[33];

  if (ctx == NULL || buf == NULL || size < sizeof(derived) || ctx->private_key_len == 0) {
    return -1;
  }

  derived[0] = 0x04;
  for (size_t i = 0; i < sizeof(derived) - 1U; ++i) {
    derived[i + 1U] = (unsigned char)(ctx->private_key[i % ctx->private_key_len] ^ (unsigned char)(0x5AU + i));
  }

  memcpy(buf + size - sizeof(derived), derived, sizeof(derived));
  return (int)sizeof(derived);
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
  sha256_compute(input, ilen, output);
  return 0;
}

int mbedtls_ecp_gen_key(
  int grp_id,
  mbedtls_ecp_keypair *key,
  int (*f_rng)(void *, unsigned char *, size_t),
  void *p_rng
) {
  if (grp_id != MBEDTLS_ECP_DP_SECP256R1 || key == NULL || f_rng == NULL) {
    return -1;
  }

  key->private_key_len = 32;
  key->setup = 1;
  return f_rng(p_rng, key->private_key, key->private_key_len);
}

psa_status_t psa_crypto_init(void) {
  return PSA_SUCCESS;
}

psa_status_t psa_generate_key(const psa_key_attributes_t *attributes, psa_key_id_t *key) {
  size_t slot_index = 0;

  if (attributes == NULL || key == NULL) {
    return PSA_ERROR_INVALID_ARGUMENT;
  }
  if (attributes->type != PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1) ||
      attributes->bits != 256U ||
      (attributes->usage & PSA_KEY_USAGE_EXPORT) == 0U ||
      (attributes->usage & PSA_KEY_USAGE_SIGN_HASH) == 0U ||
      attributes->alg != PSA_ALG_ECDSA(PSA_ALG_SHA_256)) {
    return PSA_ERROR_INVALID_ARGUMENT;
  }

  slot_index = (size_t)(s_psa_next_key_id - 1U);
  if (slot_index >= (sizeof(s_psa_keys) / sizeof(s_psa_keys[0]))) {
    return PSA_ERROR_GENERIC_ERROR;
  }

  memset(&s_psa_keys[slot_index], 0, sizeof(s_psa_keys[slot_index]));
  s_psa_keys[slot_index].in_use = 1;
  s_psa_keys[slot_index].private_key_len = 32;
  esp_fill_random(s_psa_keys[slot_index].private_key, s_psa_keys[slot_index].private_key_len);
  s_psa_generate_count++;
  *key = s_psa_next_key_id++;
  return PSA_SUCCESS;
}

psa_status_t psa_destroy_key(psa_key_id_t key) {
  size_t slot_index = key == 0 ? 0U : (size_t)(key - 1U);

  if (key == 0 || slot_index >= (sizeof(s_psa_keys) / sizeof(s_psa_keys[0]))) {
    return PSA_ERROR_INVALID_ARGUMENT;
  }

  memset(&s_psa_keys[slot_index], 0, sizeof(s_psa_keys[slot_index]));
  s_psa_destroy_count++;
  return PSA_SUCCESS;
}

const mbedtls_md_info_t *mbedtls_md_info_from_type(int md_type) {
  static mbedtls_md_info_t info = {0};

  info.type = md_type;
  return md_type == MBEDTLS_MD_SHA256 ? &info : NULL;
}

int mbedtls_md_hmac(
  const mbedtls_md_info_t *md_info,
  const unsigned char *key,
  size_t keylen,
  const unsigned char *input,
  size_t ilen,
  unsigned char *output
) {
  unsigned char ipad[64];
  unsigned char opad[64];
  unsigned char inner_hash[32];
  unsigned char key_block[64];
  unsigned char *inner_input = NULL;
  unsigned char *outer_input = NULL;
  size_t inner_len = 0;

  if (md_info == NULL || md_info->type != MBEDTLS_MD_SHA256 || key == NULL || input == NULL || output == NULL) {
    return -1;
  }

  memset(key_block, 0, sizeof(key_block));
  if (keylen > sizeof(key_block)) {
    sha256_compute(key, keylen, key_block);
  } else if (keylen > 0) {
    memcpy(key_block, key, keylen);
  }

  for (size_t i = 0; i < sizeof(key_block); ++i) {
    ipad[i] = (unsigned char)(key_block[i] ^ 0x36U);
    opad[i] = (unsigned char)(key_block[i] ^ 0x5CU);
  }

  inner_len = sizeof(ipad) + ilen;
  inner_input = malloc(inner_len);
  outer_input = malloc(sizeof(opad) + sizeof(inner_hash));
  if (inner_input == NULL || outer_input == NULL) {
    free(inner_input);
    free(outer_input);
    return -1;
  }

  memcpy(inner_input, ipad, sizeof(ipad));
  memcpy(inner_input + sizeof(ipad), input, ilen);
  sha256_compute(inner_input, inner_len, inner_hash);

  memcpy(outer_input, opad, sizeof(opad));
  memcpy(outer_input + sizeof(opad), inner_hash, sizeof(inner_hash));
  sha256_compute(outer_input, sizeof(opad) + sizeof(inner_hash), output);

  free(inner_input);
  free(outer_input);
  return 0;
}

esp_err_t nvs_flash_init(void) {
  return ESP_OK;
}

void nvs_flash_deinit(void) {
}

esp_err_t nvs_flash_erase(void) {
  return ESP_OK;
}

esp_err_t nvs_flash_secure_init(nvs_sec_cfg_t *cfg) {
  (void)cfg;
  return ESP_OK;
}

esp_err_t nvs_sec_provider_register_hmac(const nvs_sec_config_hmac_t *cfg, nvs_sec_scheme_t **out_scheme) {
  static nvs_sec_scheme_t scheme = {0};

  (void)cfg;
  if (out_scheme == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *out_scheme = &scheme;
  return ESP_OK;
}

void nvs_sec_provider_deregister(nvs_sec_scheme_t *scheme) {
  (void)scheme;
}

esp_err_t nvs_flash_generate_keys_v2(nvs_sec_scheme_t *scheme, nvs_sec_cfg_t *cfg) {
  (void)scheme;
  if (cfg == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(cfg, 0x5A, sizeof(*cfg));
  return ESP_OK;
}

esp_err_t nvs_flash_read_security_cfg_v2(nvs_sec_scheme_t *scheme, nvs_sec_cfg_t *cfg) {
  (void)scheme;
  (void)cfg;
  return ESP_ERR_NVS_SEC_HMAC_KEY_NOT_FOUND;
}
