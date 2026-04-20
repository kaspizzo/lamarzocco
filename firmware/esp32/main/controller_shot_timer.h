#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wifi_setup_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LM_CTRL_SHOT_TIMER_HIDDEN = 0,
  LM_CTRL_SHOT_TIMER_LIVE,
  LM_CTRL_SHOT_TIMER_STICKY,
} lm_ctrl_shot_timer_mode_t;

typedef struct {
  lm_ctrl_shot_timer_mode_t mode;
  uint32_t seconds;
} lm_ctrl_shot_timer_state_t;

void lm_ctrl_shot_timer_reset(lm_ctrl_shot_timer_state_t *state);
bool lm_ctrl_shot_timer_update(lm_ctrl_shot_timer_state_t *state, const lm_ctrl_shot_timer_info_t *info);
bool lm_ctrl_shot_timer_dismiss(lm_ctrl_shot_timer_state_t *state);
bool lm_ctrl_shot_timer_visible(const lm_ctrl_shot_timer_state_t *state);
bool lm_ctrl_shot_timer_dismissable(const lm_ctrl_shot_timer_state_t *state);

#ifdef __cplusplus
}
#endif
