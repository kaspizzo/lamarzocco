#include "controller_shot_timer.h"

static bool shot_timer_live(const lm_ctrl_shot_timer_info_t *info) {
  return info != NULL && info->available && info->brew_active;
}

void lm_ctrl_shot_timer_reset(lm_ctrl_shot_timer_state_t *state) {
  if (state == NULL) {
    return;
  }

  *state = (lm_ctrl_shot_timer_state_t){
    .mode = LM_CTRL_SHOT_TIMER_HIDDEN,
    .seconds = 0,
  };
}

bool lm_ctrl_shot_timer_update(lm_ctrl_shot_timer_state_t *state, const lm_ctrl_shot_timer_info_t *info) {
  lm_ctrl_shot_timer_state_t previous;

  if (state == NULL) {
    return false;
  }

  previous = *state;
  if (shot_timer_live(info)) {
    state->mode = LM_CTRL_SHOT_TIMER_LIVE;
    state->seconds = info->seconds;
  } else if (state->mode == LM_CTRL_SHOT_TIMER_LIVE) {
    state->mode = LM_CTRL_SHOT_TIMER_STICKY;
  } else if (state->mode == LM_CTRL_SHOT_TIMER_HIDDEN) {
    state->seconds = 0;
  }

  return state->mode != previous.mode || state->seconds != previous.seconds;
}

bool lm_ctrl_shot_timer_dismiss(lm_ctrl_shot_timer_state_t *state) {
  if (state == NULL || state->mode != LM_CTRL_SHOT_TIMER_STICKY) {
    return false;
  }

  state->mode = LM_CTRL_SHOT_TIMER_HIDDEN;
  state->seconds = 0;
  return true;
}

bool lm_ctrl_shot_timer_visible(const lm_ctrl_shot_timer_state_t *state) {
  return state != NULL && state->mode != LM_CTRL_SHOT_TIMER_HIDDEN;
}

bool lm_ctrl_shot_timer_dismissable(const lm_ctrl_shot_timer_state_t *state) {
  return state != NULL && state->mode == LM_CTRL_SHOT_TIMER_STICKY;
}
