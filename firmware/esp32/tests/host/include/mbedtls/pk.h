#pragma once

#include <stddef.h>

#define MBEDTLS_MD_SHA256 0
#define MBEDTLS_PK_SIGNATURE_MAX_SIZE 128

typedef struct {
  int unused;
} mbedtls_pk_context;

void mbedtls_pk_init(mbedtls_pk_context *ctx);
void mbedtls_pk_free(mbedtls_pk_context *ctx);
int mbedtls_pk_parse_key(
  mbedtls_pk_context *ctx,
  const unsigned char *key,
  size_t keylen,
  const unsigned char *pwd,
  size_t pwdlen
);
int mbedtls_pk_write_pubkey_der(mbedtls_pk_context *ctx, unsigned char *buf, size_t size);
int mbedtls_pk_sign(
  mbedtls_pk_context *ctx,
  int md_alg,
  const unsigned char *hash,
  size_t hash_len,
  unsigned char *sig,
  size_t sig_size,
  size_t *sig_len
);
