#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bitmask of machine-facing fields that can be read or written by the link layer. */
typedef enum {
  LM_CTRL_MACHINE_FIELD_NONE = 0,
  LM_CTRL_MACHINE_FIELD_TEMPERATURE = 1 << 0,
  LM_CTRL_MACHINE_FIELD_STEAM = 1 << 1,
  LM_CTRL_MACHINE_FIELD_STANDBY = 1 << 2,
  LM_CTRL_MACHINE_FIELD_INFUSE = 1 << 3,
  LM_CTRL_MACHINE_FIELD_PAUSE = 1 << 4,
  LM_CTRL_MACHINE_FIELD_BBW_MODE = 1 << 5,
  LM_CTRL_MACHINE_FIELD_BBW_DOSE_1 = 1 << 6,
  LM_CTRL_MACHINE_FIELD_BBW_DOSE_2 = 1 << 7,
  LM_CTRL_MACHINE_FIELD_PREBREWING = LM_CTRL_MACHINE_FIELD_INFUSE | LM_CTRL_MACHINE_FIELD_PAUSE,
  LM_CTRL_MACHINE_FIELD_BBW = LM_CTRL_MACHINE_FIELD_BBW_MODE | LM_CTRL_MACHINE_FIELD_BBW_DOSE_1 | LM_CTRL_MACHINE_FIELD_BBW_DOSE_2,
} lm_ctrl_machine_field_t;

/** Optional feature flags surfaced by the machine dashboard. */
typedef enum {
  LM_CTRL_MACHINE_FEATURE_NONE = 0,
  LM_CTRL_MACHINE_FEATURE_BBW = 1 << 0,
} lm_ctrl_machine_feature_t;

/** Warmup status surfaced by the cloud dashboard for the selected machine. */
typedef struct {
  bool available;
  bool heating;
  bool eta_available;
  bool coffee_heating;
  bool coffee_eta_available;
  bool steam_heating;
  bool steam_eta_available;
  int64_t observed_epoch_ms;
  int64_t ready_epoch_ms;
  int64_t coffee_ready_epoch_ms;
  int64_t steam_ready_epoch_ms;
} lm_ctrl_machine_heat_info_t;

/** Sync sources that can be requested from the machine link worker. */
typedef enum {
  LM_CTRL_MACHINE_SYNC_NONE = 0,
  LM_CTRL_MACHINE_SYNC_BLE = 1 << 0,
  LM_CTRL_MACHINE_SYNC_CLOUD = 1 << 1,
  LM_CTRL_MACHINE_SYNC_ALL = LM_CTRL_MACHINE_SYNC_BLE | LM_CTRL_MACHINE_SYNC_CLOUD,
} lm_ctrl_machine_sync_mode_t;

/** Final command states surfaced through the cloud dashboard websocket. */
typedef enum {
  LM_CTRL_CLOUD_COMMAND_STATUS_UNKNOWN = 0,
  LM_CTRL_CLOUD_COMMAND_STATUS_SUCCESS,
  LM_CTRL_CLOUD_COMMAND_STATUS_ERROR,
  LM_CTRL_CLOUD_COMMAND_STATUS_TIMEOUT,
  LM_CTRL_CLOUD_COMMAND_STATUS_PENDING,
  LM_CTRL_CLOUD_COMMAND_STATUS_IN_PROGRESS,
} lm_ctrl_cloud_command_status_t;

/** Command update extracted from the cloud dashboard websocket `commands[]` list. */
typedef struct {
  char command_id[64];
  char error_code[32];
  lm_ctrl_cloud_command_status_t status;
} lm_ctrl_cloud_command_update_t;

/** Runtime status for BLE/cloud connectivity and the machine link worker. */
typedef struct {
  bool connected;
  bool authenticated;
  bool pending_work;
  bool sync_pending;
  uint32_t pending_mask;
  uint32_t sync_flags;
  uint32_t loaded_mask;
  uint32_t feature_mask;
} lm_ctrl_machine_link_info_t;

#ifdef __cplusplus
}
#endif
