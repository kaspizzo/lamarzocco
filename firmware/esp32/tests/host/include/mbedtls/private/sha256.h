#pragma once

#include <stddef.h>

int mbedtls_sha256(
  const unsigned char *input,
  size_t ilen,
  unsigned char output[32],
  int is224
);
