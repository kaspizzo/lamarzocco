#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller_state.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t preset_count;
  float temperature_step_c;
  float time_step_s;
  bool preset_reduce_confirmed;
} lm_ctrl_setup_portal_advanced_form_t;

/** Parse the advanced settings form fields from a URL-encoded request body. */
bool lm_ctrl_setup_portal_parse_advanced_form(
  const char *body,
  lm_ctrl_setup_portal_advanced_form_t *form
);
/** Validate the advanced settings form against the current controller state. */
bool lm_ctrl_setup_portal_validate_advanced_form(
  const lm_ctrl_setup_portal_advanced_form_t *form,
  const ctrl_state_t *current_state,
  char *error_text,
  size_t error_text_size
);
/** Start the setup portal HTTP server and register all portal and captive routes. */
esp_err_t lm_ctrl_setup_portal_start_http_server(void);

#ifdef __cplusplus
}
#endif
