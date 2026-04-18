#include "cloud_api.h"
#include "cJSON.h"
#include "machine_link_types.h"
#include "test_support.h"

#include <string.h>

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
      "{\"serialNumber\":\"LM123\",\"name\":\"Kitchen Micra\",\"modelName\":\"Micra\",\"bleAuthToken\":\"abc\"},"
      "{\"name\":\"Ignored Missing Serial\"},"
      "{\"serialNumber\":\"LM456\",\"name\":\"Studio Mini\",\"modelName\":\"Mini\",\"bleAuthToken\":\"def\"}"
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
  ASSERT_STREQ("LM456", machines[1].serial);
  ASSERT_STREQ("Studio Mini", machines[1].name);

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

int run_cloud_api_tests(void) {
  RUN_TEST(test_parse_access_token_success_and_invalid_payload);
  RUN_TEST(test_parse_customer_fleet_filters_invalid_entries);
  RUN_TEST(test_parse_dashboard_values_extracts_machine_and_bbw_state);
  RUN_TEST(test_parse_prebrew_widget_supports_both_widget_shapes);
  return 0;
}
