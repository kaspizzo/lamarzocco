#pragma once

#include <stddef.h>

int mbedtls_base64_encode(
  unsigned char *dst,
  size_t dlen,
  size_t *olen,
  const unsigned char *src,
  size_t slen
);

int mbedtls_base64_decode(
  unsigned char *dst,
  size_t dlen,
  size_t *olen,
  const unsigned char *src,
  size_t slen
);
