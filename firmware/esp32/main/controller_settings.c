#include "controller_settings.h"

#include <string.h>

#include "esp_check.h"
#include "esp_lv_adapter.h"
#include "nvs.h"

#include "cloud_machine_selection.h"
#include "wifi_setup_internal.h"

static const char *TAG = "lm_ctrl_settings";

static esp_err_t erase_key_if_present(nvs_handle_t handle, const char *key) {
  esp_err_t ret = nvs_erase_key(handle, key);

  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  return ret;
}

esp_err_t lm_ctrl_settings_load(void) {
  nvs_handle_t handle = 0;
  char language_code[8] = {0};
  uint8_t logo_schema_version = 0;
  bool has_logo_version = false;
  bool has_logo_blob = false;
  size_t size = 0;
  esp_err_t ret = ESP_OK;

  lock_state();
  copy_text(s_state.hostname, sizeof(s_state.hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  s_state.language = CTRL_LANGUAGE_EN;
  s_state.sta_ssid[0] = '\0';
  s_state.sta_password[0] = '\0';
  s_state.cloud_username[0] = '\0';
  s_state.cloud_password[0] = '\0';
  clear_cached_cloud_access_token_locked();
  s_state.cloud_installation_ready = false;
  s_state.cloud_installation_registered = false;
  s_state.cloud_installation_id[0] = '\0';
  memset(s_state.cloud_secret, 0, sizeof(s_state.cloud_secret));
  memset(s_state.cloud_private_key_der, 0, sizeof(s_state.cloud_private_key_der));
  s_state.cloud_private_key_der_len = 0;
  clear_selected_machine_locked();
  clear_custom_logo_locked();
  clear_fleet_locked();
  s_state.has_credentials = false;
  s_state.has_cloud_credentials = false;
  unlock_state();

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READONLY, &handle);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  if (ret != ESP_OK) {
    return ret;
  }

  lock_state();

  size = sizeof(s_state.sta_ssid);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_SSID, s_state.sta_ssid, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  size = sizeof(s_state.sta_password);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_PASS, s_state.sta_password, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  size = sizeof(s_state.hostname);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_HOST, s_state.hostname, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  size = sizeof(language_code);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_LANG, language_code, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }
  if (ret == ESP_OK) {
    s_state.language = ctrl_language_from_code(language_code);
  }

  size = sizeof(s_state.cloud_username);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_CLOUD_USER, s_state.cloud_username, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  size = sizeof(s_state.cloud_password);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_CLOUD_PASS, s_state.cloud_password, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  size = sizeof(s_state.selected_machine.serial);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_MACHINE_SERIAL, s_state.selected_machine.serial, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  size = sizeof(s_state.selected_machine.name);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_MACHINE_NAME, s_state.selected_machine.name, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  size = sizeof(s_state.selected_machine.model);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_MACHINE_MODEL, s_state.selected_machine.model, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  size = sizeof(s_state.selected_machine.communication_key);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_MACHINE_KEY, s_state.selected_machine.communication_key, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  ret = nvs_get_u8(handle, LM_CTRL_WIFI_KEY_LOGO_VERSION, &logo_schema_version);
  if (ret == ESP_OK) {
    has_logo_version = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  size = sizeof(s_state.custom_logo_blob);
  ret = nvs_get_blob(handle, LM_CTRL_WIFI_KEY_LOGO_BLOB, s_state.custom_logo_blob, &size);
  if (ret == ESP_OK && size == sizeof(s_state.custom_logo_blob)) {
    has_logo_blob = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_ERR_NVS_INVALID_LENGTH) {
    goto exit;
  }

  if (has_logo_version && has_logo_blob && logo_schema_version == LM_CTRL_CUSTOM_LOGO_SCHEMA_VERSION) {
    s_state.has_custom_logo = true;
    s_state.custom_logo_schema_version = logo_schema_version;
  } else {
    clear_custom_logo_locked();
  }

  s_state.has_credentials = s_state.sta_ssid[0] != '\0';
  s_state.has_cloud_credentials = s_state.cloud_username[0] != '\0' && s_state.cloud_password[0] != '\0';
  s_state.has_machine_selection = s_state.selected_machine.serial[0] != '\0';
  ret = ESP_OK;

exit:
  unlock_state();
  nvs_close(handle);
  return ret;
}

esp_err_t lm_ctrl_settings_save_wifi_credentials(const char *ssid, const char *password, const char *hostname, ctrl_language_t language) {
  nvs_handle_t handle = 0;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_SSID, ssid), exit, TAG, "Failed to store SSID");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_PASS, password), exit, TAG, "Failed to store password");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_HOST, hostname), exit, TAG, "Failed to store hostname");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_LANG, ctrl_language_code(language)), exit, TAG, "Failed to store controller language");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit Wi-Fi settings");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    copy_text(s_state.sta_ssid, sizeof(s_state.sta_ssid), ssid);
    copy_text(s_state.sta_password, sizeof(s_state.sta_password), password);
    copy_text(s_state.hostname, sizeof(s_state.hostname), hostname);
    s_state.language = language;
    s_state.has_credentials = ssid[0] != '\0';
    s_state.sta_connected = false;
    s_state.sta_connecting = s_state.has_credentials;
    s_state.sta_ip[0] = '\0';
    mark_status_dirty_locked();
    unlock_state();
  }

  return ret;
}

esp_err_t lm_ctrl_settings_save_controller_preferences(const char *hostname, ctrl_language_t language) {
  nvs_handle_t handle = 0;
  char effective_hostname[33];
  esp_err_t ret;

  copy_text(effective_hostname, sizeof(effective_hostname), hostname);
  if (effective_hostname[0] == '\0') {
    copy_text(effective_hostname, sizeof(effective_hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_HOST, effective_hostname), exit, TAG, "Failed to store hostname");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_LANG, ctrl_language_code(language)), exit, TAG, "Failed to store controller language");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit controller settings");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    copy_text(s_state.hostname, sizeof(s_state.hostname), effective_hostname);
    s_state.language = language;
    mark_status_dirty_locked();
    unlock_state();
  }

  return ret;
}

esp_err_t lm_ctrl_settings_save_controller_logo(uint8_t schema_version, const uint8_t *logo_data, size_t logo_size) {
  nvs_handle_t handle = 0;
  esp_err_t ret;

  if (schema_version != LM_CTRL_CUSTOM_LOGO_SCHEMA_VERSION ||
      logo_data == NULL ||
      logo_size != LM_CTRL_CUSTOM_LOGO_BLOB_SIZE) {
    return ESP_ERR_INVALID_ARG;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_LOGO_VERSION, schema_version), exit, TAG, "Failed to store logo schema");
  ESP_GOTO_ON_ERROR(nvs_set_blob(handle, LM_CTRL_WIFI_KEY_LOGO_BLOB, logo_data, logo_size), exit, TAG, "Failed to store logo data");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit controller logo");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
      lock_state();
      memcpy(s_state.custom_logo_blob, logo_data, logo_size);
      s_state.custom_logo_schema_version = schema_version;
      s_state.has_custom_logo = true;
      mark_status_dirty_locked();
      unlock_state();
      lv_img_cache_invalidate_src(&s_custom_logo_dsc);
      esp_lv_adapter_unlock();
    } else {
      lock_state();
      memcpy(s_state.custom_logo_blob, logo_data, logo_size);
      s_state.custom_logo_schema_version = schema_version;
      s_state.has_custom_logo = true;
      mark_status_dirty_locked();
      unlock_state();
    }
  }

  return ret;
}

esp_err_t lm_ctrl_settings_clear_controller_logo(void) {
  nvs_handle_t handle = 0;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    ret = ESP_OK;
  } else if (ret == ESP_OK) {
    ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_LOGO_VERSION), exit, TAG, "Failed to erase logo schema");
    ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_LOGO_BLOB), exit, TAG, "Failed to erase logo data");
    ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit controller logo removal");
  } else {
    return ret;
  }

exit:
  if (ret != ESP_ERR_NVS_NOT_FOUND && handle != 0) {
    nvs_close(handle);
  }
  if (ret == ESP_OK) {
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
      lock_state();
      clear_custom_logo_locked();
      mark_status_dirty_locked();
      unlock_state();
      lv_img_cache_invalidate_src(&s_custom_logo_dsc);
      esp_lv_adapter_unlock();
    } else {
      lock_state();
      clear_custom_logo_locked();
      mark_status_dirty_locked();
      unlock_state();
    }
  }

  return ret;
}

esp_err_t lm_ctrl_settings_save_cloud_credentials(const char *username, const char *password, bool *credentials_changed) {
  nvs_handle_t handle = 0;
  bool changed = false;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (credentials_changed != NULL) {
    *credentials_changed = false;
  }
  if (ret != ESP_OK) {
    return ret;
  }

  lock_state();
  changed =
    strcmp(s_state.cloud_username, username) != 0 ||
    strcmp(s_state.cloud_password, password) != 0;
  unlock_state();

  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_CLOUD_USER, username), exit, TAG, "Failed to store cloud username");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_CLOUD_PASS, password), exit, TAG, "Failed to store cloud password");
  if (changed) {
    nvs_erase_key(handle, LM_CTRL_WIFI_KEY_MACHINE_SERIAL);
    nvs_erase_key(handle, LM_CTRL_WIFI_KEY_MACHINE_NAME);
    nvs_erase_key(handle, LM_CTRL_WIFI_KEY_MACHINE_MODEL);
    nvs_erase_key(handle, LM_CTRL_WIFI_KEY_MACHINE_KEY);
  }
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit cloud settings");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    copy_text(s_state.cloud_username, sizeof(s_state.cloud_username), username);
    copy_text(s_state.cloud_password, sizeof(s_state.cloud_password), password);
    s_state.has_cloud_credentials = username[0] != '\0' && password[0] != '\0';
    clear_cached_cloud_access_token_locked();
    if (changed) {
      clear_selected_machine_locked();
      clear_fleet_locked();
    }
    mark_status_dirty_locked();
    unlock_state();
    if (credentials_changed != NULL) {
      *credentials_changed = changed;
    }
  }

  return ret;
}

esp_err_t lm_ctrl_settings_save_machine_selection(const lm_ctrl_cloud_machine_t *machine) {
  nvs_handle_t handle = 0;
  esp_err_t ret;

  if (machine == NULL || machine->serial[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_SERIAL, machine->serial), exit, TAG, "Failed to store machine serial");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_NAME, machine->name), exit, TAG, "Failed to store machine name");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_MODEL, machine->model), exit, TAG, "Failed to store machine model");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_KEY, machine->communication_key), exit, TAG, "Failed to store machine key");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit machine selection");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    s_state.selected_machine = *machine;
    s_state.has_machine_selection = true;
    mark_status_dirty_locked();
    unlock_state();
  }

  return ret;
}

bool lm_ctrl_settings_get_effective_selected_machine(lm_ctrl_cloud_machine_t *machine) {
  lm_ctrl_cloud_machine_t resolved_machine = {0};
  bool auto_selected = false;
  bool has_machine = false;

  lock_state();
  has_machine = lm_ctrl_cloud_resolve_effective_machine_selection(
    &s_state.selected_machine,
    s_state.has_machine_selection,
    s_state.fleet,
    s_state.fleet_count,
    &resolved_machine,
    &auto_selected
  );
  if (has_machine && auto_selected) {
    s_state.selected_machine = resolved_machine;
    s_state.has_machine_selection = true;
    mark_status_dirty_locked();
  }
  unlock_state();

  if (!has_machine) {
    if (machine != NULL) {
      memset(machine, 0, sizeof(*machine));
    }
    return false;
  }

  if (machine != NULL) {
    *machine = resolved_machine;
  }
  return true;
}

esp_err_t lm_ctrl_settings_reset_network(void) {
  nvs_handle_t handle = 0;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_SSID), exit, TAG, "Failed to erase SSID");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_PASS), exit, TAG, "Failed to erase Wi-Fi password");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_CLOUD_USER), exit, TAG, "Failed to erase cloud username");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_CLOUD_PASS), exit, TAG, "Failed to erase cloud password");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_SERIAL), exit, TAG, "Failed to erase machine serial");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_NAME), exit, TAG, "Failed to erase machine name");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_MODEL), exit, TAG, "Failed to erase machine model");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_KEY), exit, TAG, "Failed to erase machine key");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_INSTALL_ID), exit, TAG, "Failed to erase installation id");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_INSTALL_KEY), exit, TAG, "Failed to erase installation key");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_INSTALL_REG), exit, TAG, "Failed to erase installation registration");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit network reset");

exit:
  nvs_close(handle);
  if (ret != ESP_OK) {
    return ret;
  }

  lock_state();
  s_state.sta_ssid[0] = '\0';
  s_state.sta_password[0] = '\0';
  s_state.cloud_username[0] = '\0';
  s_state.cloud_password[0] = '\0';
  s_state.has_credentials = false;
  s_state.has_cloud_credentials = false;
  s_state.sta_connected = false;
  s_state.sta_connecting = false;
  s_state.sta_ip[0] = '\0';
  s_state.cloud_installation_ready = false;
  s_state.cloud_installation_registered = false;
  s_state.cloud_installation_id[0] = '\0';
  memset(s_state.cloud_secret, 0, sizeof(s_state.cloud_secret));
  memset(s_state.cloud_private_key_der, 0, sizeof(s_state.cloud_private_key_der));
  s_state.cloud_private_key_der_len = 0;
  clear_cached_cloud_access_token_locked();
  clear_selected_machine_locked();
  clear_fleet_locked();
  mark_status_dirty_locked();
  unlock_state();

  return ESP_OK;
}

esp_err_t lm_ctrl_settings_factory_reset(void) {
  nvs_handle_t handle = 0;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret == ESP_OK) {
    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
      ret = nvs_commit(handle);
    }
    nvs_close(handle);
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    return ret;
  }

  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    ret = ESP_OK;
  }
  if (ret != ESP_OK) {
    return ret;
  }

  ret = ctrl_state_reset_persisted();
  if (ret != ESP_OK) {
    return ret;
  }

  lock_state();
  copy_text(s_state.hostname, sizeof(s_state.hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  s_state.language = CTRL_LANGUAGE_EN;
  s_state.sta_ssid[0] = '\0';
  s_state.sta_password[0] = '\0';
  s_state.cloud_username[0] = '\0';
  s_state.cloud_password[0] = '\0';
  s_state.has_credentials = false;
  s_state.has_cloud_credentials = false;
  s_state.sta_connected = false;
  s_state.sta_connecting = false;
  s_state.sta_ip[0] = '\0';
  s_state.cloud_installation_ready = false;
  s_state.cloud_installation_registered = false;
  s_state.cloud_installation_id[0] = '\0';
  memset(s_state.cloud_secret, 0, sizeof(s_state.cloud_secret));
  memset(s_state.cloud_private_key_der, 0, sizeof(s_state.cloud_private_key_der));
  s_state.cloud_private_key_der_len = 0;
  clear_cached_cloud_access_token_locked();
  clear_selected_machine_locked();
  clear_custom_logo_locked();
  clear_fleet_locked();
  mark_status_dirty_locked();
  unlock_state();

  return ESP_OK;
}

bool lm_ctrl_settings_get_machine_binding(lm_ctrl_machine_binding_t *binding) {
  lm_ctrl_cloud_machine_t machine = {0};

  if (binding == NULL) {
    return false;
  }

  memset(binding, 0, sizeof(*binding));
  if (!lm_ctrl_settings_get_effective_selected_machine(&machine)) {
    return false;
  }

  binding->configured = true;
  copy_text(binding->serial, sizeof(binding->serial), machine.serial);
  copy_text(binding->name, sizeof(binding->name), machine.name);
  copy_text(binding->model, sizeof(binding->model), machine.model);
  copy_text(binding->communication_key, sizeof(binding->communication_key), machine.communication_key);
  return binding->configured;
}
