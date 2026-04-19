#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "controller_state.h"
#include "machine_link_types.h"
#include "wifi_setup_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Remote-path state used by the on-device UI to color the Wi-Fi indicator. */
typedef enum {
  LM_CTRL_REMOTE_PATH_HIDDEN = 0,
  LM_CTRL_REMOTE_PATH_CONNECTING,
  LM_CTRL_REMOTE_PATH_NETWORK,
  LM_CTRL_REMOTE_PATH_CLOUD,
  LM_CTRL_REMOTE_PATH_MACHINE_ONLINE,
} lm_ctrl_remote_path_state_t;

/** Derived field availability for the current transport state. */
typedef struct {
  lm_ctrl_remote_path_state_t remote_path_state;
  uint32_t readable_mask;
  uint32_t editable_mask;
  bool preset_load_enabled;
} lm_ctrl_controller_access_t;

/** Focus presentation state used by the UI to suppress stale values. */
typedef enum {
  LM_CTRL_FIELD_PRESENTATION_UNAVAILABLE = 0,
  LM_CTRL_FIELD_PRESENTATION_LOADING,
  LM_CTRL_FIELD_PRESENTATION_READY,
} lm_ctrl_field_presentation_t;

/** Icon emphasis used by the UI for the remote-path indicator. */
typedef enum {
  LM_CTRL_INDICATOR_HIDDEN = 0,
  LM_CTRL_INDICATOR_CROSSED,
  LM_CTRL_INDICATOR_ACTIVE,
} lm_ctrl_indicator_state_t;

/** Return the machine field bit for one UI focus. */
uint32_t lm_ctrl_machine_field_for_focus(ctrl_focus_t focus);
/** Return the field mask needed to apply the active preset to the machine. */
uint32_t lm_ctrl_machine_preset_field_mask(uint32_t feature_mask);
/** Return the current presentation state for one focus from readable and loaded field masks. */
lm_ctrl_field_presentation_t lm_ctrl_controller_field_presentation(
  uint32_t readable_mask,
  uint32_t loaded_mask,
  ctrl_focus_t focus
);
/** Return whether one focus is editable under the current transport state. */
bool lm_ctrl_controller_field_is_editable(uint32_t editable_mask, ctrl_focus_t focus);
/** Map the remote-path state to one icon-emphasis tier for the Wi-Fi indicator. */
lm_ctrl_indicator_state_t lm_ctrl_remote_path_indicator_state(lm_ctrl_remote_path_state_t state);
/** Derive remote-path state plus readable/editable machine fields from current connectivity. */
void lm_ctrl_controller_compute_access(
  const lm_ctrl_wifi_info_t *wifi_info,
  const lm_ctrl_machine_link_info_t *machine_info,
  uint32_t feature_mask,
  lm_ctrl_controller_access_t *access
);

#ifdef __cplusplus
}
#endif
