#include "test_nvs.h"
#include "test_support.h"

#include <string.h>

#include "../../../main/storage_security.c"

static int test_legacy_wifi_snapshot_preserves_portal_password_and_cloud_provisioning(void) {
  legacy_wifi_snapshot_t snapshot = {0};
  lm_ctrl_cloud_provisioning_blob_t expected_provisioning = {0};
  lm_ctrl_cloud_provisioning_blob_t restored_provisioning = {0};
  nvs_handle_t handle = 0;
  char portal_password[65] = {0};
  size_t portal_password_size = sizeof(portal_password);
  size_t provisioning_size = sizeof(restored_provisioning);
  uint8_t install_reg = 0;

  test_nvs_reset();

  expected_provisioning.schema_version = LM_CTRL_CLOUD_PROVISIONING_SCHEMA_VERSION;
  expected_provisioning.private_key_der_len = 8;
  strcpy(expected_provisioning.installation_id, "test-installation-id");
  for (size_t i = 0; i < sizeof(expected_provisioning.secret); ++i) {
    expected_provisioning.secret[i] = (uint8_t)(0x30U + i);
  }
  for (size_t i = 0; i < expected_provisioning.private_key_der_len; ++i) {
    expected_provisioning.private_key_der[i] = (uint8_t)(0x80U + i);
  }

  ASSERT_EQ_INT(ESP_OK, test_nvs_seed_str(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_PORTAL_PASS, "PortalPass42"));
  ASSERT_EQ_INT(ESP_OK, test_nvs_seed_blob(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_INSTALL_BLOB, &expected_provisioning, sizeof(expected_provisioning)));
  ASSERT_EQ_INT(ESP_OK, test_nvs_seed_u8(LM_CTRL_WIFI_NAMESPACE, LM_CTRL_WIFI_KEY_INSTALL_REG, 1));

  ASSERT_EQ_INT(ESP_OK, capture_legacy_wifi_snapshot(&snapshot));
  ASSERT_TRUE(snapshot.has_portal_password);
  ASSERT_STREQ("PortalPass42", snapshot.portal_password);
  ASSERT_TRUE(snapshot.has_cloud_provisioning);
  ASSERT_TRUE(snapshot.has_cloud_install_reg);
  ASSERT_EQ_INT(0, memcmp(&expected_provisioning, &snapshot.cloud_provisioning, sizeof(expected_provisioning)));
  ASSERT_EQ_INT(1, snapshot.cloud_install_reg);

  test_nvs_reset();
  ASSERT_EQ_INT(ESP_OK, restore_legacy_wifi_snapshot(&snapshot));

  ASSERT_EQ_INT(ESP_OK, nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READONLY, &handle));
  ASSERT_EQ_INT(ESP_OK, nvs_get_str(handle, LM_CTRL_WIFI_KEY_PORTAL_PASS, portal_password, &portal_password_size));
  ASSERT_STREQ("PortalPass42", portal_password);
  ASSERT_EQ_INT(ESP_OK, nvs_get_blob(handle, LM_CTRL_WIFI_KEY_INSTALL_BLOB, &restored_provisioning, &provisioning_size));
  ASSERT_EQ_INT((int)sizeof(restored_provisioning), (int)provisioning_size);
  ASSERT_EQ_INT(0, memcmp(&expected_provisioning, &restored_provisioning, sizeof(expected_provisioning)));
  ASSERT_EQ_INT(ESP_OK, nvs_get_u8(handle, LM_CTRL_WIFI_KEY_INSTALL_REG, &install_reg));
  ASSERT_EQ_INT(1, install_reg);
  nvs_close(handle);

  secure_zero(&snapshot, sizeof(snapshot));
  secure_zero(&expected_provisioning, sizeof(expected_provisioning));
  secure_zero(&restored_provisioning, sizeof(restored_provisioning));
  secure_zero(portal_password, sizeof(portal_password));
  return 0;
}

int run_storage_security_tests(void) {
  RUN_TEST(test_legacy_wifi_snapshot_preserves_portal_password_and_cloud_provisioning);
  return 0;
}
