#include "cloud_api.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "machine_link_types.h"
#include "test_support.h"

#include <string.h>

static int test_generate_installation_secret_matches_reference_vector(void) {
  static const uint8_t public_key_der[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  static const uint8_t expected_secret[32] = {
    0x53, 0x1b, 0x9e, 0x0e, 0x2f, 0x25, 0x73, 0x84,
    0x88, 0x35, 0xf4, 0x04, 0x21, 0x19, 0x1a, 0xfd,
    0xe8, 0x79, 0x94, 0x7a, 0xe8, 0x53, 0x57, 0x49,
    0xc7, 0xd4, 0x44, 0x3c, 0x4c, 0xc3, 0x46, 0xa1,
  };
  uint8_t secret[32] = {0};
  char secret_b64[64] = {0};
  size_t secret_b64_len = 0;

  ASSERT_EQ_INT(
    ESP_OK,
    lm_ctrl_cloud_generate_installation_secret(
      "28af7c9e-36cf-4f82-b4c6-0181adc3f59f",
      public_key_der,
      sizeof(public_key_der),
      secret
    )
  );
  ASSERT_EQ_INT(0, memcmp(expected_secret, secret, sizeof(secret)));
  ASSERT_EQ_INT(0, mbedtls_base64_encode((unsigned char *)secret_b64, sizeof(secret_b64), &secret_b64_len, secret, sizeof(secret)));
  secret_b64[secret_b64_len] = '\0';
  ASSERT_STREQ("UxueDi8lc4SINfQEIRka/eh5lHroU1dJx9REPEzDRqE=", secret_b64);
  return 0;
}

static int test_generate_installation_populates_uuid_secret_and_private_key(void) {
  lm_ctrl_cloud_installation_t installation = {0};

  ASSERT_EQ_INT(ESP_OK, lm_ctrl_cloud_generate_installation(&installation));
  ASSERT_TRUE(strlen(installation.installation_id) == 36U);
  ASSERT_TRUE(installation.installation_id[8] == '-');
  ASSERT_TRUE(installation.installation_id[13] == '-');
  ASSERT_TRUE(installation.installation_id[18] == '-');
  ASSERT_TRUE(installation.installation_id[23] == '-');
  ASSERT_TRUE(installation.private_key_der_len > 0U);
  ASSERT_TRUE(installation.private_key_der_len <= sizeof(installation.private_key_der));
  ASSERT_FALSE(installation.secret[0] == 0 && installation.secret[1] == 0 && installation.secret[2] == 0);
  return 0;
}

static int test_parse_access_token_success_and_invalid_payload(void) {
  char access_token[64];

  ASSERT_EQ_INT(
    ESP_OK,
    lm_ctrl_cloud_parse_access_token("{\"accessToken\":\"token-123\"}", access_token, sizeof(access_token))
  );
  ASSERT_STREQ("token-123", access_token);

  ASSERT_EQ_INT(
    ESP_ERR_INVALID_RESPONSE,
    lm_ctrl_cloud_parse_access_token("{\"missing\":true}", access_token, sizeof(access_token))
  );

  return 0;
}

static int test_parse_customer_fleet_filters_invalid_entries(void) {
  static const char *response_body =
    "["
      "{\"serialNumber\":\"LM123\",\"name\":\"Kitchen Micra\",\"modelName\":\"Micra\",\"bleAuthToken\":\"abc\",\"connected\":true,\"offlineMode\":false},"
      "{\"name\":\"Ignored Missing Serial\"},"
      "{\"serialNumber\":\"LM456\",\"name\":\"Studio Mini\",\"modelName\":\"Mini\",\"bleAuthToken\":\"def\",\"connected\":false,\"offlineMode\":true}"
    "]";
  lm_ctrl_cloud_machine_t machines[4];
  size_t machine_count = 0;

  ASSERT_EQ_INT(
    ESP_OK,
    lm_ctrl_cloud_parse_customer_fleet(response_body, machines, 4, &machine_count)
  );
  ASSERT_EQ_INT(2, (int)machine_count);
  ASSERT_STREQ("LM123", machines[0].serial);
  ASSERT_STREQ("Kitchen Micra", machines[0].name);
  ASSERT_STREQ("Micra", machines[0].model);
  ASSERT_STREQ("abc", machines[0].communication_key);
  ASSERT_TRUE(machines[0].cloud_status.connected_known);
  ASSERT_TRUE(machines[0].cloud_status.connected);
  ASSERT_TRUE(machines[0].cloud_status.offline_mode_known);
  ASSERT_FALSE(machines[0].cloud_status.offline_mode);
  ASSERT_STREQ("LM456", machines[1].serial);
  ASSERT_STREQ("Studio Mini", machines[1].name);
  ASSERT_TRUE(machines[1].cloud_status.connected_known);
  ASSERT_FALSE(machines[1].cloud_status.connected);
  ASSERT_TRUE(machines[1].cloud_status.offline_mode_known);
  ASSERT_TRUE(machines[1].cloud_status.offline_mode);

  return 0;
}

static int test_parse_dashboard_machine_status_extracts_online_signal(void) {
  cJSON *root = cJSON_Parse("{\"connected\":true,\"offlineMode\":false}");
  lm_ctrl_cloud_machine_status_t status = {0};

  ASSERT_TRUE(root != NULL);
  ASSERT_TRUE(lm_ctrl_cloud_parse_dashboard_machine_status(root, &status));
  ASSERT_TRUE(status.connected_known);
  ASSERT_TRUE(status.connected);
  ASSERT_TRUE(status.offline_mode_known);
  ASSERT_FALSE(status.offline_mode);
  ASSERT_TRUE(lm_ctrl_cloud_machine_status_is_online(&status));

  cJSON_Delete(root);
  return 0;
}

static int test_parse_dashboard_values_extracts_machine_and_bbw_state(void) {
  static const char *response_body =
    "{"
      "\"widgets\":["
        "{\"code\":\"CMMachineStatus\",\"output\":{\"mode\":\"BrewingMode\",\"brewingStartTime\":1700000000123}},"
        "{\"code\":\"CMCoffeeBoiler\",\"output\":{\"targetTemperature\":94.5}},"
        "{\"code\":\"CMSteamBoilerLevel\",\"output\":{\"enabled\":true,\"targetLevel\":\"Level3\"}},"
        "{\"code\":\"CMPreExtraction\",\"output\":{\"In\":{\"seconds\":{\"PreBrewing\":1.5}},\"Out\":{\"seconds\":{\"PreBrewing\":2.5}}}},"
        "{\"code\":\"CMBrewByWeightDoses\",\"output\":{\"mode\":\"Dose2\",\"doses\":{\"Dose1\":{\"dose\":31.2},\"Dose2\":{\"dose\":33.4}}}}"
      "]"
    "}";
  cJSON *root = cJSON_Parse(response_body);
  ctrl_values_t values = {0};
  uint32_t loaded_mask = 0;
  uint32_t feature_mask = 0;
  bool brew_active = false;
  int64_t brew_start_epoch_ms = 0;

  ASSERT_TRUE(root != NULL);
  ASSERT_EQ_INT(
    ESP_OK,
    lm_ctrl_cloud_parse_dashboard_root_values(
      root,
      &values,
      &loaded_mask,
      &feature_mask,
      NULL,
      &brew_active,
      &brew_start_epoch_ms
    )
  );
  ASSERT_TRUE((loaded_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0);
  ASSERT_TRUE((loaded_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0);
  ASSERT_TRUE((loaded_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0);
  ASSERT_TRUE((loaded_mask & LM_CTRL_MACHINE_FIELD_PREBREWING) == LM_CTRL_MACHINE_FIELD_PREBREWING);
  ASSERT_TRUE((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0);
  ASSERT_TRUE((feature_mask & LM_CTRL_MACHINE_FEATURE_BBW) != 0);
  ASSERT_FLOAT_EQ(94.5f, values.temperature_c, 0.0001f);
  ASSERT_EQ_INT(CTRL_STEAM_LEVEL_3, values.steam_level);
  ASSERT_FALSE(values.standby_on);
  ASSERT_FLOAT_EQ(1.5f, values.infuse_s, 0.0001f);
  ASSERT_FLOAT_EQ(2.5f, values.pause_s, 0.0001f);
  ASSERT_EQ_INT(CTRL_BBW_MODE_DOSE_2, values.bbw_mode);
  ASSERT_FLOAT_EQ(31.2f, values.bbw_dose_1_g, 0.0001f);
  ASSERT_FLOAT_EQ(33.4f, values.bbw_dose_2_g, 0.0001f);
  ASSERT_TRUE(brew_active);
  ASSERT_EQ_I64(1700000000123LL, brew_start_epoch_ms);

  cJSON_Delete(root);
  return 0;
}

static int test_parse_prebrew_widget_supports_both_widget_shapes(void) {
  static const char *classic_widget =
    "{\"code\":\"CMPreBrewing\",\"output\":{\"times\":{\"PreBrewing\":[{\"seconds\":{\"In\":0.7,\"Out\":1.3}}]}}}";
  static const char *modern_widget =
    "{\"code\":\"CMPreExtraction\",\"output\":{\"In\":{\"seconds\":{\"PreBrewing\":0.9}},\"Out\":{\"seconds\":{\"PreBrewing\":1.4}}}}";
  cJSON *widget = cJSON_Parse(classic_widget);
  float seconds_in = 0.0f;
  float seconds_out = 0.0f;

  ASSERT_TRUE(widget != NULL);
  ASSERT_TRUE(lm_ctrl_cloud_parse_prebrew_widget_values(widget, &seconds_in, &seconds_out));
  ASSERT_FLOAT_EQ(0.7f, seconds_in, 0.0001f);
  ASSERT_FLOAT_EQ(1.3f, seconds_out, 0.0001f);
  cJSON_Delete(widget);

  widget = cJSON_Parse(modern_widget);
  ASSERT_TRUE(widget != NULL);
  ASSERT_TRUE(lm_ctrl_cloud_parse_prebrew_widget_values(widget, &seconds_in, &seconds_out));
  ASSERT_FLOAT_EQ(0.9f, seconds_in, 0.0001f);
  ASSERT_FLOAT_EQ(1.4f, seconds_out, 0.0001f);
  cJSON_Delete(widget);
  return 0;
}

static int test_parse_dashboard_values_prefers_latest_ready_time_across_heating_boilers(void) {
  static const char *response_body =
    "{"
      "\"widgets\":["
        "{\"code\":\"CMCoffeeBoiler\",\"output\":{\"status\":\"HeatingUp\",\"readyStartTime\":1700000005000}},"
        "{\"code\":\"CMSteamBoilerLevel\",\"output\":{\"status\":\"HeatingUp\",\"readyStartTime\":1700000015000}}"
      "]"
    "}";
  cJSON *root = cJSON_Parse(response_body);
  ctrl_values_t values = {0};
  uint32_t loaded_mask = 0;
  uint32_t feature_mask = 0;
  lm_ctrl_machine_heat_info_t heat_info = {0};

  ASSERT_TRUE(root != NULL);
  ASSERT_EQ_INT(
    ESP_OK,
    lm_ctrl_cloud_parse_dashboard_root_values(
      root,
      &values,
      &loaded_mask,
      &feature_mask,
      &heat_info,
      NULL,
      NULL
    )
  );
  ASSERT_TRUE(heat_info.available);
  ASSERT_TRUE(heat_info.heating);
  ASSERT_TRUE(heat_info.eta_available);
  ASSERT_TRUE(heat_info.coffee_heating);
  ASSERT_TRUE(heat_info.steam_heating);
  ASSERT_TRUE(heat_info.coffee_eta_available);
  ASSERT_TRUE(heat_info.steam_eta_available);
  ASSERT_EQ_I64(1700000015000LL, heat_info.ready_epoch_ms);
  ASSERT_EQ_I64(1700000005000LL, heat_info.coffee_ready_epoch_ms);
  ASSERT_EQ_I64(1700000015000LL, heat_info.steam_ready_epoch_ms);

  cJSON_Delete(root);
  return 0;
}

static int test_parse_dashboard_values_falls_back_to_coffee_eta_when_steam_is_not_heating(void) {
  static const char *response_body =
    "{"
      "\"widgets\":["
        "{\"code\":\"CMCoffeeBoiler\",\"output\":{\"status\":\"HeatingUp\",\"readyStartTime\":1700000005000}},"
        "{\"code\":\"CMSteamBoilerLevel\",\"output\":{\"enabled\":false,\"status\":\"Ready\",\"readyStartTime\":1700000015000}}"
      "]"
    "}";
  cJSON *root = cJSON_Parse(response_body);
  ctrl_values_t values = {0};
  uint32_t loaded_mask = 0;
  uint32_t feature_mask = 0;
  lm_ctrl_machine_heat_info_t heat_info = {0};

  ASSERT_TRUE(root != NULL);
  ASSERT_EQ_INT(
    ESP_OK,
    lm_ctrl_cloud_parse_dashboard_root_values(
      root,
      &values,
      &loaded_mask,
      &feature_mask,
      &heat_info,
      NULL,
      NULL
    )
  );
  ASSERT_TRUE(heat_info.available);
  ASSERT_TRUE(heat_info.heating);
  ASSERT_TRUE(heat_info.eta_available);
  ASSERT_TRUE(heat_info.coffee_heating);
  ASSERT_FALSE(heat_info.steam_heating);
  ASSERT_TRUE(heat_info.coffee_eta_available);
  ASSERT_FALSE(heat_info.steam_eta_available);
  ASSERT_EQ_I64(1700000005000LL, heat_info.ready_epoch_ms);
  ASSERT_EQ_I64(1700000005000LL, heat_info.coffee_ready_epoch_ms);
  ASSERT_EQ_I64(0LL, heat_info.steam_ready_epoch_ms);

  cJSON_Delete(root);
  return 0;
}

int run_cloud_api_tests(void) {
  RUN_TEST(test_generate_installation_secret_matches_reference_vector);
  RUN_TEST(test_generate_installation_populates_uuid_secret_and_private_key);
  RUN_TEST(test_parse_access_token_success_and_invalid_payload);
  RUN_TEST(test_parse_customer_fleet_filters_invalid_entries);
  RUN_TEST(test_parse_dashboard_machine_status_extracts_online_signal);
  RUN_TEST(test_parse_dashboard_values_extracts_machine_and_bbw_state);
  RUN_TEST(test_parse_prebrew_widget_supports_both_widget_shapes);
  RUN_TEST(test_parse_dashboard_values_prefers_latest_ready_time_across_heating_boilers);
  RUN_TEST(test_parse_dashboard_values_falls_back_to_coffee_eta_when_steam_is_not_heating);
  return 0;
}
