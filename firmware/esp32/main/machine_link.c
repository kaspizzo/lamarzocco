/**
 * Machine link core state, worker orchestration, and public API.
 */
#include "machine_link_internal.h"

#include <stdarg.h>
#include <string.h>

static const char *TAG = "lm_ble";

lm_ctrl_machine_link_state_t s_link = {
  .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};
lm_ctrl_pending_cloud_command_t s_pending_cloud_commands[LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS];
lm_ctrl_machine_link_deps_t s_deps = {0};
portMUX_TYPE s_link_lock = portMUX_INITIALIZER_UNLOCKED;

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
  if ((field_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      approx_equal(s_link.desired_values.temperature_c, sent_values->temperature_c)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_TEMPERATURE;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      s_link.desired_values.steam_level == sent_values->steam_level) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_STEAM;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      s_link.desired_values.standby_on == sent_values->standby_on) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_STANDBY;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      approx_equal(s_link.desired_values.infuse_s, sent_values->infuse_s)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_INFUSE;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      approx_equal(s_link.desired_values.pause_s, sent_values->pause_s)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_PAUSE;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      s_link.desired_values.bbw_mode == sent_values->bbw_mode) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_MODE;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      approx_equal(s_link.desired_values.bbw_dose_1_g, sent_values->bbw_dose_1_g)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_DOSE_1;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      approx_equal(s_link.desired_values.bbw_dose_2_g, sent_values->bbw_dose_2_g)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
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

  if ((field_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      !approx_equal(s_link.reported_values.temperature_c, values->temperature_c)) {
    s_link.reported_values.temperature_c = values->temperature_c;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      !approx_equal(s_link.reported_values.infuse_s, values->infuse_s)) {
    s_link.reported_values.infuse_s = values->infuse_s;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      !approx_equal(s_link.reported_values.pause_s, values->pause_s)) {
    s_link.reported_values.pause_s = values->pause_s;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      s_link.reported_values.steam_level != values->steam_level) {
    s_link.reported_values.steam_level = values->steam_level;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      s_link.reported_values.standby_on != values->standby_on) {
    s_link.reported_values.standby_on = values->standby_on;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      s_link.reported_values.bbw_mode != values->bbw_mode) {
    s_link.reported_values.bbw_mode = values->bbw_mode;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      !approx_equal(s_link.reported_values.bbw_dose_1_g, values->bbw_dose_1_g)) {
    s_link.reported_values.bbw_dose_1_g = values->bbw_dose_1_g;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      !approx_equal(s_link.reported_values.bbw_dose_2_g, values->bbw_dose_2_g)) {
    s_link.reported_values.bbw_dose_2_g = values->bbw_dose_2_g;
    *changed = true;
  }
  if ((s_link.loaded_mask & field_mask) != field_mask) {
    s_link.loaded_mask |= field_mask;
    *changed = true;
  }
}

static bool should_accept_remote_float_locked(uint32_t field_mask, float incoming_value) {
  const uint32_t protected_mask = s_link.pending_mask | s_link.inflight_cloud_mask;

  if ((protected_mask & field_mask) == 0) {
    return true;
  }

  switch (field_mask) {
    case LM_CTRL_MACHINE_FIELD_TEMPERATURE:
      return approx_equal(s_link.desired_values.temperature_c, incoming_value);
    case LM_CTRL_MACHINE_FIELD_INFUSE:
      return approx_equal(s_link.desired_values.infuse_s, incoming_value);
    case LM_CTRL_MACHINE_FIELD_PAUSE:
      return approx_equal(s_link.desired_values.pause_s, incoming_value);
    case LM_CTRL_MACHINE_FIELD_BBW_DOSE_1:
      return approx_equal(s_link.desired_values.bbw_dose_1_g, incoming_value);
    case LM_CTRL_MACHINE_FIELD_BBW_DOSE_2:
      return approx_equal(s_link.desired_values.bbw_dose_2_g, incoming_value);
    default:
      return true;
  }
}

static bool should_accept_remote_bool_locked(uint32_t field_mask, bool incoming_value) {
  const uint32_t protected_mask = s_link.pending_mask | s_link.inflight_cloud_mask;

  if ((protected_mask & field_mask) == 0) {
    return true;
  }

  switch (field_mask) {
    case LM_CTRL_MACHINE_FIELD_STANDBY:
      return s_link.desired_values.standby_on == incoming_value;
    default:
      return true;
  }
}

static bool should_accept_remote_steam_locked(ctrl_steam_level_t incoming_level) {
  const uint32_t protected_mask = s_link.pending_mask | s_link.inflight_cloud_mask;

  if ((protected_mask & LM_CTRL_MACHINE_FIELD_STEAM) == 0) {
    return true;
  }

  return s_link.desired_values.steam_level == incoming_level;
}

static bool should_accept_remote_int_locked(uint32_t field_mask, int incoming_value) {
  const uint32_t protected_mask = s_link.pending_mask | s_link.inflight_cloud_mask;

  if ((protected_mask & field_mask) == 0) {
    return true;
  }

  if (field_mask == LM_CTRL_MACHINE_FIELD_BBW_MODE) {
    return (int)s_link.desired_values.bbw_mode == incoming_value;
  }
  return true;
}

void mark_fields_inflight(uint32_t field_mask, const ctrl_values_t *sent_values) {
  bool changed = false;

  if (sent_values == NULL || field_mask == 0) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  if ((field_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      approx_equal(s_link.desired_values.temperature_c, sent_values->temperature_c)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_TEMPERATURE;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_TEMPERATURE;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      s_link.desired_values.steam_level == sent_values->steam_level) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_STEAM;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_STEAM;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      s_link.desired_values.standby_on == sent_values->standby_on) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_STANDBY;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_STANDBY;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      approx_equal(s_link.desired_values.infuse_s, sent_values->infuse_s)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_INFUSE;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_INFUSE;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      approx_equal(s_link.desired_values.pause_s, sent_values->pause_s)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_PAUSE;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_PAUSE;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      s_link.desired_values.bbw_mode == sent_values->bbw_mode) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_MODE;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_BBW_MODE;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      approx_equal(s_link.desired_values.bbw_dose_1_g, sent_values->bbw_dose_1_g)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_DOSE_1;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_BBW_DOSE_1;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      approx_equal(s_link.desired_values.bbw_dose_2_g, sent_values->bbw_dose_2_g)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
    changed = true;
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
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0) {
    s_link.remote_values.temperature_c = values->temperature_c;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0) {
    s_link.remote_values.infuse_s = values->infuse_s;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0) {
    s_link.remote_values.pause_s = values->pause_s;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0) {
    s_link.remote_values.steam_level = values->steam_level;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0) {
    s_link.remote_values.standby_on = values->standby_on;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0) {
    s_link.remote_values.bbw_mode = values->bbw_mode;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0) {
    s_link.remote_values.bbw_dose_1_g = values->bbw_dose_1_g;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0) {
    s_link.remote_values.bbw_dose_2_g = values->bbw_dose_2_g;
  }
  s_link.remote_loaded_mask |= loaded_mask;

  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      should_accept_remote_float_locked(LM_CTRL_MACHINE_FIELD_TEMPERATURE, values->temperature_c) &&
      !approx_equal(s_link.reported_values.temperature_c, values->temperature_c)) {
    s_link.reported_values.temperature_c = values->temperature_c;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      should_accept_remote_float_locked(LM_CTRL_MACHINE_FIELD_INFUSE, values->infuse_s) &&
      !approx_equal(s_link.reported_values.infuse_s, values->infuse_s)) {
    s_link.reported_values.infuse_s = values->infuse_s;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      should_accept_remote_float_locked(LM_CTRL_MACHINE_FIELD_PAUSE, values->pause_s) &&
      !approx_equal(s_link.reported_values.pause_s, values->pause_s)) {
    s_link.reported_values.pause_s = values->pause_s;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      should_accept_remote_steam_locked(values->steam_level) &&
      s_link.reported_values.steam_level != values->steam_level) {
    s_link.reported_values.steam_level = values->steam_level;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      should_accept_remote_bool_locked(LM_CTRL_MACHINE_FIELD_STANDBY, values->standby_on) &&
      s_link.reported_values.standby_on != values->standby_on) {
    s_link.reported_values.standby_on = values->standby_on;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      should_accept_remote_int_locked(LM_CTRL_MACHINE_FIELD_BBW_MODE, (int)values->bbw_mode) &&
      s_link.reported_values.bbw_mode != values->bbw_mode) {
    s_link.reported_values.bbw_mode = values->bbw_mode;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      should_accept_remote_float_locked(LM_CTRL_MACHINE_FIELD_BBW_DOSE_1, values->bbw_dose_1_g) &&
      !approx_equal(s_link.reported_values.bbw_dose_1_g, values->bbw_dose_1_g)) {
    s_link.reported_values.bbw_dose_1_g = values->bbw_dose_1_g;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      should_accept_remote_float_locked(LM_CTRL_MACHINE_FIELD_BBW_DOSE_2, values->bbw_dose_2_g) &&
      !approx_equal(s_link.reported_values.bbw_dose_2_g, values->bbw_dose_2_g)) {
    s_link.reported_values.bbw_dose_2_g = values->bbw_dose_2_g;
    changed = true;
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

bool should_skip_ble_attempt(void) {
  int64_t last_failure_us;

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
        uint32_t merged_mask = 0;
        uint32_t cloud_loaded_mask = 0;
        uint32_t ble_loaded_mask = 0;
        uint32_t feature_mask = 0;
        bool synced = false;

        if ((sync_request_flags & LM_CTRL_MACHINE_SYNC_CLOUD) != 0) {
          synced |= fetch_values_via_cloud(&cloud_values, &cloud_loaded_mask, &feature_mask);
          update_feature_mask(feature_mask);
          if (cloud_loaded_mask != 0) {
            merged_values = cloud_values;
            merged_mask = cloud_loaded_mask;
            synced = true;
          }
        }

        if ((sync_request_flags & LM_CTRL_MACHINE_SYNC_BLE) != 0 &&
            fetch_values_via_ble(&binding, &ble_values, &ble_loaded_mask)) {
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

  xTaskCreate(machine_link_worker, "lm_ble_worker", LM_CTRL_MACHINE_WORKER_STACK_SIZE, NULL, 5, &s_link.worker_task);
  nimble_port_freertos_init(machine_link_host_task);

  portENTER_CRITICAL(&s_link_lock);
  s_deps = *deps;
  s_link.initialized = true;
  portEXIT_CRITICAL(&s_link_lock);

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
  info->pending_mask = s_link.pending_mask | s_link.inflight_cloud_mask;
  info->sync_flags = s_link.sync_request_flags;
  info->loaded_mask = s_link.loaded_mask;
  info->feature_mask = s_link.feature_mask;
  portEXIT_CRITICAL(&s_link_lock);
}

uint32_t lm_ctrl_machine_link_status_version(void) {
  uint32_t version;

  portENTER_CRITICAL(&s_link_lock);
  version = s_link.status_version;
  portEXIT_CRITICAL(&s_link_lock);

  return version;
}
