#include "machine_link_policy.h"

bool lm_ctrl_machine_should_prefer_cloud_power_wakeup(
  const ctrl_values_t *desired_values,
  const ctrl_values_t *remote_values,
  uint32_t remote_loaded_mask,
  uint32_t pending_mask
) {
  if (desired_values == NULL || remote_values == NULL) {
    return false;
  }

  if ((pending_mask & LM_CTRL_MACHINE_FIELD_STANDBY) == 0) {
    return false;
  }
  if (desired_values->standby_on) {
    return false;
  }
  if ((remote_loaded_mask & LM_CTRL_MACHINE_FIELD_STANDBY) == 0) {
    return false;
  }

  return remote_values->standby_on;
}

bool lm_ctrl_machine_should_request_periodic_ble_sync(
  bool ble_binding_available,
  bool ble_authenticated
) {
  return ble_binding_available || ble_authenticated;
}
