#include "controller_connectivity.h"

static const uint32_t BLE_FIELD_MASK =
  LM_CTRL_MACHINE_FIELD_TEMPERATURE |
  LM_CTRL_MACHINE_FIELD_STEAM |
  LM_CTRL_MACHINE_FIELD_STANDBY;

static bool cloud_machine_online(const lm_ctrl_wifi_info_t *wifi_info) {
  return wifi_info != NULL &&
         wifi_info->has_cloud_credentials &&
         wifi_info->sta_connected &&
         wifi_info->cloud_connected &&
         wifi_info->has_machine_selection &&
         wifi_info->machine_cloud_online;
}

static bool ble_machine_online(const lm_ctrl_machine_link_info_t *machine_info) {
  return machine_info != NULL && machine_info->authenticated;
}

uint32_t lm_ctrl_machine_field_for_focus(ctrl_focus_t focus) {
  switch (focus) {
    case CTRL_FOCUS_TEMPERATURE:
      return LM_CTRL_MACHINE_FIELD_TEMPERATURE;
    case CTRL_FOCUS_INFUSE:
      return LM_CTRL_MACHINE_FIELD_INFUSE;
    case CTRL_FOCUS_PAUSE:
      return LM_CTRL_MACHINE_FIELD_PAUSE;
    case CTRL_FOCUS_STEAM:
      return LM_CTRL_MACHINE_FIELD_STEAM;
    case CTRL_FOCUS_STANDBY:
      return LM_CTRL_MACHINE_FIELD_STANDBY;
    case CTRL_FOCUS_BBW_MODE:
      return LM_CTRL_MACHINE_FIELD_BBW_MODE;
    case CTRL_FOCUS_BBW_DOSE_1:
      return LM_CTRL_MACHINE_FIELD_BBW_DOSE_1;
    case CTRL_FOCUS_BBW_DOSE_2:
      return LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
    default:
      return LM_CTRL_MACHINE_FIELD_NONE;
  }
}

uint32_t lm_ctrl_machine_preset_field_mask(uint32_t feature_mask) {
  uint32_t field_mask = LM_CTRL_MACHINE_FIELD_TEMPERATURE | LM_CTRL_MACHINE_FIELD_PREBREWING;

  if ((feature_mask & CTRL_FEATURE_BBW) != 0) {
    field_mask |= LM_CTRL_MACHINE_FIELD_BBW;
  }

  return field_mask;
}

lm_ctrl_field_presentation_t lm_ctrl_controller_field_presentation(
  uint32_t readable_mask,
  uint32_t loaded_mask,
  ctrl_focus_t focus
) {
  const uint32_t field_mask = lm_ctrl_machine_field_for_focus(focus);

  if (field_mask == LM_CTRL_MACHINE_FIELD_NONE || (readable_mask & field_mask) != field_mask) {
    return LM_CTRL_FIELD_PRESENTATION_UNAVAILABLE;
  }
  if ((loaded_mask & field_mask) != field_mask) {
    return LM_CTRL_FIELD_PRESENTATION_LOADING;
  }

  return LM_CTRL_FIELD_PRESENTATION_READY;
}

bool lm_ctrl_controller_field_is_editable(uint32_t editable_mask, ctrl_focus_t focus) {
  const uint32_t field_mask = lm_ctrl_machine_field_for_focus(focus);

  return field_mask != LM_CTRL_MACHINE_FIELD_NONE && (editable_mask & field_mask) == field_mask;
}

lm_ctrl_indicator_state_t lm_ctrl_remote_path_indicator_state(lm_ctrl_remote_path_state_t state) {
  switch (state) {
    case LM_CTRL_REMOTE_PATH_CONNECTING:
    case LM_CTRL_REMOTE_PATH_NETWORK:
    case LM_CTRL_REMOTE_PATH_CLOUD:
      return LM_CTRL_INDICATOR_CROSSED;
    case LM_CTRL_REMOTE_PATH_MACHINE_ONLINE:
      return LM_CTRL_INDICATOR_ACTIVE;
    case LM_CTRL_REMOTE_PATH_HIDDEN:
    default:
      return LM_CTRL_INDICATOR_HIDDEN;
  }
}

void lm_ctrl_controller_compute_access(
  const lm_ctrl_wifi_info_t *wifi_info,
  const lm_ctrl_machine_link_info_t *machine_info,
  uint32_t feature_mask,
  lm_ctrl_controller_access_t *access
) {
  const bool remote_online = cloud_machine_online(wifi_info);
  const bool ble_online = ble_machine_online(machine_info);
  const bool remote_icon_visible =
    wifi_info != NULL &&
    wifi_info->has_cloud_credentials &&
    wifi_info->has_machine_selection;
  const bool wifi_connecting = wifi_info != NULL && wifi_info->sta_connecting;
  const bool wifi_connected = wifi_info != NULL && wifi_info->sta_connected;
  const bool cloud_ready =
    wifi_info != NULL &&
    wifi_info->sta_connected &&
    wifi_info->cloud_connected;

  if (access == NULL) {
    return;
  }

  access->remote_path_state = LM_CTRL_REMOTE_PATH_HIDDEN;
  access->readable_mask = 0;
  access->editable_mask = 0;
  access->preset_load_enabled = false;

  if (!remote_icon_visible) {
    access->remote_path_state = LM_CTRL_REMOTE_PATH_HIDDEN;
  } else if (remote_online) {
    access->remote_path_state = LM_CTRL_REMOTE_PATH_MACHINE_ONLINE;
  } else if (cloud_ready) {
    access->remote_path_state = LM_CTRL_REMOTE_PATH_CLOUD;
  } else if (wifi_connected) {
    access->remote_path_state = LM_CTRL_REMOTE_PATH_NETWORK;
  } else if (wifi_connecting) {
    access->remote_path_state = LM_CTRL_REMOTE_PATH_CONNECTING;
  }

  if (ble_online || remote_online) {
    access->readable_mask |= BLE_FIELD_MASK;
    access->editable_mask |= BLE_FIELD_MASK;
  }
  if (remote_online) {
    access->readable_mask |= LM_CTRL_MACHINE_FIELD_PREBREWING;
    access->editable_mask |= LM_CTRL_MACHINE_FIELD_PREBREWING;
    if ((feature_mask & CTRL_FEATURE_BBW) != 0) {
      access->readable_mask |= LM_CTRL_MACHINE_FIELD_BBW;
      access->editable_mask |= LM_CTRL_MACHINE_FIELD_BBW;
    }
  }

  access->preset_load_enabled =
    (access->editable_mask & lm_ctrl_machine_preset_field_mask(feature_mask)) ==
    lm_ctrl_machine_preset_field_mask(feature_mask);
}
