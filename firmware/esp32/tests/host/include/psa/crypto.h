#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int32_t psa_status_t;
typedef unsigned int psa_key_id_t;
typedef unsigned int psa_key_usage_t;
typedef unsigned int psa_algorithm_t;
typedef unsigned int psa_key_type_t;

typedef struct {
  psa_key_type_t type;
  size_t bits;
  psa_key_usage_t usage;
  psa_algorithm_t alg;
} psa_key_attributes_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_INVALID_ARGUMENT ((psa_status_t)-1)
#define PSA_ERROR_GENERIC_ERROR ((psa_status_t)-2)

#define PSA_KEY_ATTRIBUTES_INIT {0}

#define PSA_KEY_USAGE_EXPORT ((psa_key_usage_t)0x0001U)
#define PSA_KEY_USAGE_SIGN_HASH ((psa_key_usage_t)0x0002U)

#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000008U)
#define PSA_ALG_ECDSA(hash_alg) ((psa_algorithm_t)(0x06000600U | ((hash_alg) & 0x000000ffU)))

#define PSA_ECC_FAMILY_SECP_R1 ((psa_key_type_t)0x12U)
#define PSA_KEY_TYPE_ECC_KEY_PAIR(family) ((psa_key_type_t)(0x7100U | ((family) & 0x00ffU)))

static inline void psa_set_key_type(psa_key_attributes_t *attributes, psa_key_type_t type) {
  if (attributes != NULL) {
    attributes->type = type;
  }
}

static inline void psa_set_key_bits(psa_key_attributes_t *attributes, size_t bits) {
  if (attributes != NULL) {
    attributes->bits = bits;
  }
}

static inline void psa_set_key_usage_flags(psa_key_attributes_t *attributes, psa_key_usage_t usage_flags) {
  if (attributes != NULL) {
    attributes->usage = usage_flags;
  }
}

static inline void psa_set_key_algorithm(psa_key_attributes_t *attributes, psa_algorithm_t alg) {
  if (attributes != NULL) {
    attributes->alg = alg;
  }
}

static inline void psa_reset_key_attributes(psa_key_attributes_t *attributes) {
  if (attributes != NULL) {
    *attributes = (psa_key_attributes_t)PSA_KEY_ATTRIBUTES_INIT;
  }
}

psa_status_t psa_crypto_init(void);
psa_status_t psa_generate_key(const psa_key_attributes_t *attributes, psa_key_id_t *key);
psa_status_t psa_destroy_key(psa_key_id_t key);
