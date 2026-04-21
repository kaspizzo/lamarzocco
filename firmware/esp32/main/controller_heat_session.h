#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool heating;
  int64_t session_start_local_us;
  int64_t deadline_local_us;
  int64_t source_ready_epoch_ms;
} lm_ctrl_heat_session_t;

void lm_ctrl_heat_session_reset(lm_ctrl_heat_session_t *session);
bool lm_ctrl_heat_session_apply(
  lm_ctrl_heat_session_t *session,
  bool heating,
  bool eta_available,
  int64_t now_local_us,
  int64_t now_epoch_ms,
  int64_t observed_epoch_ms,
  int64_t ready_epoch_ms
);
bool lm_ctrl_heat_session_has_eta(const lm_ctrl_heat_session_t *session);
int64_t lm_ctrl_heat_session_remaining_us(const lm_ctrl_heat_session_t *session, int64_t now_local_us);
uint16_t lm_ctrl_heat_session_progress_permille(const lm_ctrl_heat_session_t *session, int64_t now_local_us);

#ifdef __cplusplus
}
#endif
