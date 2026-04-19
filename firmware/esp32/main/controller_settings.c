#include "controller_settings.h"

#include <string.h>

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "esp_check.h"
#include "esp_lv_adapter.h"
#include "mbedtls/md.h"
#include "nvs.h"

#include "cloud_machine_selection.h"
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
  esp_err_t ret;

  if (salt == NULL || hash == NULL || iterations == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_WEB_MODE, (uint8_t)LM_CTRL_WEB_AUTH_ENABLED), exit, TAG, "Failed to store web auth mode");
  ESP_GOTO_ON_ERROR(nvs_set_blob(handle, LM_CTRL_WIFI_KEY_WEB_SALT, salt, LM_CTRL_WEB_ADMIN_SALT_LEN), exit, TAG, "Failed to store web auth salt");
  ESP_GOTO_ON_ERROR(nvs_set_blob(handle, LM_CTRL_WIFI_KEY_WEB_HASH, hash, LM_CTRL_WEB_ADMIN_HASH_LEN), exit, TAG, "Failed to store web auth hash");
  ESP_GOTO_ON_ERROR(nvs_set_u32(handle, LM_CTRL_WIFI_KEY_WEB_ITER, iterations), exit, TAG, "Failed to store web auth iterations");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit web auth settings");

exit:
  nvs_close(handle);
  return ret;
}

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
  bool has_web_salt = false;
  bool has_web_hash = false;
  uint8_t web_auth_mode = (uint8_t)LM_CTRL_WEB_AUTH_UNSET;
  uint8_t heat_display_enabled = 1;
  uint8_t debug_screenshot_enabled = 0;
  uint8_t install_reg = 0;
  uint32_t web_admin_iterations = 0;
  lm_ctrl_cloud_provisioning_blob_t provisioning = {0};
  size_t size = 0;
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

  size = sizeof(s_state.portal_password);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_PORTAL_PASS, s_state.portal_password, &size);
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

  ret = nvs_get_u8(handle, LM_CTRL_WIFI_KEY_WEB_MODE, &web_auth_mode);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }
  if (ret == ESP_OK && web_auth_mode <= (uint8_t)LM_CTRL_WEB_AUTH_ENABLED) {
    s_state.web_auth_mode = (lm_ctrl_web_auth_mode_t)web_auth_mode;
  }

  ret = nvs_get_u8(handle, LM_CTRL_WIFI_KEY_DEBUG_SHOT, &debug_screenshot_enabled);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }
  if (ret == ESP_OK) {
    s_state.debug_screenshot_enabled = debug_screenshot_enabled != 0;
  }

  ret = nvs_get_u8(handle, LM_CTRL_WIFI_KEY_HEAT_DISPLAY, &heat_display_enabled);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }
  if (ret == ESP_OK) {
    s_state.heat_display_enabled = heat_display_enabled != 0;
  }

  size = sizeof(s_state.web_admin_salt);
  ret = nvs_get_blob(handle, LM_CTRL_WIFI_KEY_WEB_SALT, s_state.web_admin_salt, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_ERR_NVS_INVALID_LENGTH) {
    goto exit;
  }
  if (ret == ESP_OK && size == sizeof(s_state.web_admin_salt)) {
    has_web_salt = true;
  } else if (ret == ESP_OK) {
    clear_web_admin_material_locked();
    s_state.web_auth_mode = LM_CTRL_WEB_AUTH_UNSET;
  }

  size = sizeof(s_state.web_admin_hash);
  ret = nvs_get_blob(handle, LM_CTRL_WIFI_KEY_WEB_HASH, s_state.web_admin_hash, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_ERR_NVS_INVALID_LENGTH) {
    goto exit;
  }
  if (ret == ESP_OK && size == sizeof(s_state.web_admin_hash)) {
    has_web_hash = true;
  } else if (ret == ESP_OK) {
    clear_web_admin_material_locked();
    s_state.web_auth_mode = LM_CTRL_WEB_AUTH_UNSET;
  }

  ret = nvs_get_u32(handle, LM_CTRL_WIFI_KEY_WEB_ITER, &web_admin_iterations);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }
  if (ret == ESP_OK && web_admin_iterations > 0) {
    s_state.web_admin_iterations = web_admin_iterations;
  }

  size = sizeof(provisioning);
  ret = nvs_get_blob(handle, LM_CTRL_WIFI_KEY_INSTALL_BLOB, &provisioning, &size);
  if (ret == ESP_OK &&
      size == sizeof(provisioning) &&
      provisioning.schema_version == LM_CTRL_CLOUD_PROVISIONING_SCHEMA_VERSION &&
      provisioning.private_key_der_len > 0 &&
      provisioning.private_key_der_len <= LM_CTRL_PRIVATE_KEY_DER_MAX &&
      provisioning.installation_id[0] != '\0') {
    s_state.has_cloud_provisioning = true;
    s_state.cloud_installation_ready = true;
    copy_text(s_state.cloud_installation_id, sizeof(s_state.cloud_installation_id), provisioning.installation_id);
    memcpy(s_state.cloud_secret, provisioning.secret, sizeof(s_state.cloud_secret));
    memcpy(s_state.cloud_private_key_der, provisioning.private_key_der, provisioning.private_key_der_len);
    s_state.cloud_private_key_der_len = provisioning.private_key_der_len;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_ERR_NVS_INVALID_LENGTH) {
    goto exit;
  }

  ret = nvs_get_u8(handle, LM_CTRL_WIFI_KEY_INSTALL_REG, &install_reg);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }
  if (ret == ESP_OK) {
    s_state.cloud_installation_registered = install_reg != 0;
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
  secure_zero(&provisioning, sizeof(provisioning));
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

esp_err_t lm_ctrl_settings_save_cloud_provisioning(
  const char *installation_id,
  const uint8_t *secret,
  const uint8_t *private_key_der,
  size_t private_key_der_len
) {
  nvs_handle_t handle = 0;
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

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    secure_zero(&provisioning, sizeof(provisioning));
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_blob(handle, LM_CTRL_WIFI_KEY_INSTALL_BLOB, &provisioning, sizeof(provisioning)), exit, TAG, "Failed to store cloud provisioning");
  ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_INSTALL_REG, 0), exit, TAG, "Failed to clear cloud installation registration");
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
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_INSTALL_REG, registered ? 1 : 0), exit, TAG, "Failed to store cloud installation registration");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit cloud installation registration");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    s_state.cloud_installation_registered = registered && s_state.has_cloud_provisioning;
    unlock_state();
  }
  return ret;
}

esp_err_t lm_ctrl_settings_save_portal_password(const char *password) {
  nvs_handle_t handle = 0;
  esp_err_t ret;

  if (password == NULL || password[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_PORTAL_PASS, password), exit, TAG, "Failed to store setup AP password");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit setup AP password");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    copy_text(s_state.portal_password, sizeof(s_state.portal_password), password);
    mark_status_dirty_locked();
    unlock_state();
  }
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
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_WEB_MODE, (uint8_t)LM_CTRL_WEB_AUTH_DISABLED), exit, TAG, "Failed to store disabled web auth mode");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_WEB_SALT), exit, TAG, "Failed to erase web auth salt");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_WEB_HASH), exit, TAG, "Failed to erase web auth hash");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_WEB_ITER), exit, TAG, "Failed to erase web auth iterations");
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
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_HEAT_DISPLAY, enabled ? 1U : 0U), exit, TAG, "Failed to store heat display toggle");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit heat display toggle");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    s_state.heat_display_enabled = enabled;
    mark_status_dirty_locked();
    unlock_state();
  }
  return ret;
}

esp_err_t lm_ctrl_settings_set_debug_screenshot_enabled(bool enabled) {
  nvs_handle_t handle = 0;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_DEBUG_SHOT, enabled ? 1U : 0U), exit, TAG, "Failed to store debug screenshot toggle");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit debug screenshot toggle");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    s_state.debug_screenshot_enabled = enabled;
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
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_PORTAL_PASS), exit, TAG, "Failed to erase setup AP password");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_WEB_MODE), exit, TAG, "Failed to erase web auth mode");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_WEB_SALT), exit, TAG, "Failed to erase web auth salt");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_WEB_HASH), exit, TAG, "Failed to erase web auth hash");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_WEB_ITER), exit, TAG, "Failed to erase web auth iterations");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_DEBUG_SHOT), exit, TAG, "Failed to erase debug screenshot toggle");
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
