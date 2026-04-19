#pragma once

#include <stddef.h>

#define MBEDTLS_MD_SHA256 0
#define MBEDTLS_PK_ECKEY 1
#define MBEDTLS_PK_SIGNATURE_MAX_SIZE 128

typedef struct {
  unsigned char private_key[64];
  size_t private_key_len;
  int setup;
} mbedtls_pk_context;

typedef unsigned int mbedtls_svc_key_id_t;

typedef struct {
  int type;
} mbedtls_pk_info_t;

void mbedtls_pk_init(mbedtls_pk_context *ctx);
void mbedtls_pk_free(mbedtls_pk_context *ctx);
int mbedtls_pk_wrap_psa(mbedtls_pk_context *ctx, const mbedtls_svc_key_id_t key);
const mbedtls_pk_info_t *mbedtls_pk_info_from_type(int pk_type);
int mbedtls_pk_setup(mbedtls_pk_context *ctx, const mbedtls_pk_info_t *info);
int mbedtls_pk_parse_key(
  mbedtls_pk_context *ctx,
  const unsigned char *key,
  size_t keylen,
  const unsigned char *pwd,
  size_t pwdlen
);
int mbedtls_pk_write_key_der(mbedtls_pk_context *ctx, unsigned char *buf, size_t size);
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

#define mbedtls_pk_ec(ctx) (&(ctx))
