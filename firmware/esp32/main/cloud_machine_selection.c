#include "cloud_machine_selection.h"

#include <string.h>

bool lm_ctrl_cloud_find_machine_by_serial(
  const char *serial,
  const lm_ctrl_cloud_machine_t *fleet,
  size_t fleet_count,
  lm_ctrl_cloud_machine_t *machine
) {
  size_t index;

  if (machine != NULL) {
    memset(machine, 0, sizeof(*machine));
  }
  if (serial == NULL || serial[0] == '\0' || fleet == NULL) {
    return false;
  }

  for (index = 0; index < fleet_count; ++index) {
    if (strcmp(serial, fleet[index].serial) == 0) {
      if (machine != NULL) {
        *machine = fleet[index];
      }
      return true;
    }
  }

  return false;
}

bool lm_ctrl_cloud_resolve_effective_machine_selection(
  const lm_ctrl_cloud_machine_t *selected_machine,
  bool has_machine_selection,
  const lm_ctrl_cloud_machine_t *fleet,
  size_t fleet_count,
  lm_ctrl_cloud_machine_t *machine,
  bool *auto_selected
) {
  if (machine != NULL) {
    memset(machine, 0, sizeof(*machine));
  }
  if (auto_selected != NULL) {
    *auto_selected = false;
  }

  if (has_machine_selection && selected_machine != NULL && selected_machine->serial[0] != '\0') {
    if (machine != NULL) {
      *machine = *selected_machine;
    }
    return true;
  }

  if (fleet != NULL && fleet_count == 1 && fleet[0].serial[0] != '\0') {
    if (machine != NULL) {
      *machine = fleet[0];
    }
    if (auto_selected != NULL) {
      *auto_selected = true;
    }
    return true;
  }

  return false;
}
