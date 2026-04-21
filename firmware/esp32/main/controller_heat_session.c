#include "controller_heat_session.h"

#include <stddef.h>

static int64_t resolve_remaining_us(
  const lm_ctrl_heat_session_t *session,
  int64_t now_local_us,
  int64_t now_epoch_ms,
  int64_t observed_epoch_ms,
  int64_t ready_epoch_ms
) {
  int64_t remaining_us = 0;

  if (ready_epoch_ms <= 0) {
    return 0;
  }
  if (observed_epoch_ms > 0 && ready_epoch_ms > observed_epoch_ms) {
    return (ready_epoch_ms - observed_epoch_ms) * 1000LL;
  }
  if (now_epoch_ms > 0 && ready_epoch_ms > now_epoch_ms) {
    return (ready_epoch_ms - now_epoch_ms) * 1000LL;
  }
  if (session == NULL ||
      session->deadline_local_us <= now_local_us ||
      session->source_ready_epoch_ms <= 0) {
    return 0;
  }

  remaining_us = session->deadline_local_us - now_local_us;
  if (session->source_ready_epoch_ms == ready_epoch_ms) {
    return remaining_us;
  }

  remaining_us += (ready_epoch_ms - session->source_ready_epoch_ms) * 1000LL;
  return remaining_us > 0 ? remaining_us : 0;
}

void lm_ctrl_heat_session_reset(lm_ctrl_heat_session_t *session) {
  if (session == NULL) {
    return;
  }

  *session = (lm_ctrl_heat_session_t){0};
}

bool lm_ctrl_heat_session_apply(
  lm_ctrl_heat_session_t *session,
  bool heating,
  bool eta_available,
  int64_t now_local_us,
  int64_t now_epoch_ms,
  int64_t observed_epoch_ms,
  int64_t ready_epoch_ms
) {
  int64_t remaining_us = 0;

  if (session == NULL) {
    return false;
  }
  if (!heating) {
    lm_ctrl_heat_session_reset(session);
    return false;
  }

  session->heating = true;
  if (!eta_available || ready_epoch_ms <= 0) {
    return lm_ctrl_heat_session_has_eta(session);
  }

  remaining_us = resolve_remaining_us(session, now_local_us, now_epoch_ms, observed_epoch_ms, ready_epoch_ms);
  if (remaining_us <= 0) {
    return lm_ctrl_heat_session_has_eta(session);
  }

  if (session->session_start_local_us <= 0 || session->session_start_local_us > now_local_us) {
    session->session_start_local_us = now_local_us;
  }
  session->deadline_local_us = now_local_us + remaining_us;
  session->source_ready_epoch_ms = ready_epoch_ms;
  return true;
}

bool lm_ctrl_heat_session_has_eta(const lm_ctrl_heat_session_t *session) {
  return session != NULL &&
         session->heating &&
         session->session_start_local_us > 0 &&
         session->deadline_local_us > 0 &&
         session->deadline_local_us > session->session_start_local_us;
}

int64_t lm_ctrl_heat_session_remaining_us(const lm_ctrl_heat_session_t *session, int64_t now_local_us) {
  if (!lm_ctrl_heat_session_has_eta(session) || now_local_us >= session->deadline_local_us) {
    return 0;
  }

  return session->deadline_local_us - now_local_us;
}

uint16_t lm_ctrl_heat_session_progress_permille(const lm_ctrl_heat_session_t *session, int64_t now_local_us) {
  int64_t total_us = 0;
  int64_t remaining_us = 0;

  if (!lm_ctrl_heat_session_has_eta(session)) {
    return 0;
  }

  total_us = session->deadline_local_us - session->session_start_local_us;
  remaining_us = lm_ctrl_heat_session_remaining_us(session, now_local_us);
  if (total_us <= 0 || remaining_us <= 0) {
    return 0;
  }
  if (remaining_us >= total_us) {
    return 1000;
  }

  return (uint16_t)((remaining_us * 1000LL) / total_us);
}
