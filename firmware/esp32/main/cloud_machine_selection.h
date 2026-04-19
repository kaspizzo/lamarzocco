#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cloud_api.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lm_ctrl_cloud_find_machine_by_serial(
  const char *serial,
  const lm_ctrl_cloud_machine_t *fleet,
  size_t fleet_count,
  lm_ctrl_cloud_machine_t *machine
);

bool lm_ctrl_cloud_resolve_effective_machine_selection(
  const lm_ctrl_cloud_machine_t *selected_machine,
  bool has_machine_selection,
  const lm_ctrl_cloud_machine_t *fleet,
  size_t fleet_count,
  lm_ctrl_cloud_machine_t *machine,
  bool *auto_selected
);

#ifdef __cplusplus
}
#endif
