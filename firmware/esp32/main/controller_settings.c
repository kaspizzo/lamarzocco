#include "controller_settings.h"

#include <stdlib.h>
#include <string.h>

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "mbedtls/md.h"
#include "nvs.h"

#include "cloud_machine_selection.h"
#include "settings_storage_model.h"
#include "wifi_setup_internal.h"

static const char *TAG = "lm_ctrl_settings";

static void clear_web_session_locked(void) {
  s_state.web_session_token[0] = '\0';
  s_state.web_csrf_token[0] = '\0';
  s_state.web_session_valid_until_us = 0;
}

static void clear_web_admin_material_locked(void) {
  secure_zero(s_state.web_admin_salt, sizeof(s_state.web_admin_salt));
  secure_zero(s_state.web_admin_hash, sizeof(s_state.web_admin_hash));
  s_state.web_admin_iterations = 0;
}

static lm_ctrl_settings_snapshot_t *alloc_settings_snapshot(void) {
  return calloc(1, sizeof(lm_ctrl_settings_snapshot_t));
}

static void free_settings_snapshot(lm_ctrl_settings_snapshot_t *snapshot) {
  if (snapshot == NULL) {
    return;
  }

  secure_zero(snapshot, sizeof(*snapshot));
  free(snapshot);
}

static esp_err_t write_settings_snapshot_fields(
  nvs_handle_t handle,
  const lm_ctrl_settings_snapshot_t *snapshot,
  uint64_t field_mask,
  const char *context
) {
  esp_err_t ret = lm_ctrl_settings_snapshot_write_fields(handle, snapshot, field_mask);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to store %s", context);
  }
  return ret;
}

static esp_err_t erase_settings_fields(
  nvs_handle_t handle,
  uint64_t field_mask,
  const char *context
) {
  esp_err_t ret = lm_ctrl_settings_erase_fields(handle, field_mask);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to erase %s", context);
  }
  return ret;
}

static esp_err_t derive_web_admin_hash(
  const char *password,
  const uint8_t salt[LM_CTRL_WEB_ADMIN_SALT_LEN],
  uint32_t iterations,
  uint8_t out_hash[LM_CTRL_WEB_ADMIN_HASH_LEN]
) {
  const mbedtls_md_info_t *md_info = NULL;
  uint8_t u[LM_CTRL_WEB_ADMIN_HASH_LEN];
  uint8_t t[LM_CTRL_WEB_ADMIN_HASH_LEN];
  uint8_t salt_block[LM_CTRL_WEB_ADMIN_SALT_LEN + 4];
  size_t password_len = 0;
  int mbedtls_ret = 0;

  if (password == NULL || salt == NULL || out_hash == NULL || iterations == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  password_len = strlen(password);
  if (password_len == 0 || password_len > 63) {
    return ESP_ERR_INVALID_ARG;
  }

  md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md_info == NULL) {
    return ESP_FAIL;
  }

  memcpy(salt_block, salt, LM_CTRL_WEB_ADMIN_SALT_LEN);
  salt_block[LM_CTRL_WEB_ADMIN_SALT_LEN + 0] = 0;
  salt_block[LM_CTRL_WEB_ADMIN_SALT_LEN + 1] = 0;
  salt_block[LM_CTRL_WEB_ADMIN_SALT_LEN + 2] = 0;
  salt_block[LM_CTRL_WEB_ADMIN_SALT_LEN + 3] = 1;

  mbedtls_ret = mbedtls_md_hmac(
    md_info,
    (const unsigned char *)password,
    password_len,
    salt_block,
    sizeof(salt_block),
    u
  );
  if (mbedtls_ret != 0) {
    secure_zero(salt_block, sizeof(salt_block));
    secure_zero(u, sizeof(u));
    secure_zero(t, sizeof(t));
    return ESP_FAIL;
  }

  memcpy(t, u, sizeof(t));
  for (uint32_t iteration = 1; iteration < iterations; ++iteration) {
    mbedtls_ret = mbedtls_md_hmac(
      md_info,
      (const unsigned char *)password,
      password_len,
      u,
      sizeof(u),
      u
    );
    if (mbedtls_ret != 0) {
      secure_zero(salt_block, sizeof(salt_block));
      secure_zero(u, sizeof(u));
      secure_zero(t, sizeof(t));
      return ESP_FAIL;
    }
    for (size_t index = 0; index < sizeof(t); ++index) {
      t[index] ^= u[index];
    }
  }

  memcpy(out_hash, t, LM_CTRL_WEB_ADMIN_HASH_LEN);
  secure_zero(salt_block, sizeof(salt_block));
  secure_zero(u, sizeof(u));
  secure_zero(t, sizeof(t));
  return ESP_OK;
}

static esp_err_t persist_web_admin_hash_material(
  const uint8_t salt[LM_CTRL_WEB_ADMIN_SALT_LEN],
  const uint8_t hash[LM_CTRL_WEB_ADMIN_HASH_LEN],
  uint32_t iterations
) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = NULL;
  esp_err_t ret;

  if (salt == NULL || hash == NULL || iterations == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  snapshot = alloc_settings_snapshot();
  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  snapshot->present_mask = LM_CTRL_SETTINGS_WEB_AUTH_FIELDS;
  snapshot->values.web_auth_mode = (uint8_t)LM_CTRL_WEB_AUTH_ENABLED;
  memcpy(snapshot->values.web_auth_salt, salt, sizeof(snapshot->values.web_auth_salt));
  memcpy(snapshot->values.web_auth_hash, hash, sizeof(snapshot->values.web_auth_hash));
  snapshot->values.web_auth_iterations = iterations;
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "web auth settings"),
    exit,
    TAG,
    "Failed to store web auth settings"
  );
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit web auth settings");

exit:
  nvs_close(handle);
  free_settings_snapshot(snapshot);
  return ret;
}

esp_err_t lm_ctrl_settings_load(void) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = NULL;
  bool has_logo_version = false;
  bool has_logo_blob = false;
  bool has_web_salt = false;
  bool has_web_hash = false;
  esp_err_t ret = ESP_OK;

  lock_state();
  copy_text(s_state.hostname, sizeof(s_state.hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  s_state.language = CTRL_LANGUAGE_EN;
  s_state.sta_ssid[0] = '\0';
  s_state.sta_password[0] = '\0';
  s_state.portal_password[0] = '\0';
  s_state.cloud_username[0] = '\0';
  s_state.cloud_password[0] = '\0';
  clear_cached_cloud_access_token_locked();
  clear_web_session_locked();
  clear_web_admin_material_locked();
  s_state.web_auth_mode = LM_CTRL_WEB_AUTH_UNSET;
  s_state.heat_display_enabled = true;
  s_state.debug_screenshot_enabled = false;
  s_state.has_cloud_provisioning = false;
  s_state.cloud_installation_ready = false;
  s_state.cloud_installation_registered = false;
  s_state.cloud_installation_id[0] = '\0';
  secure_zero(s_state.cloud_secret, sizeof(s_state.cloud_secret));
  secure_zero(s_state.cloud_private_key_der, sizeof(s_state.cloud_private_key_der));
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

  snapshot = alloc_settings_snapshot();
  if (snapshot == NULL) {
    nvs_close(handle);
    return ESP_ERR_NO_MEM;
  }

  lock_state();
  ret = lm_ctrl_settings_snapshot_load_from_nvs(handle, snapshot);
  if (ret != ESP_OK) {
    goto exit;
  }

  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_SSID)) {
    copy_text(s_state.sta_ssid, sizeof(s_state.sta_ssid), snapshot->values.ssid);
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_PASSWORD)) {
    copy_text(s_state.sta_password, sizeof(s_state.sta_password), snapshot->values.password);
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_PORTAL_PASSWORD)) {
    copy_text(s_state.portal_password, sizeof(s_state.portal_password), snapshot->values.portal_password);
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_HOSTNAME)) {
    copy_text(s_state.hostname, sizeof(s_state.hostname), snapshot->values.hostname);
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_LANGUAGE_CODE)) {
    s_state.language = ctrl_language_from_code(snapshot->values.language_code);
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_CLOUD_USERNAME)) {
    copy_text(s_state.cloud_username, sizeof(s_state.cloud_username), snapshot->values.cloud_username);
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_CLOUD_PASSWORD)) {
    copy_text(s_state.cloud_password, sizeof(s_state.cloud_password), snapshot->values.cloud_password);
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_MACHINE_SERIAL)) {
    copy_text(s_state.selected_machine.serial, sizeof(s_state.selected_machine.serial), snapshot->values.machine_serial);
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_MACHINE_NAME)) {
    copy_text(s_state.selected_machine.name, sizeof(s_state.selected_machine.name), snapshot->values.machine_name);
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_MACHINE_MODEL)) {
    copy_text(s_state.selected_machine.model, sizeof(s_state.selected_machine.model), snapshot->values.machine_model);
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_MACHINE_KEY)) {
    copy_text(
      s_state.selected_machine.communication_key,
      sizeof(s_state.selected_machine.communication_key),
      snapshot->values.machine_key
    );
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_WEB_AUTH_MODE) &&
      snapshot->values.web_auth_mode <= (uint8_t)LM_CTRL_WEB_AUTH_ENABLED) {
    s_state.web_auth_mode = (lm_ctrl_web_auth_mode_t)snapshot->values.web_auth_mode;
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_DEBUG_SCREENSHOT_ENABLED)) {
    s_state.debug_screenshot_enabled = snapshot->values.debug_screenshot_enabled != 0;
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_HEAT_DISPLAY_ENABLED)) {
    s_state.heat_display_enabled = snapshot->values.heat_display_enabled != 0;
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_WEB_AUTH_SALT)) {
    memcpy(s_state.web_admin_salt, snapshot->values.web_auth_salt, sizeof(s_state.web_admin_salt));
    has_web_salt = true;
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_WEB_AUTH_HASH)) {
    memcpy(s_state.web_admin_hash, snapshot->values.web_auth_hash, sizeof(s_state.web_admin_hash));
    has_web_hash = true;
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_WEB_AUTH_ITERATIONS) &&
      snapshot->values.web_auth_iterations > 0) {
    s_state.web_admin_iterations = snapshot->values.web_auth_iterations;
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_CLOUD_PROVISIONING) &&
      snapshot->values.cloud_provisioning.schema_version == LM_CTRL_CLOUD_PROVISIONING_SCHEMA_VERSION &&
      snapshot->values.cloud_provisioning.private_key_der_len > 0 &&
      snapshot->values.cloud_provisioning.private_key_der_len <= LM_CTRL_PRIVATE_KEY_DER_MAX &&
      snapshot->values.cloud_provisioning.installation_id[0] != '\0') {
    s_state.has_cloud_provisioning = true;
    s_state.cloud_installation_ready = true;
    copy_text(
      s_state.cloud_installation_id,
      sizeof(s_state.cloud_installation_id),
      snapshot->values.cloud_provisioning.installation_id
    );
    memcpy(s_state.cloud_secret, snapshot->values.cloud_provisioning.secret, sizeof(s_state.cloud_secret));
    memcpy(
      s_state.cloud_private_key_der,
      snapshot->values.cloud_provisioning.private_key_der,
      snapshot->values.cloud_provisioning.private_key_der_len
    );
    s_state.cloud_private_key_der_len = snapshot->values.cloud_provisioning.private_key_der_len;
  }
  if (lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_CLOUD_INSTALL_REG)) {
    s_state.cloud_installation_registered = snapshot->values.cloud_install_reg != 0;
  }

  has_logo_version = lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_LOGO_SCHEMA_VERSION);
  has_logo_blob = lm_ctrl_settings_snapshot_has(snapshot, LM_CTRL_SETTINGS_FIELD_LOGO_BLOB);
  if (has_logo_version &&
      has_logo_blob &&
      snapshot->values.logo_schema_version == LM_CTRL_CUSTOM_LOGO_SCHEMA_VERSION) {
    s_state.has_custom_logo = true;
    s_state.custom_logo_schema_version = snapshot->values.logo_schema_version;
    memcpy(s_state.custom_logo_blob, snapshot->values.logo_blob, sizeof(s_state.custom_logo_blob));
  } else {
    clear_custom_logo_locked();
  }

  if (s_state.web_auth_mode == LM_CTRL_WEB_AUTH_ENABLED && (!has_web_salt || !has_web_hash)) {
    s_state.web_auth_mode = LM_CTRL_WEB_AUTH_UNSET;
    clear_web_admin_material_locked();
  } else if (s_state.web_auth_mode == LM_CTRL_WEB_AUTH_ENABLED &&
             s_state.web_admin_iterations != 0 &&
             s_state.web_admin_iterations != LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS &&
             s_state.web_admin_iterations != LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS_LEGACY) {
    s_state.web_auth_mode = LM_CTRL_WEB_AUTH_UNSET;
    clear_web_admin_material_locked();
  }

  s_state.has_credentials = s_state.sta_ssid[0] != '\0';
  s_state.has_cloud_credentials = s_state.cloud_username[0] != '\0' && s_state.cloud_password[0] != '\0';
  s_state.has_machine_selection = s_state.selected_machine.serial[0] != '\0';
  ret = ESP_OK;

exit:
  unlock_state();
  nvs_close(handle);
  free_settings_snapshot(snapshot);
  return ret;
}

esp_err_t lm_ctrl_settings_save_wifi_credentials(const char *ssid, const char *password, const char *hostname, ctrl_language_t language) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = alloc_settings_snapshot();
  esp_err_t ret;

  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  snapshot->present_mask = LM_CTRL_SETTINGS_WIFI_CREDENTIAL_FIELDS;
  copy_text(snapshot->values.ssid, sizeof(snapshot->values.ssid), ssid);
  copy_text(snapshot->values.password, sizeof(snapshot->values.password), password);
  copy_text(snapshot->values.hostname, sizeof(snapshot->values.hostname), hostname);
  copy_text(snapshot->values.language_code, sizeof(snapshot->values.language_code), ctrl_language_code(language));
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "Wi-Fi settings"),
    exit,
    TAG,
    "Failed to store Wi-Fi settings"
  );
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

  free_settings_snapshot(snapshot);
  return ret;
}

esp_err_t lm_ctrl_settings_save_controller_preferences(const char *hostname, ctrl_language_t language) {
  nvs_handle_t handle = 0;
  char effective_hostname[33];
  lm_ctrl_settings_snapshot_t *snapshot = NULL;
  esp_err_t ret;

  copy_text(effective_hostname, sizeof(effective_hostname), hostname);
  if (effective_hostname[0] == '\0') {
    copy_text(effective_hostname, sizeof(effective_hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  }

  snapshot = alloc_settings_snapshot();
  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  snapshot->present_mask = LM_CTRL_SETTINGS_CONTROLLER_PREFERENCE_FIELDS;
  copy_text(snapshot->values.hostname, sizeof(snapshot->values.hostname), effective_hostname);
  copy_text(snapshot->values.language_code, sizeof(snapshot->values.language_code), ctrl_language_code(language));
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "controller settings"),
    exit,
    TAG,
    "Failed to store controller settings"
  );
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

  free_settings_snapshot(snapshot);
  return ret;
}

esp_err_t lm_ctrl_settings_save_controller_logo(uint8_t schema_version, const uint8_t *logo_data, size_t logo_size) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = NULL;
  esp_err_t ret;

  if (schema_version != LM_CTRL_CUSTOM_LOGO_SCHEMA_VERSION ||
      logo_data == NULL ||
      logo_size != LM_CTRL_CUSTOM_LOGO_BLOB_SIZE) {
    return ESP_ERR_INVALID_ARG;
  }

  snapshot = alloc_settings_snapshot();
  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  snapshot->present_mask = LM_CTRL_SETTINGS_LOGO_FIELDS;
  snapshot->values.logo_schema_version = schema_version;
  memcpy(snapshot->values.logo_blob, logo_data, logo_size);
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "controller logo"),
    exit,
    TAG,
    "Failed to store controller logo"
  );
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

  free_settings_snapshot(snapshot);
  return ret;
}

esp_err_t lm_ctrl_settings_clear_controller_logo(void) {
  nvs_handle_t handle = 0;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    ret = ESP_OK;
  } else if (ret == ESP_OK) {
    ESP_GOTO_ON_ERROR(
      erase_settings_fields(handle, LM_CTRL_SETTINGS_LOGO_FIELDS, "controller logo"),
      exit,
      TAG,
      "Failed to erase controller logo"
    );
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
  lm_ctrl_settings_snapshot_t *snapshot = alloc_settings_snapshot();
  bool changed = false;
  esp_err_t ret;

  if (credentials_changed != NULL) {
    *credentials_changed = false;
  }
  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  lock_state();
  changed =
    strcmp(s_state.cloud_username, username) != 0 ||
    strcmp(s_state.cloud_password, password) != 0;
  unlock_state();

  snapshot->present_mask = LM_CTRL_SETTINGS_CLOUD_CREDENTIAL_FIELDS;
  copy_text(snapshot->values.cloud_username, sizeof(snapshot->values.cloud_username), username);
  copy_text(snapshot->values.cloud_password, sizeof(snapshot->values.cloud_password), password);
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "cloud settings"),
    exit,
    TAG,
    "Failed to store cloud settings"
  );
  if (changed) {
    ESP_GOTO_ON_ERROR(
      erase_settings_fields(handle, LM_CTRL_SETTINGS_MACHINE_SELECTION_FIELDS, "machine selection"),
      exit,
      TAG,
      "Failed to erase machine selection"
    );
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

  free_settings_snapshot(snapshot);
  return ret;
}

esp_err_t lm_ctrl_settings_save_machine_selection(const lm_ctrl_cloud_machine_t *machine) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = NULL;
  esp_err_t ret;

  if (machine == NULL || machine->serial[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }

  snapshot = alloc_settings_snapshot();
  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  snapshot->present_mask = LM_CTRL_SETTINGS_MACHINE_SELECTION_FIELDS;
  copy_text(snapshot->values.machine_serial, sizeof(snapshot->values.machine_serial), machine->serial);
  copy_text(snapshot->values.machine_name, sizeof(snapshot->values.machine_name), machine->name);
  copy_text(snapshot->values.machine_model, sizeof(snapshot->values.machine_model), machine->model);
  copy_text(snapshot->values.machine_key, sizeof(snapshot->values.machine_key), machine->communication_key);
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "machine selection"),
    exit,
    TAG,
    "Failed to store machine selection"
  );
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

  free_settings_snapshot(snapshot);
  return ret;
}

esp_err_t lm_ctrl_settings_save_cloud_provisioning(
  const char *installation_id,
  const uint8_t *secret,
  const uint8_t *private_key_der,
  size_t private_key_der_len
) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = NULL;
  lm_ctrl_cloud_provisioning_blob_t provisioning = {0};
  esp_err_t ret;

  if (installation_id == NULL ||
      installation_id[0] == '\0' ||
      secret == NULL ||
      private_key_der == NULL ||
      private_key_der_len == 0 ||
      private_key_der_len > LM_CTRL_PRIVATE_KEY_DER_MAX) {
    return ESP_ERR_INVALID_ARG;
  }

  provisioning.schema_version = LM_CTRL_CLOUD_PROVISIONING_SCHEMA_VERSION;
  provisioning.private_key_der_len = (uint16_t)private_key_der_len;
  copy_text(provisioning.installation_id, sizeof(provisioning.installation_id), installation_id);
  memcpy(provisioning.secret, secret, sizeof(provisioning.secret));
  memcpy(provisioning.private_key_der, private_key_der, private_key_der_len);

  snapshot = alloc_settings_snapshot();
  if (snapshot == NULL) {
    secure_zero(&provisioning, sizeof(provisioning));
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    secure_zero(&provisioning, sizeof(provisioning));
    return ret;
  }

  snapshot->present_mask =
    LM_CTRL_SETTINGS_FIELD_MASK(CLOUD_PROVISIONING) |
    LM_CTRL_SETTINGS_FIELD_MASK(CLOUD_INSTALL_REG);
  snapshot->values.cloud_provisioning = provisioning;
  snapshot->values.cloud_install_reg = 0;
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "cloud provisioning"),
    exit,
    TAG,
    "Failed to store cloud provisioning"
  );
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit cloud provisioning");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    s_state.has_cloud_provisioning = true;
    s_state.cloud_installation_ready = true;
    s_state.cloud_installation_registered = false;
    copy_text(s_state.cloud_installation_id, sizeof(s_state.cloud_installation_id), provisioning.installation_id);
    memcpy(s_state.cloud_secret, provisioning.secret, sizeof(s_state.cloud_secret));
    memcpy(s_state.cloud_private_key_der, provisioning.private_key_der, provisioning.private_key_der_len);
    s_state.cloud_private_key_der_len = provisioning.private_key_der_len;
    clear_cached_cloud_access_token_locked();
    mark_status_dirty_locked();
    unlock_state();
  }
  free_settings_snapshot(snapshot);
  secure_zero(&provisioning, sizeof(provisioning));
  return ret;
}

esp_err_t lm_ctrl_settings_ensure_cloud_provisioning(void) {
  lm_ctrl_cloud_installation_t installation = {0};
  bool already_ready = false;
  esp_err_t ret;

  lock_state();
  already_ready =
    s_state.has_cloud_provisioning &&
    s_state.cloud_installation_ready &&
    s_state.cloud_installation_id[0] != '\0' &&
    s_state.cloud_private_key_der_len > 0 &&
    s_state.cloud_private_key_der_len <= LM_CTRL_PRIVATE_KEY_DER_MAX;
  unlock_state();
  if (already_ready) {
    return ESP_OK;
  }

  ret = lm_ctrl_cloud_generate_installation(&installation);
  if (ret == ESP_OK) {
    ret = lm_ctrl_settings_save_cloud_provisioning(
      installation.installation_id,
      installation.secret,
      installation.private_key_der,
      installation.private_key_der_len
    );
  }

  secure_zero(&installation, sizeof(installation));
  return ret;
}

esp_err_t lm_ctrl_settings_set_cloud_installation_registered(bool registered) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = alloc_settings_snapshot();
  esp_err_t ret;

  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  snapshot->present_mask = LM_CTRL_SETTINGS_FIELD_MASK(CLOUD_INSTALL_REG);
  snapshot->values.cloud_install_reg = registered ? 1U : 0U;
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "cloud installation registration"),
    exit,
    TAG,
    "Failed to store cloud installation registration"
  );
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit cloud installation registration");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    s_state.cloud_installation_registered = registered && s_state.has_cloud_provisioning;
    unlock_state();
  }
  free_settings_snapshot(snapshot);
  return ret;
}

esp_err_t lm_ctrl_settings_save_portal_password(const char *password) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = NULL;
  esp_err_t ret;

  if (password == NULL || password[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }

  snapshot = alloc_settings_snapshot();
  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  snapshot->present_mask = LM_CTRL_SETTINGS_FIELD_MASK(PORTAL_PASSWORD);
  copy_text(snapshot->values.portal_password, sizeof(snapshot->values.portal_password), password);
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "setup AP password"),
    exit,
    TAG,
    "Failed to store setup AP password"
  );
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit setup AP password");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    copy_text(s_state.portal_password, sizeof(s_state.portal_password), password);
    mark_status_dirty_locked();
    unlock_state();
  }
  free_settings_snapshot(snapshot);
  return ret;
}

esp_err_t lm_ctrl_settings_save_web_admin_password(const char *password) {
  uint8_t salt[LM_CTRL_WEB_ADMIN_SALT_LEN] = {0};
  uint8_t hash[LM_CTRL_WEB_ADMIN_HASH_LEN] = {0};
  esp_err_t ret;

  if (password == NULL || strlen(password) < 8 || strlen(password) > 63) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_fill_random(salt, sizeof(salt));
  ESP_RETURN_ON_ERROR(
    derive_web_admin_hash(password, salt, LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS, hash),
    TAG,
    "Failed to derive web-admin password hash"
  );

  ret = persist_web_admin_hash_material(salt, hash, LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS);
  if (ret == ESP_OK) {
    lock_state();
    s_state.web_auth_mode = LM_CTRL_WEB_AUTH_ENABLED;
    memcpy(s_state.web_admin_salt, salt, sizeof(salt));
    memcpy(s_state.web_admin_hash, hash, sizeof(hash));
    s_state.web_admin_iterations = LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS;
    clear_web_session_locked();
    mark_status_dirty_locked();
    unlock_state();
  }
  secure_zero(salt, sizeof(salt));
  secure_zero(hash, sizeof(hash));
  return ret;
}

esp_err_t lm_ctrl_settings_clear_web_admin_password(void) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = alloc_settings_snapshot();
  esp_err_t ret;

  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  snapshot->present_mask = LM_CTRL_SETTINGS_FIELD_MASK(WEB_AUTH_MODE);
  snapshot->values.web_auth_mode = (uint8_t)LM_CTRL_WEB_AUTH_DISABLED;
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "disabled web auth mode"),
    exit,
    TAG,
    "Failed to store disabled web auth mode"
  );
  ESP_GOTO_ON_ERROR(
    erase_settings_fields(handle, LM_CTRL_SETTINGS_WEB_AUTH_FIELDS & ~LM_CTRL_SETTINGS_FIELD_MASK(WEB_AUTH_MODE), "web auth material"),
    exit,
    TAG,
    "Failed to erase web auth material"
  );
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit web auth removal");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    s_state.web_auth_mode = LM_CTRL_WEB_AUTH_DISABLED;
    clear_web_admin_material_locked();
    clear_web_session_locked();
    mark_status_dirty_locked();
    unlock_state();
  }
  free_settings_snapshot(snapshot);
  return ret;
}

bool lm_ctrl_settings_verify_web_admin_password(const char *password) {
  uint8_t expected_hash[LM_CTRL_WEB_ADMIN_HASH_LEN] = {0};
  uint8_t salt[LM_CTRL_WEB_ADMIN_SALT_LEN] = {0};
  uint8_t stored_hash[LM_CTRL_WEB_ADMIN_HASH_LEN] = {0};
  uint8_t upgraded_hash[LM_CTRL_WEB_ADMIN_HASH_LEN] = {0};
  uint32_t iterations = 0;
  uint32_t matched_iterations = 0;
  bool valid = false;

  if (password == NULL) {
    return false;
  }

  lock_state();
  if (s_state.web_auth_mode == LM_CTRL_WEB_AUTH_ENABLED) {
    memcpy(salt, s_state.web_admin_salt, sizeof(salt));
    memcpy(stored_hash, s_state.web_admin_hash, sizeof(stored_hash));
    iterations = s_state.web_admin_iterations;
    valid = true;
  }
  unlock_state();

  if (!valid) {
    secure_zero(salt, sizeof(salt));
    secure_zero(stored_hash, sizeof(stored_hash));
    secure_zero(expected_hash, sizeof(expected_hash));
    return false;
  }

  if (iterations == 0) {
    if (derive_web_admin_hash(password, salt, LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS, expected_hash) == ESP_OK &&
        secure_equals(expected_hash, stored_hash, sizeof(expected_hash))) {
      matched_iterations = LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS;
    } else if (derive_web_admin_hash(password, salt, LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS_LEGACY, expected_hash) == ESP_OK &&
               secure_equals(expected_hash, stored_hash, sizeof(expected_hash))) {
      matched_iterations = LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS_LEGACY;
    }
  } else if (derive_web_admin_hash(password, salt, iterations, expected_hash) == ESP_OK &&
             secure_equals(expected_hash, stored_hash, sizeof(expected_hash))) {
    matched_iterations = iterations;
  }

  valid = matched_iterations != 0;
  if (valid &&
      matched_iterations != LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS &&
      derive_web_admin_hash(password, salt, LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS, upgraded_hash) == ESP_OK &&
      persist_web_admin_hash_material(salt, upgraded_hash, LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS) == ESP_OK) {
    lock_state();
    if (s_state.web_auth_mode == LM_CTRL_WEB_AUTH_ENABLED) {
      memcpy(s_state.web_admin_hash, upgraded_hash, sizeof(upgraded_hash));
      s_state.web_admin_iterations = LM_CTRL_WEB_ADMIN_PBKDF2_ITERATIONS;
    }
    unlock_state();
  }

  secure_zero(salt, sizeof(salt));
  secure_zero(stored_hash, sizeof(stored_hash));
  secure_zero(expected_hash, sizeof(expected_hash));
  secure_zero(upgraded_hash, sizeof(upgraded_hash));
  return valid;
}

esp_err_t lm_ctrl_settings_set_heat_display_enabled(bool enabled) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = alloc_settings_snapshot();
  esp_err_t ret;

  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  snapshot->present_mask = LM_CTRL_SETTINGS_FIELD_MASK(HEAT_DISPLAY_ENABLED);
  snapshot->values.heat_display_enabled = enabled ? 1U : 0U;
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "heat display toggle"),
    exit,
    TAG,
    "Failed to store heat display toggle"
  );
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit heat display toggle");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    s_state.heat_display_enabled = enabled;
    mark_status_dirty_locked();
    unlock_state();
  }
  free_settings_snapshot(snapshot);
  return ret;
}

esp_err_t lm_ctrl_settings_set_debug_screenshot_enabled(bool enabled) {
  nvs_handle_t handle = 0;
  lm_ctrl_settings_snapshot_t *snapshot = alloc_settings_snapshot();
  esp_err_t ret;

  if (snapshot == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    free_settings_snapshot(snapshot);
    return ret;
  }

  snapshot->present_mask = LM_CTRL_SETTINGS_FIELD_MASK(DEBUG_SCREENSHOT_ENABLED);
  snapshot->values.debug_screenshot_enabled = enabled ? 1U : 0U;
  ESP_GOTO_ON_ERROR(
    write_settings_snapshot_fields(handle, snapshot, snapshot->present_mask, "debug screenshot toggle"),
    exit,
    TAG,
    "Failed to store debug screenshot toggle"
  );
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit debug screenshot toggle");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    s_state.debug_screenshot_enabled = enabled;
    mark_status_dirty_locked();
    unlock_state();
  }
  free_settings_snapshot(snapshot);
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

  ESP_GOTO_ON_ERROR(
    erase_settings_fields(handle, LM_CTRL_SETTINGS_NETWORK_RESET_FIELDS, "network settings"),
    exit,
    TAG,
    "Failed to erase network settings"
  );
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit network reset");

exit:
  nvs_close(handle);
  if (ret != ESP_OK) {
    return ret;
  }

  lock_state();
  s_state.sta_ssid[0] = '\0';
  s_state.sta_password[0] = '\0';
  s_state.portal_password[0] = '\0';
  s_state.cloud_username[0] = '\0';
  s_state.cloud_password[0] = '\0';
  s_state.has_credentials = false;
  s_state.has_cloud_credentials = false;
  s_state.sta_connected = false;
  s_state.sta_connecting = false;
  s_state.sta_ip[0] = '\0';
  s_state.web_auth_mode = LM_CTRL_WEB_AUTH_UNSET;
  s_state.debug_screenshot_enabled = false;
  clear_web_admin_material_locked();
  clear_web_session_locked();
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
  s_state.portal_password[0] = '\0';
  s_state.cloud_username[0] = '\0';
  s_state.cloud_password[0] = '\0';
  s_state.has_credentials = false;
  s_state.has_cloud_credentials = false;
  s_state.sta_connected = false;
  s_state.sta_connecting = false;
  s_state.sta_ip[0] = '\0';
  s_state.web_auth_mode = LM_CTRL_WEB_AUTH_UNSET;
  s_state.heat_display_enabled = true;
  s_state.debug_screenshot_enabled = false;
  clear_web_admin_material_locked();
  clear_web_session_locked();
  s_state.has_cloud_provisioning = false;
  s_state.cloud_installation_ready = false;
  s_state.cloud_installation_registered = false;
  s_state.cloud_installation_id[0] = '\0';
  secure_zero(s_state.cloud_secret, sizeof(s_state.cloud_secret));
  secure_zero(s_state.cloud_private_key_der, sizeof(s_state.cloud_private_key_der));
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
