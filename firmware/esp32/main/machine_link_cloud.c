/**
 * Cloud fetches, fallback writes, and websocket-driven machine state updates.
 */
#include "machine_link_internal.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "lm_ble";

static void rearm_ble_after_cloud_wakeup(void) {
  clear_ble_failure();
  (void)lm_ctrl_machine_link_request_sync_mode(LM_CTRL_MACHINE_SYNC_BLE);
}

bool fetch_values_via_cloud(
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask
) {
  if (values == NULL || loaded_mask == NULL || feature_mask == NULL) {
    return false;
  }

  if (s_link.cloud_live_updates_active) {
    return false;
  }

  *values = (ctrl_values_t){0};
  values->steam_level = snapshot_preferred_steam_level();
  *loaded_mask = 0;
  *feature_mask = 0;

  if (s_deps.fetch_dashboard_values == NULL ||
      s_deps.fetch_dashboard_values(values, loaded_mask, feature_mask) != ESP_OK) {
    return false;
  }

  return *loaded_mask != 0 || *feature_mask != 0;
}


static lm_ctrl_cloud_send_result_t send_cloud_command(
  const char *command,
  const char *payload,
  uint32_t field_mask,
  const ctrl_values_t *sent_values,
  const char *success_message,
  const char *pending_message,
  char *failure_status_text,
  size_t failure_status_text_size
) {
  char status_text[96];
  lm_ctrl_cloud_command_result_t result = {0};
  bool websocket_connected;

  portENTER_CRITICAL(&s_link_lock);
  websocket_connected = s_link.cloud_live_updates_connected;
  portEXIT_CRITICAL(&s_link_lock);

  if (s_deps.execute_cloud_command == NULL ||
      s_deps.execute_cloud_command(command, payload, &result, status_text, sizeof(status_text)) != ESP_OK) {
    if (failure_status_text != NULL && failure_status_text_size > 0) {
      copy_text(failure_status_text, failure_status_text_size, status_text);
    }
    set_statusf("Cloud fallback failed: %s", status_text[0] != '\0' ? status_text : "request error");
    return LM_CTRL_CLOUD_SEND_FAILED;
  }

  if (websocket_connected && result.command_id[0] != '\0' && register_pending_cloud_command(result.command_id, field_mask, sent_values)) {
    set_statusf("%s", pending_message != NULL ? pending_message : "Cloud command accepted, waiting for dashboard confirmation.");
    return LM_CTRL_CLOUD_SEND_WAITING_FOR_ACK;
  }

  set_statusf("%s", success_message);
  return LM_CTRL_CLOUD_SEND_APPLIED;
}

lm_ctrl_cloud_send_result_t send_power_command_cloud(bool enabled) {
  char payload[96];
  ctrl_values_t sent_values = {0};

  snprintf(
    payload,
    sizeof(payload),
    "{\"mode\":\"%s\"}",
    enabled ? "BrewingMode" : "StandBy"
  );
  sent_values.standby_on = !enabled;

  return send_cloud_command(
    "CoffeeMachineChangeMode",
    payload,
    LM_CTRL_MACHINE_FIELD_STANDBY,
    &sent_values,
    enabled ? "Cloud power command applied: BrewingMode" : "Cloud power command applied: StandBy",
    enabled ? "Cloud power command accepted, waiting for dashboard confirmation." : "Cloud standby command accepted, waiting for dashboard confirmation."
    ,
    NULL,
    0
  );
}

static lm_ctrl_cloud_send_result_t send_steam_enable_command_cloud(bool enabled, ctrl_steam_level_t level) {
  char payload[80];
  ctrl_values_t sent_values = {0};

  snprintf(
    payload,
    sizeof(payload),
    "{\"boilerIndex\":1,\"enabled\":%s}",
    enabled ? "true" : "false"
  );
  sent_values.steam_level = enabled ? ctrl_steam_level_normalize(level) : CTRL_STEAM_LEVEL_OFF;

  return send_cloud_command(
    "CoffeeMachineSettingSteamBoilerEnabled",
    payload,
    LM_CTRL_MACHINE_FIELD_STEAM,
    &sent_values,
    enabled ? "Cloud steam command applied: on" : "Cloud steam command applied: off",
    enabled ? "Cloud steam command accepted, waiting for dashboard confirmation." : "Cloud steam-off command accepted, waiting for dashboard confirmation."
    ,
    NULL,
    0
  );
}

static lm_ctrl_cloud_send_result_t send_steam_level_command_cloud(ctrl_steam_level_t level) {
  const char *target_level = steam_level_cloud_code(level);
  char payload[96];
  char success_message[64];
  ctrl_values_t sent_values = {0};

  if (target_level == NULL) {
    return LM_CTRL_CLOUD_SEND_FAILED;
  }

  snprintf(
    payload,
    sizeof(payload),
    "{\"boilerIndex\":1,\"targetLevel\":\"%s\"}",
    target_level
  );
  snprintf(success_message, sizeof(success_message), "Cloud steam level applied: %s", ctrl_steam_level_label(level));
  sent_values.steam_level = ctrl_steam_level_normalize(level);

  return send_cloud_command(
    "CoffeeMachineSettingSteamBoilerTargetLevel",
    payload,
    LM_CTRL_MACHINE_FIELD_NONE,
    &sent_values,
    success_message,
    NULL,
    NULL,
    0
  );
}

lm_ctrl_cloud_send_result_t send_steam_command_cloud(ctrl_steam_level_t level) {
  ctrl_steam_level_t normalized_level = ctrl_steam_level_normalize(level);

  if (!ctrl_steam_level_enabled(normalized_level)) {
    return send_steam_enable_command_cloud(false, CTRL_STEAM_LEVEL_OFF);
  }

  if (send_steam_level_command_cloud(normalized_level) == LM_CTRL_CLOUD_SEND_FAILED) {
    return LM_CTRL_CLOUD_SEND_FAILED;
  }

  return send_steam_enable_command_cloud(true, normalized_level);
}

lm_ctrl_cloud_send_result_t send_temperature_command_cloud(float temperature_c) {
  char payload[96];
  char success_message[64];
  char pending_message[96];
  ctrl_values_t sent_values = {0};

  snprintf(
    payload,
    sizeof(payload),
    "{\"boilerIndex\":1,\"targetTemperature\":%.1f}",
    (double)temperature_c
  );
  snprintf(success_message, sizeof(success_message), "Cloud temperature applied: %.1f C", (double)temperature_c);
  snprintf(
    pending_message,
    sizeof(pending_message),
    "Cloud temperature accepted, waiting for %.1f C confirmation.",
    (double)temperature_c
  );
  sent_values.temperature_c = temperature_c;

  return send_cloud_command(
    "CoffeeMachineSettingCoffeeBoilerTargetTemperature",
    payload,
    LM_CTRL_MACHINE_FIELD_TEMPERATURE,
    &sent_values,
    success_message,
    pending_message,
    NULL,
    0
  );
}

esp_err_t send_prebrewing_mode_cloud(const char *mode) {
  char payload[80];
  char status_text[96];

  if (mode == NULL || mode[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }

  snprintf(payload, sizeof(payload), "{\"mode\":\"%s\"}", mode);

  status_text[0] = '\0';
  if (s_deps.execute_cloud_command != NULL &&
      s_deps.execute_cloud_command(
        "CoffeeMachinePreBrewingChangeMode",
        payload,
        NULL,
        status_text,
        sizeof(status_text)
      ) == ESP_OK) {
    set_statusf("Cloud prebrewing mode applied: %s", mode);
    return ESP_OK;
  }

  if (strstr(status_text, "status 412") != NULL) {
    ESP_LOGW(TAG, "Cloud prebrewing mode change returned 412, trying times update anyway.");
    return ESP_ERR_NOT_SUPPORTED;
  }

  set_statusf("Cloud fallback failed: %s", status_text[0] != '\0' ? status_text : "request error");
  return ESP_FAIL;
}

lm_ctrl_cloud_send_result_t send_prebrewing_times_cloud(float infuse_s, float pause_s) {
  char payload[128];
  char success_message[80];
  char pending_message[96];
  char status_text[192];
  ctrl_values_t sent_values = {0};
  lm_ctrl_cloud_send_result_t result;

  snprintf(
    payload,
    sizeof(payload),
    "{\"times\":{\"In\":%.1f,\"Out\":%.1f},\"groupIndex\":1,\"doseIndex\":\"ByGroup\"}",
    (double)infuse_s,
    (double)pause_s
  );
  snprintf(success_message, sizeof(success_message), "Cloud prebrewing applied: %.1f s / %.1f s", (double)infuse_s, (double)pause_s);
  snprintf(
    pending_message,
    sizeof(pending_message),
    "Cloud prebrewing accepted, waiting for %.1f s / %.1f s.",
    (double)infuse_s,
    (double)pause_s
  );
  sent_values.infuse_s = infuse_s;
  sent_values.pause_s = pause_s;

  status_text[0] = '\0';
  result = send_cloud_command(
        "CoffeeMachinePreBrewingSettingTimes",
        payload,
        LM_CTRL_MACHINE_FIELD_PREBREWING,
        &sent_values,
        success_message,
        pending_message,
        status_text,
        sizeof(status_text)
      );
  if (result != LM_CTRL_CLOUD_SEND_FAILED) {
    return result;
  }

  if (strstr(status_text, "status 412") != NULL) {
    char dashboard_status[192];

    dashboard_status[0] = '\0';
    snprintf(dashboard_status, sizeof(dashboard_status), "status 412 while writing prebrewing values");
    set_statusf("Cloud prebrewing unavailable: %s", dashboard_status);
    clear_pending_mask(LM_CTRL_MACHINE_FIELD_PREBREWING);
    return LM_CTRL_CLOUD_SEND_FAILED;
  }

  set_statusf("Cloud fallback failed: %s", status_text[0] != '\0' ? status_text : "request error");
  return LM_CTRL_CLOUD_SEND_FAILED;
}

lm_ctrl_cloud_send_result_t send_bbw_mode_cloud(ctrl_bbw_mode_t mode) {
  char payload[80];
  char success_message[96];
  char pending_message[120];
  ctrl_values_t sent_values = {0};

  snprintf(payload, sizeof(payload), "{\"mode\":\"%s\"}", ctrl_bbw_mode_cloud_code(mode));
  snprintf(success_message, sizeof(success_message), "Cloud brew by weight mode applied: %s", ctrl_bbw_mode_cloud_code(mode));
  snprintf(
    pending_message,
    sizeof(pending_message),
    "Cloud brew by weight mode accepted, waiting for %s.",
    ctrl_bbw_mode_cloud_code(mode)
  );
  sent_values.bbw_mode = mode;

  return send_cloud_command(
    "CoffeeMachineBrewByWeightChangeMode",
    payload,
    LM_CTRL_MACHINE_FIELD_BBW_MODE,
    &sent_values,
    success_message,
    pending_message,
    NULL,
    0
  );
}

lm_ctrl_cloud_send_result_t send_bbw_doses_cloud(float dose_1_g, float dose_2_g) {
  char payload[128];
  char success_message[96];
  char pending_message[128];
  ctrl_values_t sent_values = {0};

  snprintf(
    payload,
    sizeof(payload),
    "{\"doses\":{\"Dose1\":%.1f,\"Dose2\":%.1f}}",
    (double)dose_1_g,
    (double)dose_2_g
  );
  snprintf(success_message, sizeof(success_message), "Cloud brew by weight doses applied: %.1f g / %.1f g", (double)dose_1_g, (double)dose_2_g);
  snprintf(
    pending_message,
    sizeof(pending_message),
    "Cloud brew by weight doses accepted, waiting for %.1f g / %.1f g.",
    (double)dose_1_g,
    (double)dose_2_g
  );
  sent_values.bbw_dose_1_g = dose_1_g;
  sent_values.bbw_dose_2_g = dose_2_g;

  return send_cloud_command(
    "CoffeeMachineBrewByWeightSettingDoses",
    payload,
    LM_CTRL_MACHINE_FIELD_BBW_DOSE_1 | LM_CTRL_MACHINE_FIELD_BBW_DOSE_2,
    &sent_values,
    success_message,
    pending_message,
    NULL,
    0
  );
}


bool apply_prebrewing_via_cloud(const ctrl_values_t *desired_values, uint32_t pending_mask) {
  ctrl_values_t current_values;
  lm_ctrl_cloud_send_result_t times_ret;

  if (desired_values == NULL || (pending_mask & LM_CTRL_MACHINE_FIELD_PREBREWING) == 0) {
    return false;
  }

  current_values = *desired_values;
  snapshot_desired_values(&current_values);

  esp_err_t mode_ret = send_prebrewing_mode_cloud("PreBrewing");
  if (mode_ret != ESP_OK && mode_ret != ESP_ERR_NOT_SUPPORTED) {
    return false;
  }
  snapshot_desired_values(&current_values);
  times_ret = send_prebrewing_times_cloud(current_values.infuse_s, current_values.pause_s);
  if (times_ret == LM_CTRL_CLOUD_SEND_FAILED && mode_ret == ESP_ERR_NOT_SUPPORTED) {
    clear_pending_mask(LM_CTRL_MACHINE_FIELD_PREBREWING);
    return false;
  }
  if (times_ret == LM_CTRL_CLOUD_SEND_FAILED) {
    return false;
  }
  if (times_ret == LM_CTRL_CLOUD_SEND_APPLIED) {
    mark_field_complete(LM_CTRL_MACHINE_FIELD_PREBREWING, &current_values);
  }
  return true;
}

bool apply_bbw_via_cloud(const ctrl_values_t *desired_values, uint32_t pending_mask) {
  ctrl_values_t current_values;
  uint32_t feature_mask;

  if (desired_values == NULL || (pending_mask & LM_CTRL_MACHINE_FIELD_BBW) == 0) {
    return false;
  }

  current_values = *desired_values;
  snapshot_desired_values(&current_values);

  portENTER_CRITICAL(&s_link_lock);
  feature_mask = s_link.feature_mask;
  portEXIT_CRITICAL(&s_link_lock);

  if ((feature_mask & LM_CTRL_MACHINE_FEATURE_BBW) == 0) {
    clear_pending_mask(LM_CTRL_MACHINE_FIELD_BBW);
    set_statusf("Brew by weight is not available for the selected machine.");
    return false;
  }

  if ((pending_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0) {
    lm_ctrl_cloud_send_result_t result = send_bbw_mode_cloud(current_values.bbw_mode);
    if (result == LM_CTRL_CLOUD_SEND_FAILED) {
      return false;
    }
    if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
      mark_field_complete(LM_CTRL_MACHINE_FIELD_BBW_MODE, &current_values);
    }
  }

  if ((pending_mask & (LM_CTRL_MACHINE_FIELD_BBW_DOSE_1 | LM_CTRL_MACHINE_FIELD_BBW_DOSE_2)) != 0) {
    snapshot_desired_values(&current_values);
    lm_ctrl_cloud_send_result_t result = send_bbw_doses_cloud(current_values.bbw_dose_1_g, current_values.bbw_dose_2_g);
    if (result == LM_CTRL_CLOUD_SEND_FAILED) {
      return false;
    }
    if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
      mark_field_complete(LM_CTRL_MACHINE_FIELD_BBW_DOSE_1 | LM_CTRL_MACHINE_FIELD_BBW_DOSE_2, &current_values);
    }
  }

  return true;
}

bool apply_pending_via_cloud(const ctrl_values_t *desired_values, uint32_t pending_mask) {
  ctrl_values_t current_values;
  ctrl_values_t remote_values = {0};
  uint32_t remote_loaded_mask = 0;
  bool progress = false;
  bool wakeup_first = false;

  if (desired_values == NULL) {
    return false;
  }

  current_values = *desired_values;
  snapshot_desired_values(&current_values);
  snapshot_remote_values(&remote_values, &remote_loaded_mask);
  wakeup_first = lm_ctrl_machine_should_prefer_cloud_power_wakeup(
    &current_values,
    &remote_values,
    remote_loaded_mask,
    pending_mask
  );

  if (wakeup_first) {
    lm_ctrl_cloud_send_result_t result = send_power_command_cloud(true);
    if (result != LM_CTRL_CLOUD_SEND_FAILED) {
      rearm_ble_after_cloud_wakeup();
      if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
        mark_field_complete(LM_CTRL_MACHINE_FIELD_STANDBY, &current_values);
      }
      return true;
    }
    return false;
  }

  if (!current_values.standby_on && (pending_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0) {
    lm_ctrl_cloud_send_result_t result = send_steam_command_cloud(current_values.steam_level);
    if (result != LM_CTRL_CLOUD_SEND_FAILED) {
      if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
        mark_field_complete(LM_CTRL_MACHINE_FIELD_STEAM, &current_values);
      }
      progress = true;
    }
  }

  if ((pending_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0) {
    lm_ctrl_cloud_send_result_t result = send_power_command_cloud(!current_values.standby_on);
    if (result != LM_CTRL_CLOUD_SEND_FAILED) {
      if (!current_values.standby_on) {
        rearm_ble_after_cloud_wakeup();
      }
      if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
        mark_field_complete(LM_CTRL_MACHINE_FIELD_STANDBY, &current_values);
      }
      progress = true;
    }
  }

  if ((pending_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0) {
    snapshot_desired_values(&current_values);
    lm_ctrl_cloud_send_result_t result = send_temperature_command_cloud(current_values.temperature_c);
    if (result != LM_CTRL_CLOUD_SEND_FAILED) {
      if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
        mark_field_complete(LM_CTRL_MACHINE_FIELD_TEMPERATURE, &current_values);
      }
      progress = true;
    }
  }

  return progress;
}


void expire_pending_cloud_commands(void) {
  const int64_t now_us = esp_timer_get_time();
  size_t index;
  bool changed = false;
  bool should_sync = false;
  bool should_wake = false;

  portENTER_CRITICAL(&s_link_lock);
  for (index = 0; index < LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS; ++index) {
    lm_ctrl_pending_cloud_command_t *pending = &s_pending_cloud_commands[index];

    if (!pending->active || pending->started_us == 0 ||
        (now_us - pending->started_us) < LM_CTRL_MACHINE_CLOUD_ACK_TIMEOUT_US) {
      continue;
    }

    s_link.inflight_cloud_mask &= ~pending->field_mask;
    pending->active = false;
    pending->started_us = 0;
    pending->field_mask = 0;
    pending->last_status = LM_CTRL_CLOUD_COMMAND_STATUS_TIMEOUT;
    pending->command_id[0] = '\0';
    changed = true;
    should_sync = true;
  }

  if (should_sync) {
    s_link.sync_request_flags |= LM_CTRL_MACHINE_SYNC_ALL;
    s_link.status_version++;
    should_wake = s_link.pending_mask != 0 || s_link.sync_request_flags != LM_CTRL_MACHINE_SYNC_NONE;
  }
  portEXIT_CRITICAL(&s_link_lock);

  if (changed) {
    set_statusf("Cloud dashboard confirmation timed out.");
  }
  if (should_wake) {
    wake_machine_worker();
  }
}

void lm_ctrl_machine_link_apply_cloud_dashboard_values(
  const ctrl_values_t *values,
  uint32_t loaded_mask,
  uint32_t feature_mask
) {
  if (!s_link.initialized) {
    return;
  }

  update_feature_mask(feature_mask);
  update_reported_values(values, loaded_mask);
}

void lm_ctrl_machine_link_apply_cloud_command_updates(const lm_ctrl_cloud_command_update_t *updates, size_t update_count) {
  size_t index;
  bool should_wake = false;

  if (!s_link.initialized || updates == NULL || update_count == 0) {
    return;
  }

  for (index = 0; index < update_count; ++index) {
    int slot = find_pending_cloud_command_slot(updates[index].command_id);
    bool changed = false;
    uint32_t field_mask = 0;
    ctrl_values_t sent_values = {0};

    if (slot < 0) {
      continue;
    }

    portENTER_CRITICAL(&s_link_lock);
    field_mask = s_pending_cloud_commands[slot].field_mask;
    sent_values = s_pending_cloud_commands[slot].sent_values;
    s_pending_cloud_commands[slot].last_status = updates[index].status;

    if (updates[index].status == LM_CTRL_CLOUD_COMMAND_STATUS_SUCCESS) {
      s_link.inflight_cloud_mask &= ~field_mask;
      if ((s_link.pending_mask & field_mask) == 0) {
        apply_values_to_reported_locked(&sent_values, field_mask, &changed);
      } else {
        should_wake = true;
      }
      s_pending_cloud_commands[slot].active = false;
      s_pending_cloud_commands[slot].started_us = 0;
      s_pending_cloud_commands[slot].field_mask = 0;
      s_pending_cloud_commands[slot].command_id[0] = '\0';
      changed = true;
    } else if (updates[index].status == LM_CTRL_CLOUD_COMMAND_STATUS_ERROR ||
               updates[index].status == LM_CTRL_CLOUD_COMMAND_STATUS_TIMEOUT) {
      s_link.inflight_cloud_mask &= ~field_mask;
      s_pending_cloud_commands[slot].active = false;
      s_pending_cloud_commands[slot].started_us = 0;
      s_pending_cloud_commands[slot].field_mask = 0;
      s_pending_cloud_commands[slot].command_id[0] = '\0';
      s_link.sync_request_flags |= LM_CTRL_MACHINE_SYNC_ALL;
      changed = true;
      should_wake = true;
    }

    if (changed) {
      s_link.status_version++;
    }
    portEXIT_CRITICAL(&s_link_lock);

    switch (updates[index].status) {
      case LM_CTRL_CLOUD_COMMAND_STATUS_SUCCESS:
        set_statusf("Cloud command confirmed via dashboard.");
        break;
      case LM_CTRL_CLOUD_COMMAND_STATUS_ERROR:
        if (updates[index].error_code[0] != '\0') {
          set_statusf("Cloud command failed via dashboard (%s).", updates[index].error_code);
        } else {
          set_statusf("Cloud command failed via dashboard.");
        }
        break;
      case LM_CTRL_CLOUD_COMMAND_STATUS_TIMEOUT:
        set_statusf("Cloud command timed out via dashboard.");
        break;
      default:
        break;
    }
  }

  if (should_wake) {
    wake_machine_worker();
  }
}


void lm_ctrl_machine_link_handle_cloud_websocket_disconnect(void) {
  bool changed = false;
  size_t index;

  if (!s_link.initialized) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  s_link.cloud_live_updates_active = false;
  s_link.cloud_live_updates_connected = false;
  if (s_link.inflight_cloud_mask != 0) {
    s_link.inflight_cloud_mask = 0;
    s_link.sync_request_flags |= LM_CTRL_MACHINE_SYNC_ALL;
    changed = true;
  }
  for (index = 0; index < LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS; ++index) {
    s_pending_cloud_commands[index].active = false;
    s_pending_cloud_commands[index].started_us = 0;
    s_pending_cloud_commands[index].field_mask = 0;
    s_pending_cloud_commands[index].last_status = LM_CTRL_CLOUD_COMMAND_STATUS_UNKNOWN;
    s_pending_cloud_commands[index].command_id[0] = '\0';
  }
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);

  if (changed) {
    set_statusf("Cloud websocket disconnected; falling back to sync.");
    wake_machine_worker();
  }
}
