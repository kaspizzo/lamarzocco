#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_system.h"

#if defined(__has_include)
#if __has_include("esp_random.h")
#include "esp_random.h"
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LM_CTRL_WEB_ADMIN_SALT_LEN 16
#define LM_CTRL_WEB_ADMIN_HASH_LEN 32
#define LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS 1500U
#define LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS_LEGACY 120000U
#define LM_CTRL_RANDOM_TOKEN_BYTES 16
#define LM_CTRL_RANDOM_TOKEN_HEX_LEN ((LM_CTRL_RANDOM_TOKEN_BYTES * 2) + 1)

/** Copy text into a fixed-size destination buffer with guaranteed NUL termination. */
static inline void copy_text(char *dst, size_t dst_size, const char *src) {
  size_t len;

  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  len = 0;
  while ((len + 1) < dst_size && src[len] != '\0') {
    ++len;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

/** Best-effort zeroization helper for secret-bearing buffers. */
static inline void secure_zero(void *ptr, size_t len) {
  volatile uint8_t *cursor = (volatile uint8_t *)ptr;

  while (cursor != NULL && len > 0) {
    *cursor++ = 0;
    --len;
  }
}

/** Constant-time equality for fixed-size secret buffers. */
static inline bool secure_equals(const uint8_t *lhs, const uint8_t *rhs, size_t len) {
  uint8_t diff = 0;

  if (lhs == NULL || rhs == NULL) {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    diff |= (uint8_t)(lhs[i] ^ rhs[i]);
  }
  return diff == 0;
}

/** Fill a destination string with random bytes encoded as lowercase hexadecimal. */
static inline void fill_random_hex(char *dst, size_t dst_size, size_t byte_count) {
  static const char HEX[] = "0123456789abcdef";
  uint8_t random_bytes[LM_CTRL_RANDOM_TOKEN_BYTES] = {0};

  if (dst == NULL || dst_size == 0 || byte_count == 0 || byte_count > sizeof(random_bytes) || dst_size <= (byte_count * 2U)) {
    if (dst != NULL && dst_size > 0) {
      dst[0] = '\0';
    }
    return;
  }

  esp_fill_random(random_bytes, byte_count);
  for (size_t i = 0; i < byte_count; ++i) {
    dst[i * 2U] = HEX[random_bytes[i] >> 4];
    dst[(i * 2U) + 1U] = HEX[random_bytes[i] & 0x0fU];
  }
  dst[byte_count * 2U] = '\0';
  secure_zero(random_bytes, sizeof(random_bytes));
}

#ifdef __cplusplus
}
#endif
