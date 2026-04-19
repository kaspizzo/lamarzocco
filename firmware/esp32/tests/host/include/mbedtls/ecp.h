#pragma once

#include <stddef.h>

#include "mbedtls/pk.h"

#define MBEDTLS_ECP_DP_SECP256R1 1

typedef mbedtls_pk_context mbedtls_ecp_keypair;

int mbedtls_ecp_gen_key(
  int grp_id,
  mbedtls_ecp_keypair *key,
  int (*f_rng)(void *, unsigned char *, size_t),
  void *p_rng
);
