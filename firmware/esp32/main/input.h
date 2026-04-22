#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "controller_state.h"

/** Events produced by the physical ring, touch gestures/buttons, and internal runtime wakeups. */
typedef enum {
  LM_CTRL_EVENT_ROTATE = 0,
  LM_CTRL_EVENT_SELECT_FOCUS,
  LM_CTRL_EVENT_TOGGLE_FOCUS,
  LM_CTRL_EVENT_OPEN_PRESETS,
  LM_CTRL_EVENT_LOAD_PRESET,
  LM_CTRL_EVENT_SAVE_PRESET,
  LM_CTRL_EVENT_OPEN_SETUP,
  LM_CTRL_EVENT_CLOSE_SCREEN,
  LM_CTRL_EVENT_OPEN_SETUP_RESET,
  LM_CTRL_EVENT_CANCEL_SETUP_RESET,
  LM_CTRL_EVENT_CONFIRM_SETUP_RESET,
  LM_CTRL_EVENT_DISMISS_SHOT_TIMER,
  LM_CTRL_EVENT_RUNTIME_TICK,
} lm_ctrl_input_event_type_t;

/** Normalized input event consumed by the main controller loop. */
typedef struct {
  lm_ctrl_input_event_type_t type;
  int delta_steps;
  ctrl_focus_t focus;
} lm_ctrl_input_event_t;

/** Initialize physical input handling and forward events into the supplied queue. */
esp_err_t lm_ctrl_input_init(QueueHandle_t event_queue);
