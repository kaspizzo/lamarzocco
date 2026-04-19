#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "controller_state.h"
#include "machine_link_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LM_CTRL_MACHINE_FIELD_CLOUD_WRITE_MASK \
  (LM_CTRL_MACHINE_FIELD_TEMPERATURE | \
   LM_CTRL_MACHINE_FIELD_STEAM | \
   LM_CTRL_MACHINE_FIELD_STANDBY | \
   LM_CTRL_MACHINE_FIELD_PREBREWING | \
   LM_CTRL_MACHINE_FIELD_BBW)

bool lm_ctrl_machine_should_prefer_cloud_power_wakeup(
  const ctrl_values_t *desired_values,
  const ctrl_values_t *remote_values,
  uint32_t remote_loaded_mask,
  uint32_t pending_mask
);

bool lm_ctrl_machine_should_request_periodic_ble_sync(
  bool ble_binding_available,
  bool ble_authenticated
);

#ifdef __cplusplus
}
#endif
