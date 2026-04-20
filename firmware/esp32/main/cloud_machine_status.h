#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Machine reachability fields mirrored from the La Marzocco cloud thing metadata. */
typedef struct {
  bool connected_known;
  bool connected;
  bool offline_mode_known;
  bool offline_mode;
} lm_ctrl_cloud_machine_status_t;

static inline bool lm_ctrl_cloud_machine_status_is_known(const lm_ctrl_cloud_machine_status_t *status) {
  return status != NULL && (status->connected_known || status->offline_mode_known);
}

static inline bool lm_ctrl_cloud_machine_status_is_online(const lm_ctrl_cloud_machine_status_t *status) {
  if (status == NULL) {
    return false;
  }

  if (status->connected_known) {
    return status->connected && (!status->offline_mode_known || !status->offline_mode);
  }
  if (status->offline_mode_known) {
    return !status->offline_mode;
  }

  return false;
}

#ifdef __cplusplus
}
#endif
