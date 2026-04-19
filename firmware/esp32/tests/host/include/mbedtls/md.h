#pragma once

#include <stddef.h>

#define MBEDTLS_MD_SHA256 0

typedef struct {
  int type;
} mbedtls_md_info_t;

const mbedtls_md_info_t *mbedtls_md_info_from_type(int md_type);
int mbedtls_md_hmac(
  const mbedtls_md_info_t *md_info,
  const unsigned char *key,
  size_t keylen,
  const unsigned char *input,
  size_t ilen,
  unsigned char *output
);
