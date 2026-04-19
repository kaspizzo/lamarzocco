#include "test_nvs.h"
#include "test_support.h"

#include <string.h>

#define lm_ctrl_settings_save_cloud_credentials lm_ctrl_settings_save_cloud_credentials_impl
#define lm_ctrl_settings_save_machine_selection lm_ctrl_settings_save_machine_selection_impl
#include "../../../main/controller_settings.c"
#undef lm_ctrl_settings_save_machine_selection
#undef lm_ctrl_settings_save_cloud_credentials

static void reset_controller_settings_test_state(void) {
  memset(&s_state, 0, sizeof(s_state));
  copy_text(s_state.hostname, sizeof(s_state.hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
}

void clear_cached_cloud_access_token_locked(void) {
  s_state.cloud_access_token[0] = '\0';
  s_state.cloud_access_token_valid_until_us = 0;
  s_state.cloud_ws_access_token[0] = '\0';
}

void clear_fleet_locked(void) {
  memset(s_state.fleet, 0, sizeof(s_state.fleet));
  s_state.fleet_count = 0;
}

void clear_selected_machine_locked(void) {
  memset(&s_state.selected_machine, 0, sizeof(s_state.selected_machine));
  s_state.has_machine_selection = false;
}

void clear_custom_logo_locked(void) {
  s_state.has_custom_logo = false;
  s_state.custom_logo_schema_version = 0;
  memset(s_state.custom_logo_blob, 0, sizeof(s_state.custom_logo_blob));
}

void lv_img_cache_invalidate_src(const void *src) {
  (void)src;
}

static int load_cloud_provisioning_blob(lm_ctrl_cloud_provisioning_blob_t *blob) {
  nvs_handle_t handle = 0;
  size_t blob_size = sizeof(*blob);
  esp_err_t ret;

  if (blob == NULL) {
    return 1;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READONLY, &handle);
  if (ret != ESP_OK) {
    return 1;
  }

  ret = nvs_get_blob(handle, LM_CTRL_WIFI_KEY_INSTALL_BLOB, blob, &blob_size);
  nvs_close(handle);
  if (ret != ESP_OK || blob_size != sizeof(*blob)) {
    return 1;
  }

  return 0;
}

static int test_ensure_cloud_provisioning_generates_and_persists_missing_installation(void) {
  lm_ctrl_cloud_provisioning_blob_t stored = {0};

  test_nvs_reset();
  reset_controller_settings_test_state();

  ASSERT_FALSE(test_nvs_has_key(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_INSTALL_BLOB));
  ASSERT_EQ_INT(ESP_OK, lm_ctrl_settings_ensure_cloud_provisioning());
  ASSERT_TRUE(test_nvs_has_key(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_INSTALL_BLOB));
  ASSERT_TRUE(s_state.has_cloud_provisioning);
  ASSERT_TRUE(s_state.cloud_installation_ready);
  ASSERT_FALSE(s_state.cloud_installation_registered);
  ASSERT_TRUE(strlen(s_state.cloud_installation_id) == 36U);
  ASSERT_TRUE(s_state.cloud_private_key_der_len > 0U);
  ASSERT_EQ_INT(0, load_cloud_provisioning_blob(&stored));
  ASSERT_EQ_INT(LM_CTRL_CLOUD_PROVISIONING_SCHEMA_VERSION, stored.schema_version);
  ASSERT_STREQ(s_state.cloud_installation_id, stored.installation_id);
  ASSERT_EQ_INT(0, memcmp(s_state.cloud_secret, stored.secret, sizeof(stored.secret)));
  ASSERT_EQ_INT(0, memcmp(s_state.cloud_private_key_der, stored.private_key_der, stored.private_key_der_len));
  ASSERT_EQ_INT((int)s_state.cloud_private_key_der_len, (int)stored.private_key_der_len);
  return 0;
}

static int test_reset_network_keeps_cloud_provisioning_blob_and_runtime_state(void) {
  lm_ctrl_cloud_provisioning_blob_t before = {0};
  lm_ctrl_cloud_provisioning_blob_t after = {0};

  test_nvs_reset();
  reset_controller_settings_test_state();
  ASSERT_EQ_INT(ESP_OK, lm_ctrl_settings_ensure_cloud_provisioning());
  ASSERT_EQ_INT(0, load_cloud_provisioning_blob(&before));

  ASSERT_EQ_INT(ESP_OK, test_nvs_seed_str(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_SSID, "TestWifi"));
  ASSERT_EQ_INT(ESP_OK, test_nvs_seed_str(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_PASS, "TestPass"));
  copy_text(s_state.sta_ssid, sizeof(s_state.sta_ssid), "TestWifi");
  copy_text(s_state.sta_password, sizeof(s_state.sta_password), "TestPass");
  s_state.has_credentials = true;

  ASSERT_EQ_INT(ESP_OK, lm_ctrl_settings_reset_network());
  ASSERT_TRUE(test_nvs_has_key(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_INSTALL_BLOB));
  ASSERT_TRUE(s_state.has_cloud_provisioning);
  ASSERT_TRUE(s_state.cloud_installation_ready);
  ASSERT_FALSE(s_state.has_credentials);
  ASSERT_STREQ("", s_state.sta_ssid);
  ASSERT_EQ_INT(0, load_cloud_provisioning_blob(&after));
  ASSERT_EQ_INT(0, memcmp(&before, &after, sizeof(before)));
  return 0;
}

static int test_factory_reset_clears_provisioning_and_next_ensure_regenerates_it(void) {
  char first_installation_id[LM_CTRL_INSTALLATION_ID_LEN];

  test_nvs_reset();
  reset_controller_settings_test_state();
  ASSERT_EQ_INT(ESP_OK, lm_ctrl_settings_ensure_cloud_provisioning());
  copy_text(first_installation_id, sizeof(first_installation_id), s_state.cloud_installation_id);
  ASSERT_TRUE(test_nvs_has_key(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_INSTALL_BLOB));

  ASSERT_EQ_INT(ESP_OK, lm_ctrl_settings_factory_reset());
  ASSERT_FALSE(test_nvs_has_key(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_INSTALL_BLOB));
  ASSERT_FALSE(s_state.has_cloud_provisioning);
  ASSERT_FALSE(s_state.cloud_installation_ready);
  ASSERT_STREQ("", s_state.cloud_installation_id);

  ASSERT_EQ_INT(ESP_OK, lm_ctrl_settings_ensure_cloud_provisioning());
  ASSERT_TRUE(test_nvs_has_key(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_INSTALL_BLOB));
  ASSERT_TRUE(s_state.has_cloud_provisioning);
  ASSERT_TRUE(strcmp(first_installation_id, s_state.cloud_installation_id) != 0);
  return 0;
}

int run_controller_settings_tests(void) {
  RUN_TEST(test_ensure_cloud_provisioning_generates_and_persists_missing_installation);
  RUN_TEST(test_reset_network_keeps_cloud_provisioning_blob_and_runtime_state);
  RUN_TEST(test_factory_reset_clears_provisioning_and_next_ensure_regenerates_it);
  return 0;
}
