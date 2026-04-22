/**
 * Machine link core state, worker orchestration, and public API.
 */
#include "machine_link_internal.h"

#include <stdarg.h>
#include <string.h>

static const char *TAG = "lm_ble";

typedef enum {
  LM_CTRL_MACHINE_VALUE_FIELD_FLOAT = 0,
  LM_CTRL_MACHINE_VALUE_FIELD_BOOL,
  LM_CTRL_MACHINE_VALUE_FIELD_ENUM,
} lm_ctrl_machine_value_field_kind_t;

typedef struct {
  uint32_t field_mask;
  size_t offset;
  lm_ctrl_machine_value_field_kind_t kind;
} lm_ctrl_machine_value_field_desc_t;

#define LM_CTRL_MACHINE_VALUE_FIELD_FLOAT_DESC(field_mask_, member_) \
  { (field_mask_), offsetof(ctrl_values_t, member_), LM_CTRL_MACHINE_VALUE_FIELD_FLOAT }
#define LM_CTRL_MACHINE_VALUE_FIELD_BOOL_DESC(field_mask_, member_) \
  { (field_mask_), offsetof(ctrl_values_t, member_), LM_CTRL_MACHINE_VALUE_FIELD_BOOL }
#define LM_CTRL_MACHINE_VALUE_FIELD_ENUM_DESC(field_mask_, member_) \
  { (field_mask_), offsetof(ctrl_values_t, member_), LM_CTRL_MACHINE_VALUE_FIELD_ENUM }

static const lm_ctrl_machine_value_field_desc_t s_machine_value_fields[] = {
  LM_CTRL_MACHINE_VALUE_FIELD_FLOAT_DESC(LM_CTRL_MACHINE_FIELD_TEMPERATURE, temperature_c),
  LM_CTRL_MACHINE_VALUE_FIELD_FLOAT_DESC(LM_CTRL_MACHINE_FIELD_INFUSE, infuse_s),
  LM_CTRL_MACHINE_VALUE_FIELD_FLOAT_DESC(LM_CTRL_MACHINE_FIELD_PAUSE, pause_s),
  LM_CTRL_MACHINE_VALUE_FIELD_ENUM_DESC(LM_CTRL_MACHINE_FIELD_STEAM, steam_level),
  LM_CTRL_MACHINE_VALUE_FIELD_BOOL_DESC(LM_CTRL_MACHINE_FIELD_STANDBY, standby_on),
  LM_CTRL_MACHINE_VALUE_FIELD_ENUM_DESC(LM_CTRL_MACHINE_FIELD_BBW_MODE, bbw_mode),
  LM_CTRL_MACHINE_VALUE_FIELD_FLOAT_DESC(LM_CTRL_MACHINE_FIELD_BBW_DOSE_1, bbw_dose_1_g),
  LM_CTRL_MACHINE_VALUE_FIELD_FLOAT_DESC(LM_CTRL_MACHINE_FIELD_BBW_DOSE_2, bbw_dose_2_g),
};

#define LM_CTRL_ARRAY_LEN(array_) (sizeof(array_) / sizeof((array_)[0]))

lm_ctrl_machine_link_state_t s_link = {
  .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};
lm_ctrl_pending_cloud_command_t s_pending_cloud_commands[LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS];
lm_ctrl_machine_link_deps_t s_deps = {0};
portMUX_TYPE s_link_lock = portMUX_INITIALIZER_UNLOCKED;

static bool machine_value_field_equal(
  const lm_ctrl_machine_value_field_desc_t *field,
  const ctrl_values_t *lhs,
  const ctrl_values_t *rhs
) {
  const uint8_t *lhs_bytes = (const uint8_t *)lhs;
  const uint8_t *rhs_bytes = (const uint8_t *)rhs;

  if (field == NULL || lhs == NULL || rhs == NULL) {
    return false;
  }

  switch (field->kind) {
    case LM_CTRL_MACHINE_VALUE_FIELD_FLOAT:
      return approx_equal(
        *(const float *)(lhs_bytes + field->offset),
        *(const float *)(rhs_bytes + field->offset)
      );
    case LM_CTRL_MACHINE_VALUE_FIELD_BOOL:
      return *(const bool *)(lhs_bytes + field->offset) == *(const bool *)(rhs_bytes + field->offset);
    case LM_CTRL_MACHINE_VALUE_FIELD_ENUM:
      return *(const int *)(const void *)(lhs_bytes + field->offset) ==
             *(const int *)(const void *)(rhs_bytes + field->offset);
    default:
      return false;
  }
}

static void machine_value_field_copy(
  const lm_ctrl_machine_value_field_desc_t *field,
  ctrl_values_t *dst,
  const ctrl_values_t *src
) {
  uint8_t *dst_bytes = (uint8_t *)dst;
  const uint8_t *src_bytes = (const uint8_t *)src;

  if (field == NULL || dst == NULL || src == NULL) {
    return;
  }

  switch (field->kind) {
    case LM_CTRL_MACHINE_VALUE_FIELD_FLOAT:
      *(float *)(dst_bytes + field->offset) = *(const float *)(src_bytes + field->offset);
      break;
    case LM_CTRL_MACHINE_VALUE_FIELD_BOOL:
      *(bool *)(dst_bytes + field->offset) = *(const bool *)(src_bytes + field->offset);
      break;
    case LM_CTRL_MACHINE_VALUE_FIELD_ENUM:
      *(int *)(void *)(dst_bytes + field->offset) = *(const int *)(const void *)(src_bytes + field->offset);
      break;
    default:
      break;
  }
}

static bool machine_value_field_copy_if_changed(
  const lm_ctrl_machine_value_field_desc_t *field,
  ctrl_values_t *dst,
  const ctrl_values_t *src
) {
  if (field == NULL || dst == NULL || src == NULL || machine_value_field_equal(field, dst, src)) {
    return false;
  }

  machine_value_field_copy(field, dst, src);
  return true;
}

static bool should_accept_remote_field_locked(
  const lm_ctrl_machine_value_field_desc_t *field,
  const ctrl_values_t *incoming_values
) {
  const uint32_t protected_mask = s_link.pending_mask | s_link.inflight_cloud_mask;

  if (field == NULL || incoming_values == NULL) {
    return false;
  }
  if ((protected_mask & field->field_mask) == 0) {
    return true;
  }

  return machine_value_field_equal(field, &s_link.desired_values, incoming_values);
}

static bool heat_info_equal(const lm_ctrl_machine_heat_info_t *lhs, const lm_ctrl_machine_heat_info_t *rhs) {
  if (lhs == NULL || rhs == NULL) {
    return lhs == rhs;
  }

  return lhs->available == rhs->available &&
         lhs->heating == rhs->heating &&
         lhs->eta_available == rhs->eta_available &&
         lhs->coffee_heating == rhs->coffee_heating &&
         lhs->coffee_eta_available == rhs->coffee_eta_available &&
         lhs->steam_heating == rhs->steam_heating &&
         lhs->steam_eta_available == rhs->steam_eta_available &&
         lhs->observed_epoch_ms == rhs->observed_epoch_ms &&
         lhs->ready_epoch_ms == rhs->ready_epoch_ms &&
         lhs->coffee_ready_epoch_ms == rhs->coffee_ready_epoch_ms &&
         lhs->steam_ready_epoch_ms == rhs->steam_ready_epoch_ms;
}

static bool water_status_equal(const lm_ctrl_machine_water_status_t *lhs, const lm_ctrl_machine_water_status_t *rhs) {
  if (lhs == NULL || rhs == NULL) {
    return lhs == rhs;
  }

  return lhs->available == rhs->available &&
         lhs->no_water == rhs->no_water;
}

static bool heat_hint_active_locked(void) {
  if (!s_link.local_heat_hint_active) {
    return false;
  }
  if (s_link.local_heat_hint_until_us > 0 &&
      esp_timer_get_time() >= s_link.local_heat_hint_until_us) {
    s_link.local_heat_hint_active = false;
    s_link.local_heat_hint_until_us = 0;
    s_link.status_version++;
    return false;
  }

  return true;
}
static lm_ctrl_machine_water_status_t preferred_water_status_locked(void) {
  if (s_link.ble_water_status.available) {
    return s_link.ble_water_status;
  }

  return s_link.cloud_water_status;
}

static ctrl_steam_level_t preferred_non_off_steam_level_locked(void) {
  if (ctrl_steam_level_enabled(s_link.reported_values.steam_level)) {
    return ctrl_steam_level_normalize(s_link.reported_values.steam_level);
  }
  if (ctrl_steam_level_enabled(s_link.desired_values.steam_level)) {
    return ctrl_steam_level_normalize(s_link.desired_values.steam_level);
  }
  return CTRL_STEAM_LEVEL_2;
}

ctrl_steam_level_t snapshot_preferred_steam_level(void) {
  ctrl_steam_level_t level;

  portENTER_CRITICAL(&s_link_lock);
  level = preferred_non_off_steam_level_locked();
  portEXIT_CRITICAL(&s_link_lock);

  return level;
}


void set_statusf(const char *fmt, ...) {
  char buffer[LM_CTRL_MACHINE_STATUS_TEXT_LEN];
  va_list args;

  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  portENTER_CRITICAL(&s_link_lock);
  copy_text(s_link.status_text, sizeof(s_link.status_text), buffer);
  s_link.status_version++;
  portEXIT_CRITICAL(&s_link_lock);

  ESP_LOGI(TAG, "%s", buffer);
}

void clear_connection_state_locked(void) {
  const lm_ctrl_machine_water_status_t previous_water_status = preferred_water_status_locked();
  lm_ctrl_machine_water_status_t current_water_status;

  s_link.scanning = false;
  s_link.connect_after_scan = false;
  s_link.connect_in_progress = false;
  s_link.connected = false;
  s_link.handles_ready = false;
  s_link.authenticated = false;
  s_link.conn_handle = BLE_HS_CONN_HANDLE_NONE;
  s_link.read_handle = 0;
  s_link.write_handle = 0;
  s_link.auth_handle = 0;
  s_link.att_mtu = BLE_ATT_MTU_DFLT;
  s_link.read_properties = 0;
  s_link.write_properties = 0;
  s_link.auth_properties = 0;
  s_link.mtu_ready = false;
  s_link.discovered_def_handle = 0;
  s_link.discovered_properties = 0;
  s_link.ble_water_status = (lm_ctrl_machine_water_status_t){0};

  current_water_status = preferred_water_status_locked();
  if (!water_status_equal(&previous_water_status, &current_water_status)) {
    s_link.status_version++;
  }
}

void snapshot_desired_values(ctrl_values_t *values) {
  if (values == NULL) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  *values = s_link.desired_values;
  portEXIT_CRITICAL(&s_link_lock);
}

void snapshot_remote_values(ctrl_values_t *values, uint32_t *loaded_mask) {
  portENTER_CRITICAL(&s_link_lock);
  if (values != NULL) {
    *values = s_link.remote_values;
  }
  if (loaded_mask != NULL) {
    *loaded_mask = s_link.remote_loaded_mask;
  }
  portEXIT_CRITICAL(&s_link_lock);
}

bool snapshot_ble_write_ready(void) {
  bool ready;

  portENTER_CRITICAL(&s_link_lock);
  ready = s_link.connected && s_link.handles_ready && s_link.authenticated;
  portEXIT_CRITICAL(&s_link_lock);

  return ready;
}

uint32_t snapshot_pending_mask(void) {
  uint32_t pending_mask;

  portENTER_CRITICAL(&s_link_lock);
  pending_mask = s_link.pending_mask & ~s_link.inflight_cloud_mask;
  portEXIT_CRITICAL(&s_link_lock);

  return pending_mask;
}

void mark_field_complete(uint32_t field_mask, const ctrl_values_t *sent_values) {
  bool needs_wakeup = false;

  portENTER_CRITICAL(&s_link_lock);
  for (size_t i = 0; i < LM_CTRL_ARRAY_LEN(s_machine_value_fields); ++i) {
    const lm_ctrl_machine_value_field_desc_t *field = &s_machine_value_fields[i];

    if ((field_mask & field->field_mask) != 0 &&
        machine_value_field_equal(field, &s_link.desired_values, sent_values)) {
      s_link.pending_mask &= ~field->field_mask;
    }
  }
  needs_wakeup = s_link.pending_mask != 0;
  portEXIT_CRITICAL(&s_link_lock);

  if (needs_wakeup && s_link.worker_task != NULL) {
    xTaskNotifyGive(s_link.worker_task);
  }
}

void clear_pending_mask(uint32_t field_mask) {
  portENTER_CRITICAL(&s_link_lock);
  s_link.pending_mask &= ~field_mask;
  portEXIT_CRITICAL(&s_link_lock);
}

void apply_values_to_reported_locked(const ctrl_values_t *values, uint32_t field_mask, bool *changed) {
  if (values == NULL) {
    return;
  }

  for (size_t i = 0; i < LM_CTRL_ARRAY_LEN(s_machine_value_fields); ++i) {
    const lm_ctrl_machine_value_field_desc_t *field = &s_machine_value_fields[i];

    if ((field_mask & field->field_mask) != 0 &&
        machine_value_field_copy_if_changed(field, &s_link.reported_values, values)) {
      *changed = true;
    }
  }
  if ((s_link.loaded_mask & field_mask) != field_mask) {
    s_link.loaded_mask |= field_mask;
    *changed = true;
  }
}

void mark_fields_inflight(uint32_t field_mask, const ctrl_values_t *sent_values) {
  bool changed = false;

  if (sent_values == NULL || field_mask == 0) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  for (size_t i = 0; i < LM_CTRL_ARRAY_LEN(s_machine_value_fields); ++i) {
    const lm_ctrl_machine_value_field_desc_t *field = &s_machine_value_fields[i];

    if ((field_mask & field->field_mask) != 0 &&
        (s_link.pending_mask & field->field_mask) != 0 &&
        machine_value_field_equal(field, &s_link.desired_values, sent_values)) {
      s_link.pending_mask &= ~field->field_mask;
      s_link.inflight_cloud_mask |= field->field_mask;
      changed = true;
    }
  }
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);
}

int find_pending_cloud_command_slot(const char *command_id) {
  size_t index;

  if (command_id == NULL || command_id[0] == '\0') {
    return -1;
  }

  for (index = 0; index < LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS; ++index) {
    if (s_pending_cloud_commands[index].active &&
        strcmp(s_pending_cloud_commands[index].command_id, command_id) == 0) {
      return (int)index;
    }
  }
  return -1;
}

static int allocate_pending_cloud_command_slot(void) {
  size_t index;

  for (index = 0; index < LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS; ++index) {
    if (!s_pending_cloud_commands[index].active) {
      return (int)index;
    }
  }
  return -1;
}

bool register_pending_cloud_command(const char *command_id, uint32_t field_mask, const ctrl_values_t *sent_values) {
  int slot;

  if (command_id == NULL || command_id[0] == '\0' || sent_values == NULL || field_mask == 0) {
    return false;
  }

  slot = find_pending_cloud_command_slot(command_id);
  if (slot < 0) {
    slot = allocate_pending_cloud_command_slot();
  }
  if (slot < 0) {
    set_statusf("Cloud command accepted, but no pending slot was available.");
    return false;
  }

  s_pending_cloud_commands[slot].active = true;
  copy_text(s_pending_cloud_commands[slot].command_id, sizeof(s_pending_cloud_commands[slot].command_id), command_id);
  s_pending_cloud_commands[slot].field_mask = field_mask;
  s_pending_cloud_commands[slot].started_us = esp_timer_get_time();
  s_pending_cloud_commands[slot].sent_values = *sent_values;
  s_pending_cloud_commands[slot].last_status = LM_CTRL_CLOUD_COMMAND_STATUS_PENDING;

  mark_fields_inflight(field_mask, sent_values);
  return true;
}

void wake_machine_worker(void) {
  if (s_link.worker_task != NULL) {
    xTaskNotifyGive(s_link.worker_task);
  }
}

void mark_ble_failure_now(void) {
  portENTER_CRITICAL(&s_link_lock);
  s_link.last_ble_failure_us = esp_timer_get_time();
  portEXIT_CRITICAL(&s_link_lock);
}

void clear_ble_failure(void) {
  portENTER_CRITICAL(&s_link_lock);
  s_link.last_ble_failure_us = 0;
  portEXIT_CRITICAL(&s_link_lock);
}

void update_reported_values(const ctrl_values_t *values, uint32_t loaded_mask) {
  bool changed = false;

  if (values == NULL || loaded_mask == 0) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  for (size_t i = 0; i < LM_CTRL_ARRAY_LEN(s_machine_value_fields); ++i) {
    const lm_ctrl_machine_value_field_desc_t *field = &s_machine_value_fields[i];

    if ((loaded_mask & field->field_mask) != 0) {
      machine_value_field_copy(field, &s_link.remote_values, values);
    }
  }
  s_link.remote_loaded_mask |= loaded_mask;

  for (size_t i = 0; i < LM_CTRL_ARRAY_LEN(s_machine_value_fields); ++i) {
    const lm_ctrl_machine_value_field_desc_t *field = &s_machine_value_fields[i];

    if ((loaded_mask & field->field_mask) != 0 &&
        should_accept_remote_field_locked(field, values) &&
        machine_value_field_copy_if_changed(field, &s_link.reported_values, values)) {
      changed = true;
    }
  }
  if ((s_link.loaded_mask & loaded_mask) != loaded_mask) {
    s_link.loaded_mask |= loaded_mask;
    changed = true;
  }
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);
}

void update_feature_mask(uint32_t feature_mask) {
  bool changed = false;

  portENTER_CRITICAL(&s_link_lock);
  if (s_link.feature_mask != feature_mask) {
    s_link.feature_mask = feature_mask;
    changed = true;
  }
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);
}

void update_heat_info(const lm_ctrl_machine_heat_info_t *info) {
  bool hint_was_active = false;

  if (info == NULL) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  hint_was_active = s_link.local_heat_hint_active || s_link.local_heat_hint_until_us != 0;
  s_link.local_heat_hint_active = false;
  s_link.local_heat_hint_until_us = 0;
  if (hint_was_active || !heat_info_equal(&s_link.heat_info, info)) {
    s_link.heat_info = *info;
    s_link.status_version++;
  } else {
    s_link.heat_info = *info;
  }
  portEXIT_CRITICAL(&s_link_lock);
}

void set_local_heat_hint_active(bool active) {
  bool changed = false;

  portENTER_CRITICAL(&s_link_lock);
  if (active) {
    if (!s_link.local_heat_hint_active) {
      changed = true;
    }
    s_link.local_heat_hint_active = true;
    s_link.local_heat_hint_until_us = esp_timer_get_time() + (15LL * 1000LL * 1000LL);
  } else {
    if (s_link.local_heat_hint_active || s_link.local_heat_hint_until_us != 0) {
      changed = true;
    }
    s_link.local_heat_hint_active = false;
    s_link.local_heat_hint_until_us = 0;
  }
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);
}
void update_cloud_water_status(const lm_ctrl_machine_water_status_t *status) {
  lm_ctrl_machine_water_status_t previous_water_status;
  lm_ctrl_machine_water_status_t current_water_status;

  if (status == NULL) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  previous_water_status = preferred_water_status_locked();
  s_link.cloud_water_status = *status;
  current_water_status = preferred_water_status_locked();
  if (!water_status_equal(&previous_water_status, &current_water_status)) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);
}

void update_ble_water_status(const lm_ctrl_machine_water_status_t *status) {
  lm_ctrl_machine_water_status_t previous_water_status;
  lm_ctrl_machine_water_status_t current_water_status;

  if (status == NULL) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  previous_water_status = preferred_water_status_locked();
  s_link.ble_water_status = *status;
  current_water_status = preferred_water_status_locked();
  if (!water_status_equal(&previous_water_status, &current_water_status)) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);
}

bool should_skip_ble_attempt(void) {
  int64_t last_failure_us;

  if (!machine_link_ble_transport_enabled()) {
    return true;
  }

  portENTER_CRITICAL(&s_link_lock);
  last_failure_us = s_link.last_ble_failure_us;
  portEXIT_CRITICAL(&s_link_lock);

  return last_failure_us > 0 && (esp_timer_get_time() - last_failure_us) < LM_CTRL_MACHINE_BLE_BACKOFF_US;
}

static bool has_sendable_work(const ctrl_values_t *values, uint32_t pending_mask) {
  if ((pending_mask & LM_CTRL_MACHINE_FIELD_PREBREWING) != 0) {
    return values != NULL;
  }
  if ((pending_mask & LM_CTRL_MACHINE_FIELD_BBW) != 0) {
    return values != NULL;
  }
  if ((pending_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0) {
    return true;
  }
  if (values == NULL) {
    return false;
  }
  if ((pending_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0) {
    return true;
  }
  if (values->standby_on) {
    return false;
  }
  return (pending_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0;
}


static void machine_link_worker(void *arg) {
  (void)arg;

  while (1) {
    ctrl_values_t desired_values;
    uint32_t pending_mask;
    lm_ctrl_machine_binding_t binding = {0};
    bool progress = false;
    uint32_t sync_request_flags = LM_CTRL_MACHINE_SYNC_NONE;

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LM_CTRL_MACHINE_WORKER_IDLE_WAIT_MS)) > 0) {
      while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LM_CTRL_MACHINE_DEBOUNCE_MS)) > 0) {
      }
    }

    expire_pending_cloud_commands();

    while (1) {
      portENTER_CRITICAL(&s_link_lock);
      desired_values = s_link.desired_values;
      pending_mask = s_link.pending_mask & ~s_link.inflight_cloud_mask;
      sync_request_flags = s_link.sync_request_flags;
      portEXIT_CRITICAL(&s_link_lock);

      if (pending_mask == 0 && sync_request_flags == LM_CTRL_MACHINE_SYNC_NONE) {
        break;
      }

      if (s_deps.get_machine_binding == NULL ||
          !s_deps.get_machine_binding(&binding) ||
          !binding.configured ||
          binding.serial[0] == '\0') {
        clear_pending_mask(pending_mask);
        portENTER_CRITICAL(&s_link_lock);
        s_link.sync_request_flags = LM_CTRL_MACHINE_SYNC_NONE;
        portEXIT_CRITICAL(&s_link_lock);
        set_statusf("No cloud machine selected.");
        break;
      }

      if (sync_request_flags != LM_CTRL_MACHINE_SYNC_NONE) {
        ctrl_values_t merged_values = {0};
        ctrl_values_t cloud_values = {0};
        ctrl_values_t ble_values = {0};
        lm_ctrl_machine_heat_info_t heat_info = {0};
        lm_ctrl_machine_water_status_t cloud_water_status = {0};
        lm_ctrl_machine_water_status_t ble_water_status = {0};
        uint32_t merged_mask = 0;
        uint32_t cloud_loaded_mask = 0;
        uint32_t ble_loaded_mask = 0;
        uint32_t feature_mask = 0;
        bool synced = false;

        if ((sync_request_flags & LM_CTRL_MACHINE_SYNC_CLOUD) != 0) {
          synced |= fetch_values_via_cloud(
            &cloud_values,
            &cloud_loaded_mask,
            &feature_mask,
            &heat_info,
            &cloud_water_status
          );
          if (cloud_loaded_mask != 0 || feature_mask != 0) {
            update_feature_mask(feature_mask);
          }
          if (heat_info.available) {
            update_heat_info(&heat_info);
          }
          if (cloud_water_status.available) {
            update_cloud_water_status(&cloud_water_status);
            synced = true;
          }
          if (cloud_loaded_mask != 0) {
            merged_values = cloud_values;
            merged_mask = cloud_loaded_mask;
            synced = true;
          }
        }

        if ((sync_request_flags & LM_CTRL_MACHINE_SYNC_BLE) != 0 &&
            fetch_values_via_ble(&binding, &ble_values, &ble_loaded_mask, &ble_water_status)) {
          if ((ble_loaded_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0) {
            merged_values.standby_on = ble_values.standby_on;
          }
          if ((ble_loaded_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0) {
            merged_values.temperature_c = ble_values.temperature_c;
          }
          if ((ble_loaded_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0) {
            merged_values.steam_level = ble_values.steam_level;
          }
          merged_mask |= ble_loaded_mask;
          if (ble_water_status.available) {
            update_ble_water_status(&ble_water_status);
          }
          synced = true;
        }

        if (merged_mask != 0) {
          update_reported_values(&merged_values, merged_mask);
        }

        portENTER_CRITICAL(&s_link_lock);
        s_link.sync_request_flags = LM_CTRL_MACHINE_SYNC_NONE;
        portEXIT_CRITICAL(&s_link_lock);

        if (synced) {
          set_statusf("Machine values synchronized.");
        }
      }

      if (!has_sendable_work(&desired_values, pending_mask)) {
        if (sync_request_flags == LM_CTRL_MACHINE_SYNC_NONE) {
          break;
        }
        continue;
      }

      progress = apply_prebrewing_via_cloud(&desired_values, pending_mask);

      portENTER_CRITICAL(&s_link_lock);
      pending_mask = s_link.pending_mask;
      portEXIT_CRITICAL(&s_link_lock);

      if (pending_mask != 0) {
        progress |= apply_bbw_via_cloud(&desired_values, pending_mask);
        snapshot_desired_values(&desired_values);
        pending_mask = snapshot_pending_mask();
      }

      if (pending_mask == 0) {
        break;
      }

      {
        ctrl_values_t remote_values = {0};
        uint32_t remote_loaded_mask = 0;

        snapshot_remote_values(&remote_values, &remote_loaded_mask);
        if (!snapshot_ble_write_ready() &&
            lm_ctrl_machine_should_prefer_cloud_power_wakeup(
              &desired_values,
              &remote_values,
              remote_loaded_mask,
              pending_mask
            )) {
          progress = apply_pending_via_cloud(&desired_values, pending_mask);
          if (progress) {
            continue;
          }
        }
      }

      if ((pending_mask & LM_CTRL_MACHINE_FIELD_BLE_MASK) == 0) {
        if (!progress) {
          vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_RETRY_DELAY_MS));
        }
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
        }
        continue;
      }

      if (binding.communication_key[0] == '\0') {
        progress = apply_pending_via_cloud(&desired_values, pending_mask);
        snapshot_desired_values(&desired_values);
        pending_mask = snapshot_pending_mask();
        if ((pending_mask & LM_CTRL_MACHINE_FIELD_BLE_MASK & ~LM_CTRL_MACHINE_FIELD_CLOUD_WRITE_MASK) != 0) {
          clear_pending_mask(pending_mask & LM_CTRL_MACHINE_FIELD_BLE_MASK & ~LM_CTRL_MACHINE_FIELD_CLOUD_WRITE_MASK);
          set_statusf("No BLE machine token configured.");
        }
        if (!progress) {
          break;
        }
        continue;
      }

      if (should_skip_ble_attempt()) {
        progress = apply_pending_via_cloud(&desired_values, pending_mask);
        if (!progress) {
          vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_RETRY_DELAY_MS));
        }
        continue;
      }

      {
        esp_err_t ble_ret = ensure_connected_and_authenticated(&binding);

        if (ble_ret == ESP_ERR_NOT_FINISHED) {
          continue;
        }
        if (ble_ret != ESP_OK) {
        mark_ble_failure_now();
        progress = apply_pending_via_cloud(&desired_values, pending_mask);
        if (!progress) {
          vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_RETRY_DELAY_MS));
        }
        continue;
      }
      }

      progress = false;
      snapshot_desired_values(&desired_values);
      pending_mask = snapshot_pending_mask();

      if ((pending_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0) {
        if (send_power_command(!desired_values.standby_on) == ESP_OK) {
          mark_field_complete(LM_CTRL_MACHINE_FIELD_STANDBY, &desired_values);
          progress = true;
        }
      }

      if ((pending_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0) {
        if (send_temperature_command(desired_values.temperature_c) == ESP_OK) {
          mark_field_complete(LM_CTRL_MACHINE_FIELD_TEMPERATURE, &desired_values);
          progress = true;
        }
      }

      if (!desired_values.standby_on && (pending_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0) {
        if (send_steam_command(desired_values.steam_level) == ESP_OK) {
          mark_field_complete(LM_CTRL_MACHINE_FIELD_STEAM, &desired_values);
          progress = true;
        }
      }

      if (!progress) {
        vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_RETRY_DELAY_MS));
      }

      while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
      }
    }
  }
}


esp_err_t lm_ctrl_machine_link_init(const lm_ctrl_machine_link_deps_t *deps) {
  int rc;

  if (deps == NULL ||
      deps->get_machine_binding == NULL ||
      deps->execute_cloud_command == NULL ||
      deps->fetch_dashboard_values == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (s_link.initialized) {
    return ESP_OK;
  }

  portENTER_CRITICAL(&s_link_lock);
  s_deps = *deps;
  s_link.initialized = true;
  portEXIT_CRITICAL(&s_link_lock);

  if (xTaskCreate(machine_link_worker, "lm_ble_worker", LM_CTRL_MACHINE_WORKER_STACK_SIZE, NULL, 5, &s_link.worker_task) != pdPASS) {
    portENTER_CRITICAL(&s_link_lock);
    s_link.initialized = false;
    s_link.worker_task = NULL;
    portEXIT_CRITICAL(&s_link_lock);
    return ESP_ERR_NO_MEM;
  }

  if (!machine_link_ble_transport_enabled()) {
    set_statusf("BLE transport disabled in build configuration.");
    return ESP_OK;
  }

  s_link.conn_sem = xSemaphoreCreateBinary();
  s_link.op_sem = xSemaphoreCreateBinary();
  s_link.mtu_sem = xSemaphoreCreateBinary();
  if (s_link.conn_sem == NULL || s_link.op_sem == NULL || s_link.mtu_sem == NULL) {
    return ESP_ERR_NO_MEM;
  }

  if (ble_uuid_from_str(&s_link.read_uuid, LM_CTRL_MACHINE_UUID_READ) != 0 ||
      ble_uuid_from_str(&s_link.write_uuid, LM_CTRL_MACHINE_UUID_WRITE) != 0 ||
      ble_uuid_from_str(&s_link.auth_uuid, LM_CTRL_MACHINE_UUID_AUTH) != 0) {
    return ESP_FAIL;
  }

  rc = nimble_port_init();
  if (rc != ESP_OK) {
    return rc;
  }

  ble_hs_cfg.reset_cb = machine_link_on_reset;
  ble_hs_cfg.sync_cb = machine_link_on_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_store_config_init();
  nimble_port_freertos_init(machine_link_host_task);

  set_statusf("BLE transport initialized.");
  return ESP_OK;
}

esp_err_t lm_ctrl_machine_link_queue_values(const ctrl_values_t *values, uint32_t field_mask) {
  bool changed = false;

  if (!s_link.initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (values == NULL || field_mask == LM_CTRL_MACHINE_FIELD_NONE) {
    return ESP_ERR_INVALID_ARG;
  }

  portENTER_CRITICAL(&s_link_lock);
  s_link.desired_values = *values;
  s_link.pending_mask |= field_mask;
  apply_values_to_reported_locked(values, field_mask, &changed);
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);

  ESP_LOGI(
    TAG,
    "Queued machine fields=0x%02x temp=%.1f inf=%.1f pause=%.1f steam=%d standby=%d bbw_mode=%s dose1=%.1f dose2=%.1f",
    (unsigned)field_mask,
    values->temperature_c,
    values->infuse_s,
    values->pause_s,
    (int)values->steam_level,
    values->standby_on,
    ctrl_bbw_mode_cloud_code(values->bbw_mode),
    values->bbw_dose_1_g,
    values->bbw_dose_2_g
  );

  if (s_link.worker_task != NULL) {
    xTaskNotifyGive(s_link.worker_task);
  }
  return ESP_OK;
}

esp_err_t lm_ctrl_machine_link_request_sync(void) {
  return lm_ctrl_machine_link_request_sync_mode(LM_CTRL_MACHINE_SYNC_ALL);
}

esp_err_t lm_ctrl_machine_link_request_sync_mode(uint32_t sync_flags) {
  if (!s_link.initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!machine_link_ble_transport_enabled()) {
    sync_flags &= ~LM_CTRL_MACHINE_SYNC_BLE;
  }
  if (sync_flags == LM_CTRL_MACHINE_SYNC_NONE) {
    return ESP_OK;
  }
  if (sync_flags == LM_CTRL_MACHINE_SYNC_NONE) {
    return ESP_ERR_INVALID_ARG;
  }

  portENTER_CRITICAL(&s_link_lock);
  s_link.sync_request_flags |= sync_flags;
  portEXIT_CRITICAL(&s_link_lock);

  if (s_link.worker_task != NULL) {
    xTaskNotifyGive(s_link.worker_task);
  }
  return ESP_OK;
}

bool lm_ctrl_machine_link_get_values(ctrl_values_t *values, uint32_t *loaded_mask, uint32_t *feature_mask) {
  if (values == NULL || loaded_mask == NULL || feature_mask == NULL) {
    return false;
  }

  portENTER_CRITICAL(&s_link_lock);
  *values = s_link.reported_values;
  *loaded_mask = s_link.loaded_mask;
  *feature_mask = s_link.feature_mask;
  portEXIT_CRITICAL(&s_link_lock);

  return *loaded_mask != 0 || *feature_mask != 0;
}

void lm_ctrl_machine_link_get_heat_info(lm_ctrl_machine_heat_info_t *info) {
  if (info == NULL) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  *info = s_link.heat_info;
  portEXIT_CRITICAL(&s_link_lock);
}

void lm_ctrl_machine_link_set_live_updates_state(bool active, bool connected) {
  bool changed = false;

  if (!s_link.initialized) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  if (s_link.cloud_live_updates_active != active ||
      s_link.cloud_live_updates_connected != connected) {
    s_link.cloud_live_updates_active = active;
    s_link.cloud_live_updates_connected = connected;
    s_link.status_version++;
    changed = true;
  } else {
    s_link.cloud_live_updates_active = active;
    s_link.cloud_live_updates_connected = connected;
  }
  portEXIT_CRITICAL(&s_link_lock);

  if (changed && !active) {
    wake_machine_worker();
  }
}


void lm_ctrl_machine_link_get_status(char *buffer, size_t buffer_size) {
  char local_buffer[LM_CTRL_MACHINE_STATUS_TEXT_LEN];

  if (buffer == NULL || buffer_size == 0) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  copy_text(local_buffer, sizeof(local_buffer), s_link.status_text);
  portEXIT_CRITICAL(&s_link_lock);

  copy_text(buffer, buffer_size, local_buffer);
}

void lm_ctrl_machine_link_get_info(lm_ctrl_machine_link_info_t *info) {
  if (info == NULL) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  info->connected = s_link.connected;
  info->authenticated = s_link.authenticated;
  info->pending_work = (s_link.pending_mask | s_link.inflight_cloud_mask) != 0;
  info->sync_pending = s_link.sync_request_flags != LM_CTRL_MACHINE_SYNC_NONE;
  info->heat_hint_active = heat_hint_active_locked();
  info->pending_mask = s_link.pending_mask | s_link.inflight_cloud_mask;
  info->sync_flags = s_link.sync_request_flags;
  info->loaded_mask = s_link.loaded_mask;
  info->feature_mask = s_link.feature_mask;
  info->water_status = preferred_water_status_locked();
  portEXIT_CRITICAL(&s_link_lock);
}

uint32_t lm_ctrl_machine_link_status_version(void) {
  uint32_t version;

  portENTER_CRITICAL(&s_link_lock);
  version = s_link.status_version;
  portEXIT_CRITICAL(&s_link_lock);

  return version;
}
