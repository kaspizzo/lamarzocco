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

#include "lm_ctrl_nvs_keys.h"
#include "lm_ctrl_secure_utils.h"
#include "settings_storage_model.h"

static const char *TAG = "lm_secure_storage";

typedef lm_ctrl_settings_snapshot_t legacy_wifi_snapshot_t;

static esp_err_t capture_legacy_wifi_snapshot(legacy_wifi_snapshot_t *snapshot) {
  nvs_handle_t handle = 0;
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

  ret = lm_ctrl_settings_snapshot_load_from_nvs(handle, snapshot);
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

  ESP_GOTO_ON_ERROR(
    lm_ctrl_settings_snapshot_write_fields(handle, snapshot, snapshot->present_mask),
    exit,
    TAG,
    "Failed to restore migrated Wi-Fi settings"
  );

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
