#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "controller_state.h"
#include "controller_ui.h"
#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  ctrl_values_t values;
  uint32_t mask;
  int64_t expires_us;
} lm_ctrl_runtime_local_value_hold_t;

typedef struct {
  uint32_t mask;
  int64_t due_us;
} lm_ctrl_runtime_delayed_machine_send_t;

typedef struct {
  bool heating;
  int64_t deadline_local_us;
  int64_t duration_us;
  int32_t last_rendered_remaining_s;
  int32_t last_rendered_progress_permille;
} lm_ctrl_runtime_heat_state_t;

typedef struct {
  int64_t until_us;
  int64_t next_request_us;
} lm_ctrl_runtime_heat_refresh_t;

/** Runtime-owned controller state, status text, and sync bookkeeping. */
typedef struct {
  ctrl_state_t state;
  char status[256];
  uint32_t last_wifi_status_version;
  uint32_t last_power_status_version;
  uint32_t last_machine_status_version;
  uint32_t last_preset_version;
  int64_t last_cloud_probe_request_us;
  int64_t last_ble_refresh_request_us;
  int64_t last_cloud_refresh_request_us;
  lm_ctrl_runtime_local_value_hold_t local_value_hold;
  lm_ctrl_runtime_delayed_machine_send_t delayed_machine_send;
  lm_ctrl_runtime_heat_state_t heat_state;
  lm_ctrl_runtime_heat_refresh_t heat_refresh;
} lm_ctrl_runtime_t;

void lm_ctrl_runtime_init(lm_ctrl_runtime_t *runtime);
void lm_ctrl_runtime_bootstrap(lm_ctrl_runtime_t *runtime);
void lm_ctrl_runtime_handle_input_event(lm_ctrl_runtime_t *runtime, const lm_ctrl_input_event_t *event, bool *needs_render);
void lm_ctrl_runtime_handle_wifi_status_change(lm_ctrl_runtime_t *runtime, bool *needs_render);
void lm_ctrl_runtime_handle_power_status_change(lm_ctrl_runtime_t *runtime, bool *needs_render);
void lm_ctrl_runtime_handle_machine_status_change(lm_ctrl_runtime_t *runtime, bool *needs_render);
void lm_ctrl_runtime_handle_preset_change(lm_ctrl_runtime_t *runtime, bool *needs_render);
void lm_ctrl_runtime_tick(lm_ctrl_runtime_t *runtime, bool *needs_render);
const ctrl_state_t *lm_ctrl_runtime_state(const lm_ctrl_runtime_t *runtime);
const char *lm_ctrl_runtime_status(const lm_ctrl_runtime_t *runtime);
void lm_ctrl_runtime_build_ui_view(const lm_ctrl_runtime_t *runtime, lm_ctrl_ui_view_t *view);

#ifdef __cplusplus
}
#endif
