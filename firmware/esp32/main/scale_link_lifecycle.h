#pragma once
#include <stdbool.h>
#include <stdint.h>

/* A stop is a transaction: invalidate once, then drain asynchronous GAP/GATT
 * callbacks before another source session is allowed to begin. */
typedef struct {
  bool stopping;
  uint64_t gateway_close_at;
} lm_scale_link_lifecycle_t;

static inline bool lm_scale_link_begin_stop(lm_scale_link_lifecycle_t *state, uint64_t now, bool gateway_present) {
  if (state->stopping) return false;
  state->stopping = true;
  state->gateway_close_at = gateway_present ? now + 250 : 0;
  return true;
}

static inline bool lm_scale_link_finish_stop(lm_scale_link_lifecycle_t *state,
                                            bool source_present, bool gateway_present,
                                            bool scanning, bool connecting) {
  if (!state->stopping || source_present || gateway_present || scanning || connecting) return false;
  *state = (lm_scale_link_lifecycle_t){0};
  return true;
}

static inline bool lm_scale_link_allows_io(const lm_scale_link_lifecycle_t *state) {
  return !state->stopping;
}
