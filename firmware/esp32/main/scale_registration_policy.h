#pragma once
#include <stdint.h>
typedef enum {
  LM_SCALE_REGISTRATION_UNKNOWN,
  LM_SCALE_REGISTRATION_MISSING,
  LM_SCALE_REGISTRATION_PRESENT,
  LM_SCALE_REGISTRATION_OTHER,
} lm_scale_registration_t;
lm_scale_registration_t lm_scale_registration_classify(const char *response, const char *name);
uint32_t lm_scale_registration_retry_ms(unsigned failures);
