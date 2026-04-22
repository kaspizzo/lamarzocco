#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "nvs.h"
#include "lm_ctrl_cloud_endpoints.h"
#include "lm_ctrl_nvs_keys.h"
#include "lm_ctrl_secure_utils.h"
#include "wifi_setup_types.h"

typedef enum {
  LM_CTRL_SETTINGS_FIELD_HOSTNAME = 0,
  LM_CTRL_SETTINGS_FIELD_LANGUAGE_CODE,
  LM_CTRL_SETTINGS_FIELD_SSID,
  LM_CTRL_SETTINGS_FIELD_PASSWORD,
  LM_CTRL_SETTINGS_FIELD_PORTAL_PASSWORD,
  LM_CTRL_SETTINGS_FIELD_CLOUD_USERNAME,
  LM_CTRL_SETTINGS_FIELD_CLOUD_PASSWORD,
  LM_CTRL_SETTINGS_FIELD_MACHINE_SERIAL,
  LM_CTRL_SETTINGS_FIELD_MACHINE_NAME,
  LM_CTRL_SETTINGS_FIELD_MACHINE_MODEL,
  LM_CTRL_SETTINGS_FIELD_MACHINE_KEY,
  LM_CTRL_SETTINGS_FIELD_CLOUD_PROVISIONING,
  LM_CTRL_SETTINGS_FIELD_CLOUD_INSTALL_REG,
  LM_CTRL_SETTINGS_FIELD_WEB_AUTH_MODE,
  LM_CTRL_SETTINGS_FIELD_WEB_AUTH_SALT,
  LM_CTRL_SETTINGS_FIELD_WEB_AUTH_HASH,
  LM_CTRL_SETTINGS_FIELD_WEB_AUTH_ITERATIONS,
  LM_CTRL_SETTINGS_FIELD_HEAT_DISPLAY_ENABLED,
  LM_CTRL_SETTINGS_FIELD_DEBUG_SCREENSHOT_ENABLED,
  LM_CTRL_SETTINGS_FIELD_LOGO_SCHEMA_VERSION,
  LM_CTRL_SETTINGS_FIELD_LOGO_BLOB,
  LM_CTRL_SETTINGS_FIELD_COUNT,
} lm_ctrl_settings_field_id_t;

typedef enum {
  LM_CTRL_SETTINGS_VALUE_STR = 0,
  LM_CTRL_SETTINGS_VALUE_U8,
  LM_CTRL_SETTINGS_VALUE_U32,
  LM_CTRL_SETTINGS_VALUE_BLOB,
} lm_ctrl_settings_value_kind_t;

typedef struct {
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
  uint8_t web_auth_salt[LM_CTRL_WEB_ADMIN_SALT_LEN];
  uint8_t web_auth_hash[LM_CTRL_WEB_ADMIN_HASH_LEN];
  uint32_t web_auth_iterations;
  uint8_t heat_display_enabled;
  uint8_t debug_screenshot_enabled;
  uint8_t logo_schema_version;
  uint8_t logo_blob[LM_CTRL_CUSTOM_LOGO_BLOB_SIZE];
} lm_ctrl_settings_storage_values_t;

typedef struct {
  uint64_t present_mask;
  lm_ctrl_settings_storage_values_t values;
} lm_ctrl_settings_snapshot_t;

typedef struct {
  lm_ctrl_settings_field_id_t field_id;
  const char *key;
  lm_ctrl_settings_value_kind_t kind;
  size_t value_offset;
  size_t value_size;
  uint32_t flags;
} lm_ctrl_settings_field_desc_t;

enum {
  LM_CTRL_SETTINGS_FIELD_FLAG_IGNORE_INVALID_LENGTH = 1U << 0,
};

#define LM_CTRL_SETTINGS_FIELD_MASK_VALUE(field_id_) (UINT64_C(1) << (field_id_))
#define LM_CTRL_SETTINGS_FIELD_MASK(field_name_) \
  LM_CTRL_SETTINGS_FIELD_MASK_VALUE(LM_CTRL_SETTINGS_FIELD_##field_name_)

#define LM_CTRL_SETTINGS_WIFI_CREDENTIAL_FIELDS \
  (LM_CTRL_SETTINGS_FIELD_MASK(SSID) | \
   LM_CTRL_SETTINGS_FIELD_MASK(PASSWORD) | \
   LM_CTRL_SETTINGS_FIELD_MASK(HOSTNAME) | \
   LM_CTRL_SETTINGS_FIELD_MASK(LANGUAGE_CODE))

#define LM_CTRL_SETTINGS_CONTROLLER_PREFERENCE_FIELDS \
  (LM_CTRL_SETTINGS_FIELD_MASK(HOSTNAME) | \
   LM_CTRL_SETTINGS_FIELD_MASK(LANGUAGE_CODE))

#define LM_CTRL_SETTINGS_CLOUD_CREDENTIAL_FIELDS \
  (LM_CTRL_SETTINGS_FIELD_MASK(CLOUD_USERNAME) | \
   LM_CTRL_SETTINGS_FIELD_MASK(CLOUD_PASSWORD))

#define LM_CTRL_SETTINGS_MACHINE_SELECTION_FIELDS \
  (LM_CTRL_SETTINGS_FIELD_MASK(MACHINE_SERIAL) | \
   LM_CTRL_SETTINGS_FIELD_MASK(MACHINE_NAME) | \
   LM_CTRL_SETTINGS_FIELD_MASK(MACHINE_MODEL) | \
   LM_CTRL_SETTINGS_FIELD_MASK(MACHINE_KEY))

#define LM_CTRL_SETTINGS_WEB_AUTH_FIELDS \
  (LM_CTRL_SETTINGS_FIELD_MASK(WEB_AUTH_MODE) | \
   LM_CTRL_SETTINGS_FIELD_MASK(WEB_AUTH_SALT) | \
   LM_CTRL_SETTINGS_FIELD_MASK(WEB_AUTH_HASH) | \
   LM_CTRL_SETTINGS_FIELD_MASK(WEB_AUTH_ITERATIONS))

#define LM_CTRL_SETTINGS_LOGO_FIELDS \
  (LM_CTRL_SETTINGS_FIELD_MASK(LOGO_SCHEMA_VERSION) | \
   LM_CTRL_SETTINGS_FIELD_MASK(LOGO_BLOB))

#define LM_CTRL_SETTINGS_NETWORK_RESET_FIELDS \
  (LM_CTRL_SETTINGS_FIELD_MASK(SSID) | \
   LM_CTRL_SETTINGS_FIELD_MASK(PASSWORD) | \
   LM_CTRL_SETTINGS_CLOUD_CREDENTIAL_FIELDS | \
   LM_CTRL_SETTINGS_MACHINE_SELECTION_FIELDS | \
   LM_CTRL_SETTINGS_FIELD_MASK(PORTAL_PASSWORD) | \
   LM_CTRL_SETTINGS_WEB_AUTH_FIELDS | \
   LM_CTRL_SETTINGS_FIELD_MASK(DEBUG_SCREENSHOT_ENABLED))

#define LM_CTRL_SETTINGS_FIELD_DESC_STR(field_id_, key_, member_) \
  { \
    (field_id_), \
    (key_), \
    LM_CTRL_SETTINGS_VALUE_STR, \
    offsetof(lm_ctrl_settings_storage_values_t, member_), \
    sizeof(((lm_ctrl_settings_storage_values_t *)0)->member_), \
    0, \
  }

#define LM_CTRL_SETTINGS_FIELD_DESC_U8(field_id_, key_, member_) \
  { \
    (field_id_), \
    (key_), \
    LM_CTRL_SETTINGS_VALUE_U8, \
    offsetof(lm_ctrl_settings_storage_values_t, member_), \
    sizeof(((lm_ctrl_settings_storage_values_t *)0)->member_), \
    0, \
  }

#define LM_CTRL_SETTINGS_FIELD_DESC_U32(field_id_, key_, member_) \
  { \
    (field_id_), \
    (key_), \
    LM_CTRL_SETTINGS_VALUE_U32, \
    offsetof(lm_ctrl_settings_storage_values_t, member_), \
    sizeof(((lm_ctrl_settings_storage_values_t *)0)->member_), \
    0, \
  }

#define LM_CTRL_SETTINGS_FIELD_DESC_BLOB(field_id_, key_, member_, flags_) \
  { \
    (field_id_), \
    (key_), \
    LM_CTRL_SETTINGS_VALUE_BLOB, \
    offsetof(lm_ctrl_settings_storage_values_t, member_), \
    sizeof(((lm_ctrl_settings_storage_values_t *)0)->member_), \
    (flags_), \
  }

static const lm_ctrl_settings_field_desc_t s_lm_ctrl_settings_fields[] = {
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_HOSTNAME, LM_CTRL_WIFI_KEY_HOST, hostname),
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_LANGUAGE_CODE, LM_CTRL_WIFI_KEY_LANG, language_code),
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_SSID, LM_CTRL_WIFI_KEY_SSID, ssid),
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_PASSWORD, LM_CTRL_WIFI_KEY_PASS, password),
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_PORTAL_PASSWORD, LM_CTRL_WIFI_KEY_PORTAL_PASS, portal_password),
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_CLOUD_USERNAME, LM_CTRL_WIFI_KEY_CLOUD_USER, cloud_username),
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_CLOUD_PASSWORD, LM_CTRL_WIFI_KEY_CLOUD_PASS, cloud_password),
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_MACHINE_SERIAL, LM_CTRL_WIFI_KEY_MACHINE_SERIAL, machine_serial),
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_MACHINE_NAME, LM_CTRL_WIFI_KEY_MACHINE_NAME, machine_name),
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_MACHINE_MODEL, LM_CTRL_WIFI_KEY_MACHINE_MODEL, machine_model),
  LM_CTRL_SETTINGS_FIELD_DESC_STR(LM_CTRL_SETTINGS_FIELD_MACHINE_KEY, LM_CTRL_WIFI_KEY_MACHINE_KEY, machine_key),
  LM_CTRL_SETTINGS_FIELD_DESC_BLOB(
    LM_CTRL_SETTINGS_FIELD_CLOUD_PROVISIONING,
    LM_CTRL_WIFI_KEY_INSTALL_BLOB,
    cloud_provisioning,
    LM_CTRL_SETTINGS_FIELD_FLAG_IGNORE_INVALID_LENGTH
  ),
  LM_CTRL_SETTINGS_FIELD_DESC_U8(LM_CTRL_SETTINGS_FIELD_CLOUD_INSTALL_REG, LM_CTRL_WIFI_KEY_INSTALL_REG, cloud_install_reg),
  LM_CTRL_SETTINGS_FIELD_DESC_U8(LM_CTRL_SETTINGS_FIELD_WEB_AUTH_MODE, LM_CTRL_WIFI_KEY_WEB_MODE, web_auth_mode),
  LM_CTRL_SETTINGS_FIELD_DESC_BLOB(
    LM_CTRL_SETTINGS_FIELD_WEB_AUTH_SALT,
    LM_CTRL_WIFI_KEY_WEB_SALT,
    web_auth_salt,
    LM_CTRL_SETTINGS_FIELD_FLAG_IGNORE_INVALID_LENGTH
  ),
  LM_CTRL_SETTINGS_FIELD_DESC_BLOB(
    LM_CTRL_SETTINGS_FIELD_WEB_AUTH_HASH,
    LM_CTRL_WIFI_KEY_WEB_HASH,
    web_auth_hash,
    LM_CTRL_SETTINGS_FIELD_FLAG_IGNORE_INVALID_LENGTH
  ),
  LM_CTRL_SETTINGS_FIELD_DESC_U32(LM_CTRL_SETTINGS_FIELD_WEB_AUTH_ITERATIONS, LM_CTRL_WIFI_KEY_WEB_ITER, web_auth_iterations),
  LM_CTRL_SETTINGS_FIELD_DESC_U8(LM_CTRL_SETTINGS_FIELD_HEAT_DISPLAY_ENABLED, LM_CTRL_WIFI_KEY_HEAT_DISPLAY, heat_display_enabled),
  LM_CTRL_SETTINGS_FIELD_DESC_U8(LM_CTRL_SETTINGS_FIELD_DEBUG_SCREENSHOT_ENABLED, LM_CTRL_WIFI_KEY_DEBUG_SHOT, debug_screenshot_enabled),
  LM_CTRL_SETTINGS_FIELD_DESC_U8(LM_CTRL_SETTINGS_FIELD_LOGO_SCHEMA_VERSION, LM_CTRL_WIFI_KEY_LOGO_VERSION, logo_schema_version),
  LM_CTRL_SETTINGS_FIELD_DESC_BLOB(
    LM_CTRL_SETTINGS_FIELD_LOGO_BLOB,
    LM_CTRL_WIFI_KEY_LOGO_BLOB,
    logo_blob,
    LM_CTRL_SETTINGS_FIELD_FLAG_IGNORE_INVALID_LENGTH
  ),
};

static inline size_t lm_ctrl_settings_field_count(void) {
  return sizeof(s_lm_ctrl_settings_fields) / sizeof(s_lm_ctrl_settings_fields[0]);
}

static inline const lm_ctrl_settings_field_desc_t *lm_ctrl_settings_find_field_desc(
  lm_ctrl_settings_field_id_t field_id
) {
  for (size_t i = 0; i < lm_ctrl_settings_field_count(); ++i) {
    if (s_lm_ctrl_settings_fields[i].field_id == field_id) {
      return &s_lm_ctrl_settings_fields[i];
    }
  }
  return NULL;
}

static inline void *lm_ctrl_settings_snapshot_value_ptr(
  lm_ctrl_settings_snapshot_t *snapshot,
  const lm_ctrl_settings_field_desc_t *field
) {
  return (uint8_t *)&snapshot->values + field->value_offset;
}

static inline const void *lm_ctrl_settings_snapshot_value_cptr(
  const lm_ctrl_settings_snapshot_t *snapshot,
  const lm_ctrl_settings_field_desc_t *field
) {
  return (const uint8_t *)&snapshot->values + field->value_offset;
}

static inline bool lm_ctrl_settings_snapshot_has(
  const lm_ctrl_settings_snapshot_t *snapshot,
  lm_ctrl_settings_field_id_t field_id
) {
  if (snapshot == NULL) {
    return false;
  }
  return (snapshot->present_mask & LM_CTRL_SETTINGS_FIELD_MASK_VALUE(field_id)) != 0;
}

static inline void lm_ctrl_settings_snapshot_mark_present(
  lm_ctrl_settings_snapshot_t *snapshot,
  lm_ctrl_settings_field_id_t field_id
) {
  if (snapshot == NULL) {
    return;
  }
  snapshot->present_mask |= LM_CTRL_SETTINGS_FIELD_MASK_VALUE(field_id);
}

static inline esp_err_t lm_ctrl_settings_snapshot_load_from_nvs(
  nvs_handle_t handle,
  lm_ctrl_settings_snapshot_t *snapshot
) {
  if (snapshot == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  for (size_t i = 0; i < lm_ctrl_settings_field_count(); ++i) {
    const lm_ctrl_settings_field_desc_t *field = &s_lm_ctrl_settings_fields[i];
    void *value = lm_ctrl_settings_snapshot_value_ptr(snapshot, field);
    size_t size = field->value_size;
    esp_err_t ret = ESP_OK;

    memset(value, 0, field->value_size);
    switch (field->kind) {
      case LM_CTRL_SETTINGS_VALUE_STR:
        ret = nvs_get_str(handle, field->key, (char *)value, &size);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
          continue;
        }
        if (ret != ESP_OK) {
          return ret;
        }
        break;
      case LM_CTRL_SETTINGS_VALUE_U8:
        ret = nvs_get_u8(handle, field->key, (uint8_t *)value);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
          continue;
        }
        if (ret != ESP_OK) {
          return ret;
        }
        break;
      case LM_CTRL_SETTINGS_VALUE_U32:
        ret = nvs_get_u32(handle, field->key, (uint32_t *)value);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
          continue;
        }
        if (ret != ESP_OK) {
          return ret;
        }
        break;
      case LM_CTRL_SETTINGS_VALUE_BLOB:
        ret = nvs_get_blob(handle, field->key, value, &size);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
          continue;
        }
        if (ret == ESP_ERR_NVS_INVALID_LENGTH &&
            (field->flags & LM_CTRL_SETTINGS_FIELD_FLAG_IGNORE_INVALID_LENGTH) != 0) {
          continue;
        }
        if (ret != ESP_OK) {
          return ret;
        }
        if (size != field->value_size) {
          if ((field->flags & LM_CTRL_SETTINGS_FIELD_FLAG_IGNORE_INVALID_LENGTH) != 0) {
            memset(value, 0, field->value_size);
            continue;
          }
          return ESP_ERR_INVALID_SIZE;
        }
        break;
      default:
        return ESP_ERR_INVALID_STATE;
    }

    lm_ctrl_settings_snapshot_mark_present(snapshot, field->field_id);
  }

  return ESP_OK;
}

static inline esp_err_t lm_ctrl_settings_snapshot_write_fields(
  nvs_handle_t handle,
  const lm_ctrl_settings_snapshot_t *snapshot,
  uint64_t field_mask
) {
  if (snapshot == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  for (size_t i = 0; i < lm_ctrl_settings_field_count(); ++i) {
    const lm_ctrl_settings_field_desc_t *field = &s_lm_ctrl_settings_fields[i];
    const void *value = NULL;
    uint64_t single_field_mask = LM_CTRL_SETTINGS_FIELD_MASK_VALUE(field->field_id);

    if ((field_mask & single_field_mask) == 0 ||
        !lm_ctrl_settings_snapshot_has(snapshot, field->field_id)) {
      continue;
    }

    value = lm_ctrl_settings_snapshot_value_cptr(snapshot, field);
    switch (field->kind) {
      case LM_CTRL_SETTINGS_VALUE_STR: {
        esp_err_t ret = nvs_set_str(handle, field->key, (const char *)value);

        if (ret != ESP_OK) {
          return ret;
        }
        break;
      }
      case LM_CTRL_SETTINGS_VALUE_U8: {
        esp_err_t ret = nvs_set_u8(handle, field->key, *(const uint8_t *)value);

        if (ret != ESP_OK) {
          return ret;
        }
        break;
      }
      case LM_CTRL_SETTINGS_VALUE_U32: {
        esp_err_t ret = nvs_set_u32(handle, field->key, *(const uint32_t *)value);

        if (ret != ESP_OK) {
          return ret;
        }
        break;
      }
      case LM_CTRL_SETTINGS_VALUE_BLOB: {
        esp_err_t ret = nvs_set_blob(handle, field->key, value, field->value_size);

        if (ret != ESP_OK) {
          return ret;
        }
        break;
      }
      default:
        return ESP_ERR_INVALID_STATE;
    }
  }

  return ESP_OK;
}

static inline esp_err_t lm_ctrl_settings_erase_fields(nvs_handle_t handle, uint64_t field_mask) {
  for (size_t i = 0; i < lm_ctrl_settings_field_count(); ++i) {
    const lm_ctrl_settings_field_desc_t *field = &s_lm_ctrl_settings_fields[i];
    uint64_t single_field_mask = LM_CTRL_SETTINGS_FIELD_MASK_VALUE(field->field_id);
    esp_err_t ret;

    if ((field_mask & single_field_mask) == 0) {
      continue;
    }

    ret = nvs_erase_key(handle, field->key);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
      return ret;
    }
  }

  return ESP_OK;
}
