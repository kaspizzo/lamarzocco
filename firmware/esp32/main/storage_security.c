#include "storage_security.h"

#include <stdbool.h>
#include <string.h>

#include "controller_state.h"
#include "esp_check.h"
#include "esp_hmac.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_sec_provider.h"

#include "wifi_setup_internal.h"

static const char *TAG = "lm_secure_storage";

typedef struct {
  bool has_hostname;
  bool has_language;
  bool has_ssid;
  bool has_password;
  bool has_portal_password;
  bool has_cloud_user;
  bool has_cloud_password;
  bool has_machine_serial;
  bool has_machine_name;
  bool has_machine_model;
  bool has_machine_key;
  bool has_cloud_provisioning;
  bool has_cloud_install_reg;
  bool has_web_auth_mode;
  bool has_web_auth_salt;
  bool has_web_auth_hash;
  bool has_web_auth_iterations;
  bool has_debug_screenshot_enabled;
  bool has_logo;
  char hostname[33];
  char language_code[8];
  char ssid[33];
  char password[65];
  char portal_password[65];
  char cloud_username[96];
  char cloud_password[128];
  char machine_serial[32];
  char machine_name[64];
  char machine_model[32];
  char machine_key[128];
  lm_ctrl_cloud_provisioning_blob_t cloud_provisioning;
  uint8_t cloud_install_reg;
  uint8_t web_auth_mode;
  uint8_t web_admin_salt[LM_CTRL_WEB_ADMIN_SALT_LEN];
  uint8_t web_admin_hash[LM_CTRL_WEB_ADMIN_HASH_LEN];
  uint32_t web_admin_iterations;
  bool debug_screenshot_enabled;
  uint8_t logo_schema_version;
  uint8_t logo_blob[LM_CTRL_CUSTOM_LOGO_BLOB_SIZE];
} legacy_wifi_snapshot_t;

static esp_err_t read_str_if_present(
  nvs_handle_t handle,
  const char *key,
  char *buffer,
  size_t buffer_size,
  bool *present
) {
  size_t size = buffer_size;
  esp_err_t ret;

  if (present != NULL) {
    *present = false;
  }
  if (buffer == NULL || buffer_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  buffer[0] = '\0';
  ret = nvs_get_str(handle, key, buffer, &size);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  if (ret == ESP_OK && present != NULL) {
    *present = true;
  }
  return ret;
}

static esp_err_t capture_legacy_wifi_snapshot(legacy_wifi_snapshot_t *snapshot) {
  nvs_handle_t handle = 0;
  uint8_t logo_schema_version = 0;
  uint8_t debug_screenshot_enabled = 0;
  size_t provisioning_size = sizeof(snapshot->cloud_provisioning);
  size_t web_salt_size = LM_CTRL_WEB_ADMIN_SALT_LEN;
  size_t web_hash_size = LM_CTRL_WEB_ADMIN_HASH_LEN;
  size_t logo_size = LM_CTRL_CUSTOM_LOGO_BLOB_SIZE;
  bool has_logo_version = false;
  bool has_logo_blob = false;
  esp_err_t ret;

  if (snapshot == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READONLY, &handle);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_HOST, snapshot->hostname, sizeof(snapshot->hostname), &snapshot->has_hostname), exit, TAG, "Failed to read legacy hostname");
  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_LANG, snapshot->language_code, sizeof(snapshot->language_code), &snapshot->has_language), exit, TAG, "Failed to read legacy language");
  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_SSID, snapshot->ssid, sizeof(snapshot->ssid), &snapshot->has_ssid), exit, TAG, "Failed to read legacy SSID");
  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_PASS, snapshot->password, sizeof(snapshot->password), &snapshot->has_password), exit, TAG, "Failed to read legacy Wi-Fi password");
  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_PORTAL_PASS, snapshot->portal_password, sizeof(snapshot->portal_password), &snapshot->has_portal_password), exit, TAG, "Failed to read legacy setup AP password");
  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_CLOUD_USER, snapshot->cloud_username, sizeof(snapshot->cloud_username), &snapshot->has_cloud_user), exit, TAG, "Failed to read legacy cloud user");
  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_CLOUD_PASS, snapshot->cloud_password, sizeof(snapshot->cloud_password), &snapshot->has_cloud_password), exit, TAG, "Failed to read legacy cloud password");
  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_SERIAL, snapshot->machine_serial, sizeof(snapshot->machine_serial), &snapshot->has_machine_serial), exit, TAG, "Failed to read legacy machine serial");
  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_NAME, snapshot->machine_name, sizeof(snapshot->machine_name), &snapshot->has_machine_name), exit, TAG, "Failed to read legacy machine name");
  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_MODEL, snapshot->machine_model, sizeof(snapshot->machine_model), &snapshot->has_machine_model), exit, TAG, "Failed to read legacy machine model");
  ESP_GOTO_ON_ERROR(read_str_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_KEY, snapshot->machine_key, sizeof(snapshot->machine_key), &snapshot->has_machine_key), exit, TAG, "Failed to read legacy machine key");
  ret = nvs_get_blob(handle, LM_CTRL_WIFI_KEY_INSTALL_BLOB, &snapshot->cloud_provisioning, &provisioning_size);
  if (ret == ESP_OK && provisioning_size == sizeof(snapshot->cloud_provisioning)) {
    snapshot->has_cloud_provisioning = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_ERR_NVS_INVALID_LENGTH) {
    goto exit;
  }
  ret = nvs_get_u8(handle, LM_CTRL_WIFI_KEY_INSTALL_REG, &snapshot->cloud_install_reg);
  if (ret == ESP_OK) {
    snapshot->has_cloud_install_reg = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }
  ret = nvs_get_u8(handle, LM_CTRL_WIFI_KEY_WEB_MODE, &snapshot->web_auth_mode);
  if (ret == ESP_OK) {
    snapshot->has_web_auth_mode = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }
  ret = nvs_get_blob(handle, LM_CTRL_WIFI_KEY_WEB_SALT, snapshot->web_admin_salt, &web_salt_size);
  if (ret == ESP_OK && web_salt_size == LM_CTRL_WEB_ADMIN_SALT_LEN) {
    snapshot->has_web_auth_salt = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_ERR_NVS_INVALID_LENGTH) {
    goto exit;
  }
  ret = nvs_get_blob(handle, LM_CTRL_WIFI_KEY_WEB_HASH, snapshot->web_admin_hash, &web_hash_size);
  if (ret == ESP_OK && web_hash_size == LM_CTRL_WEB_ADMIN_HASH_LEN) {
    snapshot->has_web_auth_hash = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_ERR_NVS_INVALID_LENGTH) {
    goto exit;
  }
  ret = nvs_get_u32(handle, LM_CTRL_WIFI_KEY_WEB_ITER, &snapshot->web_admin_iterations);
  if (ret == ESP_OK) {
    snapshot->has_web_auth_iterations = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }
  ret = nvs_get_u8(handle, LM_CTRL_WIFI_KEY_DEBUG_SHOT, &debug_screenshot_enabled);
  if (ret == ESP_OK) {
    snapshot->has_debug_screenshot_enabled = true;
    snapshot->debug_screenshot_enabled = debug_screenshot_enabled != 0;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  ret = nvs_get_u8(handle, LM_CTRL_WIFI_KEY_LOGO_VERSION, &logo_schema_version);
  if (ret == ESP_OK) {
    has_logo_version = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    goto exit;
  }

  ret = nvs_get_blob(handle, LM_CTRL_WIFI_KEY_LOGO_BLOB, snapshot->logo_blob, &logo_size);
  if (ret == ESP_OK && logo_size == LM_CTRL_CUSTOM_LOGO_BLOB_SIZE) {
    has_logo_blob = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_ERR_NVS_INVALID_LENGTH) {
    goto exit;
  }

  if (has_logo_version && has_logo_blob && logo_schema_version == LM_CTRL_CUSTOM_LOGO_SCHEMA_VERSION) {
    snapshot->has_logo = true;
    snapshot->logo_schema_version = logo_schema_version;
  }

  ret = ESP_OK;

exit:
  nvs_close(handle);
  return ret;
}

static esp_err_t restore_legacy_wifi_snapshot(const legacy_wifi_snapshot_t *snapshot) {
  nvs_handle_t handle = 0;
  esp_err_t ret;

  if (snapshot == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  if (snapshot->has_hostname) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_HOST, snapshot->hostname), exit, TAG, "Failed to restore hostname");
  }
  if (snapshot->has_language) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_LANG, snapshot->language_code), exit, TAG, "Failed to restore language");
  }
  if (snapshot->has_ssid) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_SSID, snapshot->ssid), exit, TAG, "Failed to restore SSID");
  }
  if (snapshot->has_password) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_PASS, snapshot->password), exit, TAG, "Failed to restore Wi-Fi password");
  }
  if (snapshot->has_portal_password) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_PORTAL_PASS, snapshot->portal_password), exit, TAG, "Failed to restore setup AP password");
  }
  if (snapshot->has_cloud_user) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_CLOUD_USER, snapshot->cloud_username), exit, TAG, "Failed to restore cloud username");
  }
  if (snapshot->has_cloud_password) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_CLOUD_PASS, snapshot->cloud_password), exit, TAG, "Failed to restore cloud password");
  }
  if (snapshot->has_machine_serial) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_SERIAL, snapshot->machine_serial), exit, TAG, "Failed to restore machine serial");
  }
  if (snapshot->has_machine_name) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_NAME, snapshot->machine_name), exit, TAG, "Failed to restore machine name");
  }
  if (snapshot->has_machine_model) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_MODEL, snapshot->machine_model), exit, TAG, "Failed to restore machine model");
  }
  if (snapshot->has_machine_key) {
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_KEY, snapshot->machine_key), exit, TAG, "Failed to restore machine key");
  }
  if (snapshot->has_cloud_provisioning) {
    ESP_GOTO_ON_ERROR(nvs_set_blob(handle, LM_CTRL_WIFI_KEY_INSTALL_BLOB, &snapshot->cloud_provisioning, sizeof(snapshot->cloud_provisioning)), exit, TAG, "Failed to restore cloud provisioning");
  }
  if (snapshot->has_cloud_install_reg) {
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_INSTALL_REG, snapshot->cloud_install_reg), exit, TAG, "Failed to restore cloud installation registration");
  }
  if (snapshot->has_web_auth_mode) {
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_WEB_MODE, snapshot->web_auth_mode), exit, TAG, "Failed to restore web auth mode");
  }
  if (snapshot->has_web_auth_salt) {
    ESP_GOTO_ON_ERROR(nvs_set_blob(handle, LM_CTRL_WIFI_KEY_WEB_SALT, snapshot->web_admin_salt, sizeof(snapshot->web_admin_salt)), exit, TAG, "Failed to restore web auth salt");
  }
  if (snapshot->has_web_auth_hash) {
    ESP_GOTO_ON_ERROR(nvs_set_blob(handle, LM_CTRL_WIFI_KEY_WEB_HASH, snapshot->web_admin_hash, sizeof(snapshot->web_admin_hash)), exit, TAG, "Failed to restore web auth hash");
  }
  if (snapshot->has_web_auth_iterations) {
    ESP_GOTO_ON_ERROR(nvs_set_u32(handle, LM_CTRL_WIFI_KEY_WEB_ITER, snapshot->web_admin_iterations), exit, TAG, "Failed to restore web auth iterations");
  }
  if (snapshot->has_debug_screenshot_enabled) {
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_DEBUG_SHOT, snapshot->debug_screenshot_enabled ? 1U : 0U), exit, TAG, "Failed to restore debug screenshot toggle");
  }
  if (snapshot->has_logo) {
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_LOGO_VERSION, snapshot->logo_schema_version), exit, TAG, "Failed to restore logo schema");
    ESP_GOTO_ON_ERROR(nvs_set_blob(handle, LM_CTRL_WIFI_KEY_LOGO_BLOB, snapshot->logo_blob, sizeof(snapshot->logo_blob)), exit, TAG, "Failed to restore logo blob");
  }

  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit migrated Wi-Fi settings");
  ret = ESP_OK;

exit:
  nvs_close(handle);
  return ret;
}

static esp_err_t init_plaintext_nvs_for_capture(void) {
  esp_err_t ret = nvs_flash_init();

  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "Legacy NVS needed erase before migration");
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase legacy plaintext NVS");
    ret = nvs_flash_init();
  }
  return ret;
}

static esp_err_t secure_init_with_cfg(nvs_sec_cfg_t *cfg) {
  esp_err_t ret = nvs_flash_secure_init(cfg);

  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "Encrypted NVS needed erase before initialization");
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase encrypted NVS");
    ret = nvs_flash_secure_init(cfg);
  }
  return ret;
}

static esp_err_t migrate_legacy_plaintext_nvs(nvs_sec_scheme_t *sec_scheme_handle, nvs_sec_cfg_t *cfg) {
  legacy_wifi_snapshot_t *wifi_snapshot = NULL;
  ctrl_state_t *controller_state = NULL;
  bool has_controller_state = false;
  esp_err_t ret = ESP_OK;

  wifi_snapshot = calloc(1, sizeof(*wifi_snapshot));
  controller_state = calloc(1, sizeof(*controller_state));
  if (wifi_snapshot == NULL || controller_state == NULL) {
    free(wifi_snapshot);
    free(controller_state);
    return ESP_ERR_NO_MEM;
  }

  ctrl_state_init(controller_state);
  ESP_RETURN_ON_ERROR(init_plaintext_nvs_for_capture(), TAG, "Failed to initialize legacy plaintext NVS");
  ESP_GOTO_ON_ERROR(capture_legacy_wifi_snapshot(wifi_snapshot), exit_plaintext, TAG, "Failed to capture legacy Wi-Fi settings");
  if (ctrl_state_load(controller_state) == ESP_OK) {
    has_controller_state = true;
  } else {
    ctrl_state_init(controller_state);
  }

exit_plaintext:
  nvs_flash_deinit();
  if (ret != ESP_OK) {
    goto exit;
  }

  ESP_RETURN_ON_ERROR(nvs_flash_generate_keys_v2(sec_scheme_handle, cfg), TAG, "Failed to provision HMAC-backed NVS keys");
  ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase legacy plaintext NVS");
  ESP_RETURN_ON_ERROR(secure_init_with_cfg(cfg), TAG, "Failed to initialize encrypted NVS after migration");
  ret = restore_legacy_wifi_snapshot(wifi_snapshot);
  if (ret == ESP_OK && has_controller_state) {
    ret = ctrl_state_persist(controller_state);
  }
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Legacy migration restore failed, clearing encrypted NVS for safe re-onboarding");
    nvs_flash_deinit();
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to clear partially restored encrypted NVS");
    ESP_RETURN_ON_ERROR(secure_init_with_cfg(cfg), TAG, "Failed to reinitialize encrypted NVS after migration fallback");
  }

exit:
  if (wifi_snapshot != NULL) {
    secure_zero(wifi_snapshot, sizeof(*wifi_snapshot));
    free(wifi_snapshot);
  }
  if (controller_state != NULL) {
    secure_zero(controller_state, sizeof(*controller_state));
    free(controller_state);
  }
  return ret;
}

esp_err_t lm_ctrl_secure_storage_init(void) {
  nvs_sec_cfg_t cfg = {0};
  nvs_sec_config_hmac_t sec_scheme_cfg = {
    .hmac_key_id = HMAC_KEY0,
  };
  nvs_sec_scheme_t *sec_scheme_handle = NULL;
  esp_err_t ret;

  ret = nvs_sec_provider_register_hmac(&sec_scheme_cfg, &sec_scheme_handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = nvs_flash_read_security_cfg_v2(sec_scheme_handle, &cfg);
  if (ret == ESP_ERR_NVS_SEC_HMAC_KEY_NOT_FOUND) {
    ESP_LOGI(TAG, "No HMAC-backed NVS key found, migrating legacy plaintext settings");
    ret = migrate_legacy_plaintext_nvs(sec_scheme_handle, &cfg);
  } else if (ret == ESP_OK) {
    ret = secure_init_with_cfg(&cfg);
  } else {
    ESP_LOGE(TAG, "Could not read NVS security configuration: %s", esp_err_to_name(ret));
  }

  if (sec_scheme_handle != NULL) {
    nvs_sec_provider_deregister(sec_scheme_handle);
  }
  secure_zero(&cfg, sizeof(cfg));
  return ret;
}
