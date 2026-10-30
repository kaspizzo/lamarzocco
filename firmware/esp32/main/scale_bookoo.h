#pragma once
#include "scale_driver.h"

/* BOOKOO Mini/Ultra common 03/0B measurement envelope. Values remain grams
 * even when the Ultra's display-unit field says ounces. No hardware validation
 * or command-completion acknowledgement is implied by this decoder. */
typedef struct {
  double grams;
  uint32_t timer_ms;
  double flow_gps;
  bool flow_valid;
  uint8_t battery;
} lm_scale_bookoo_measurement_t;

bool lm_scale_bookoo_parse(const uint8_t *data, size_t size, lm_scale_bookoo_measurement_t *out);
lm_scale_frame_t lm_scale_bookoo_tare_start(void);
