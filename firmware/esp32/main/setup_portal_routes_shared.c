/**
 * Shared advanced-form parsing and validation helpers used by the setup portal routes.
 */
#include "setup_portal_routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "setup_portal_http.h"

bool lm_ctrl_setup_portal_parse_advanced_form(
  const char *body,
  lm_ctrl_setup_portal_advanced_form_t *form
) {
  char preset_count_text[8] = {0};
  char temperature_step_text[8] = {0};
  char time_step_text[8] = {0};
  char confirm_text[8] = {0};
  char *endptr = NULL;

  if (body == NULL || form == NULL) {
    return false;
  }

  memset(form, 0, sizeof(*form));
  if (!lm_ctrl_setup_portal_parse_form_value(body, "preset_count", preset_count_text, sizeof(preset_count_text)) ||
      !lm_ctrl_setup_portal_parse_form_value(body, "temperature_step_c", temperature_step_text, sizeof(temperature_step_text)) ||
      !lm_ctrl_setup_portal_parse_form_value(body, "time_step_s", time_step_text, sizeof(time_step_text))) {
    return false;
  }

  form->preset_count = (uint8_t)strtol(preset_count_text, &endptr, 10);
  if (endptr == preset_count_text || *endptr != '\0') {
    return false;
  }

  form->temperature_step_c = strtof(temperature_step_text, &endptr);
  if (endptr == temperature_step_text || *endptr != '\0') {
    return false;
  }

  form->time_step_s = strtof(time_step_text, &endptr);
  if (endptr == time_step_text || *endptr != '\0') {
    return false;
  }

  lm_ctrl_setup_portal_parse_form_value(body, "preset_reduce_confirm", confirm_text, sizeof(confirm_text));
  form->preset_reduce_confirmed = strcmp(confirm_text, "1") == 0;
  return true;
}

bool lm_ctrl_setup_portal_validate_advanced_form(
  const lm_ctrl_setup_portal_advanced_form_t *form,
  const ctrl_state_t *current_state,
  char *error_text,
  size_t error_text_size
) {
  if (error_text != NULL && error_text_size > 0) {
    error_text[0] = '\0';
  }
  if (form == NULL || current_state == NULL || error_text == NULL || error_text_size == 0) {
    return false;
  }
  if (!ctrl_state_is_supported_preset_count((int)form->preset_count)) {
    snprintf(error_text, error_text_size, "Preset count must stay between 2 and %d.", CTRL_PRESET_MAX_COUNT);
    return false;
  }
  if (!ctrl_state_is_supported_edit_step(form->temperature_step_c)) {
    snprintf(error_text, error_text_size, "Temperature step must be 0.1 C or 0.5 C.");
    return false;
  }
  if (!ctrl_state_is_supported_edit_step(form->time_step_s)) {
    snprintf(error_text, error_text_size, "Time step must be 0.1 s or 0.5 s.");
    return false;
  }
  if (form->preset_count < current_state->preset_count && !form->preset_reduce_confirmed) {
    snprintf(error_text, error_text_size, "Confirm the preset deletion before reducing the preset count.");
    return false;
  }
  return true;
}
