#include "controller_state.h"
#include "test_nvs.h"
#include "test_support.h"

#include <string.h>

typedef struct {
  float temperature_c;
  float infuse_s;
  float pause_s;
  uint8_t bbw_mode;
  float bbw_dose_1_g;
  float bbw_dose_2_g;
} test_ctrl_recipe_values_t;

typedef struct {
  test_ctrl_recipe_values_t values;
  test_ctrl_recipe_values_t presets[CTRL_PRESET_DEFAULT_COUNT];
} test_ctrl_persisted_state_t;

typedef struct {
  char name[CTRL_PRESET_NAME_LEN];
  test_ctrl_recipe_values_t values;
} test_ctrl_persisted_preset_t;

typedef struct {
  float temperature_step_c;
  float time_step_s;
  uint8_t preset_count;
  uint8_t reserved[3];
} test_ctrl_persisted_settings_t;

static int test_rotate_clamps_and_updates_active_screen(void) {
  ctrl_state_t state;

  ctrl_state_init(&state);
  ctrl_set_focus(&state, CTRL_FOCUS_TEMPERATURE);
  ctrl_rotate(&state, 200);
  ASSERT_FLOAT_EQ(103.0f, state.values.temperature_c, 0.0001f);

  ctrl_rotate(&state, -500);
  ASSERT_FLOAT_EQ(80.0f, state.values.temperature_c, 0.0001f);

  ctrl_open_presets(&state);
  ctrl_rotate(&state, 1);
  ASSERT_EQ_INT(CTRL_SCREEN_PRESETS, state.screen);
  ASSERT_EQ_INT(1, state.preset_index);

  ctrl_open_setup_reset(&state);
  ctrl_rotate(&state, 24);
  ASSERT_EQ_INT(CTRL_SCREEN_SETUP_RESET_CONFIRM, state.screen);
  ASSERT_EQ_INT(CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD, state.recovery_action);

  return 0;
}

static int test_rotate_uses_configured_steps_and_active_preset_count(void) {
  ctrl_state_t state;

  ctrl_state_init(&state);
  state.temperature_step_c = 0.5f;
  state.time_step_s = 0.5f;
  state.preset_count = 2;

  ctrl_set_focus(&state, CTRL_FOCUS_TEMPERATURE);
  ctrl_rotate(&state, 1);
  ASSERT_FLOAT_EQ(93.5f, state.values.temperature_c, 0.0001f);

  ctrl_set_focus(&state, CTRL_FOCUS_INFUSE);
  ctrl_rotate(&state, 1);
  ASSERT_FLOAT_EQ(1.5f, state.values.infuse_s, 0.0001f);

  ctrl_set_focus(&state, CTRL_FOCUS_PAUSE);
  ctrl_rotate(&state, -2);
  ASSERT_FLOAT_EQ(1.0f, state.values.pause_s, 0.0001f);

  ctrl_open_presets(&state);
  state.preset_index = 1;
  ctrl_rotate(&state, 1);
  ASSERT_EQ_INT(0, state.preset_index);

  return 0;
}

static int test_rotate_status_maps_left_to_standby_and_right_to_on(void) {
  ctrl_state_t state;

  ctrl_state_init(&state);
  ctrl_set_focus(&state, CTRL_FOCUS_STANDBY);

  state.values.standby_on = false;
  ctrl_rotate(&state, -1);
  ASSERT_TRUE(state.values.standby_on);

  ctrl_rotate(&state, +1);
  ASSERT_FALSE(state.values.standby_on);

  return 0;
}

static int test_save_and_load_preset_roundtrip(void) {
  ctrl_state_t state;
  ctrl_action_t action;

  ctrl_state_init(&state);
  state.values.temperature_c = 120.0f;
  state.values.infuse_s = -1.0f;
  state.values.pause_s = 10.0f;
  state.values.bbw_mode = (ctrl_bbw_mode_t)99;
  state.values.bbw_dose_1_g = 2.0f;
  state.values.bbw_dose_2_g = 110.0f;
  ctrl_open_presets(&state);
  state.preset_index = 2;

  action = ctrl_save_preset(&state);
  ASSERT_EQ_INT(CTRL_ACTION_SAVE_PRESET, action.type);
  ASSERT_EQ_INT(2, action.preset_slot);
  ASSERT_FLOAT_EQ(103.0f, state.presets[2].values.temperature_c, 0.0001f);
  ASSERT_FLOAT_EQ(0.0f, state.presets[2].values.infuse_s, 0.0001f);
  ASSERT_FLOAT_EQ(9.0f, state.presets[2].values.pause_s, 0.0001f);
  ASSERT_EQ_INT(CTRL_BBW_MODE_DOSE_1, state.presets[2].values.bbw_mode);
  ASSERT_FLOAT_EQ(5.0f, state.presets[2].values.bbw_dose_1_g, 0.0001f);
  ASSERT_FLOAT_EQ(100.0f, state.presets[2].values.bbw_dose_2_g, 0.0001f);

  state.values.temperature_c = 90.0f;
  state.values.infuse_s = 1.0f;
  state.values.pause_s = 2.0f;
  ctrl_open_presets(&state);
  state.preset_index = 2;
  action = ctrl_load_preset(&state);
  ASSERT_EQ_INT(CTRL_ACTION_LOAD_PRESET, action.type);
  ASSERT_EQ_INT(CTRL_SCREEN_MAIN, state.screen);
  ASSERT_FLOAT_EQ(103.0f, state.values.temperature_c, 0.0001f);
  ASSERT_FLOAT_EQ(0.0f, state.values.infuse_s, 0.0001f);
  ASSERT_FLOAT_EQ(9.0f, state.values.pause_s, 0.0001f);

  return 0;
}

static int test_setup_reset_flow_requires_arm_and_confirm(void) {
  ctrl_state_t state;
  ctrl_action_t action;

  ctrl_state_init(&state);
  ctrl_open_setup_reset(&state);

  action = ctrl_confirm_setup_reset(&state);
  ASSERT_EQ_INT(CTRL_ACTION_NONE, action.type);

  ctrl_rotate(&state, 24);
  ASSERT_EQ_INT(CTRL_SCREEN_SETUP_RESET_CONFIRM, state.screen);
  ASSERT_EQ_INT(CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD, state.recovery_action);

  action = ctrl_confirm_setup_reset(&state);
  ASSERT_EQ_INT(CTRL_ACTION_CLEAR_WEB_PASSWORD, action.type);
  ASSERT_EQ_INT(CTRL_SCREEN_SETUP, state.screen);

  ctrl_open_setup_reset(&state);
  ctrl_rotate(&state, 24);
  ctrl_rotate(&state, 1);
  ASSERT_EQ_INT(CTRL_RECOVERY_ACTION_RESET_NETWORK, state.recovery_action);

  action = ctrl_confirm_setup_reset(&state);
  ASSERT_EQ_INT(CTRL_ACTION_RESET_NETWORK, action.type);
  ASSERT_EQ_INT(CTRL_SCREEN_SETUP, state.screen);

  return 0;
}

static int test_persist_and_reload_state_roundtrip(void) {
  ctrl_state_t state;
  ctrl_state_t loaded;

  test_nvs_reset();
  ctrl_state_init(&state);
  state.values.temperature_c = 95.7f;
  state.values.infuse_s = 1.8f;
  state.values.pause_s = 2.9f;
  state.preset_count = 6;
  state.temperature_step_c = 0.5f;
  state.time_step_s = 0.5f;
  strncpy(state.presets[1].name, "Morning Flat", sizeof(state.presets[1].name) - 1);
  strncpy(state.presets[5].name, "Weekend Flat", sizeof(state.presets[5].name) - 1);
  state.presets[1].values.temperature_c = 94.2f;
  state.presets[5].values.temperature_c = 95.0f;

  ASSERT_EQ_INT(ESP_OK, ctrl_state_persist(&state));

  ctrl_state_init(&loaded);
  ASSERT_EQ_INT(ESP_OK, ctrl_state_load(&loaded));
  ASSERT_FLOAT_EQ(95.5f, loaded.values.temperature_c, 0.0001f);
  ASSERT_FLOAT_EQ(2.0f, loaded.values.infuse_s, 0.0001f);
  ASSERT_FLOAT_EQ(3.0f, loaded.values.pause_s, 0.0001f);
  ASSERT_EQ_INT(6, loaded.preset_count);
  ASSERT_FLOAT_EQ(0.5f, loaded.temperature_step_c, 0.0001f);
  ASSERT_FLOAT_EQ(0.5f, loaded.time_step_s, 0.0001f);
  ASSERT_STREQ("Morning Flat", loaded.presets[1].name);
  ASSERT_STREQ("Weekend Flat", loaded.presets[5].name);
  ASSERT_FLOAT_EQ(94.0f, loaded.presets[1].values.temperature_c, 0.0001f);
  ASSERT_FLOAT_EQ(95.0f, loaded.presets[5].values.temperature_c, 0.0001f);

  return 0;
}

static int test_legacy_blob_is_loaded_and_migrated(void) {
  ctrl_state_t state;
  test_ctrl_persisted_state_t legacy = {0};

  test_nvs_reset();
  legacy.values.temperature_c = 96.4f;
  legacy.values.infuse_s = 1.3f;
  legacy.values.pause_s = 2.7f;
  legacy.values.bbw_mode = CTRL_BBW_MODE_CONTINUOUS;
  legacy.values.bbw_dose_1_g = 30.5f;
  legacy.values.bbw_dose_2_g = 36.0f;
  legacy.presets[0].temperature_c = 91.1f;
  legacy.presets[0].infuse_s = 0.5f;
  legacy.presets[0].pause_s = 1.0f;
  legacy.presets[3].temperature_c = 97.0f;
  legacy.presets[3].bbw_dose_2_g = 42.0f;

  ASSERT_EQ_INT(
    ESP_OK,
    test_nvs_seed_blob("ctrl_state", "persisted", &legacy, sizeof(legacy))
  );

  ctrl_state_init(&state);
  ASSERT_EQ_INT(ESP_OK, ctrl_state_load(&state));
  ASSERT_FLOAT_EQ(96.4f, state.values.temperature_c, 0.0001f);
  ASSERT_FLOAT_EQ(1.3f, state.values.infuse_s, 0.0001f);
  ASSERT_FLOAT_EQ(2.7f, state.values.pause_s, 0.0001f);
  ASSERT_EQ_INT(CTRL_BBW_MODE_CONTINUOUS, state.values.bbw_mode);
  ASSERT_EQ_INT(CTRL_PRESET_DEFAULT_COUNT, state.preset_count);
  ASSERT_FLOAT_EQ(CTRL_TEMPERATURE_STEP_DEFAULT_C, state.temperature_step_c, 0.0001f);
  ASSERT_FLOAT_EQ(CTRL_TIME_STEP_DEFAULT_S, state.time_step_s, 0.0001f);
  ASSERT_STREQ("Preset 1", state.presets[0].name);
  ASSERT_FLOAT_EQ(91.1f, state.presets[0].values.temperature_c, 0.0001f);
  ASSERT_STREQ("Preset 4", state.presets[3].name);
  ASSERT_FLOAT_EQ(42.0f, state.presets[3].values.bbw_dose_2_g, 0.0001f);

  ASSERT_TRUE(test_nvs_has_key("ctrl_state", "schema"));
  ASSERT_TRUE(test_nvs_has_key("ctrl_state", "current"));
  ASSERT_TRUE(test_nvs_has_key("ctrl_state", "settings"));
  ASSERT_TRUE(test_nvs_has_key("ctrl_state", "preset0"));
  ASSERT_FALSE(test_nvs_has_key("ctrl_state", "persisted"));

  return 0;
}

static int test_versioned_schema_three_defaults_to_new_advanced_settings(void) {
  ctrl_state_t state;
  test_ctrl_recipe_values_t current = {0};
  test_ctrl_persisted_preset_t preset = {0};

  test_nvs_reset();
  current.temperature_c = 94.3f;
  current.infuse_s = 1.2f;
  current.pause_s = 2.4f;
  current.bbw_mode = CTRL_BBW_MODE_DOSE_2;
  current.bbw_dose_1_g = 31.0f;
  current.bbw_dose_2_g = 35.0f;
  strcpy(preset.name, "Schema Three");
  preset.values.temperature_c = 92.1f;
  preset.values.infuse_s = 0.6f;
  preset.values.pause_s = 1.1f;

  ASSERT_EQ_INT(ESP_OK, test_nvs_seed_u8("ctrl_state", "schema", 3));
  ASSERT_EQ_INT(ESP_OK, test_nvs_seed_blob("ctrl_state", "current", &current, sizeof(current)));
  ASSERT_EQ_INT(ESP_OK, test_nvs_seed_blob("ctrl_state", "preset0", &preset, sizeof(preset)));

  ctrl_state_init(&state);
  ASSERT_EQ_INT(ESP_OK, ctrl_state_load(&state));
  ASSERT_EQ_INT(CTRL_PRESET_DEFAULT_COUNT, state.preset_count);
  ASSERT_FLOAT_EQ(CTRL_TEMPERATURE_STEP_DEFAULT_C, state.temperature_step_c, 0.0001f);
  ASSERT_FLOAT_EQ(CTRL_TIME_STEP_DEFAULT_S, state.time_step_s, 0.0001f);
  ASSERT_STREQ("Schema Three", state.presets[0].name);
  ASSERT_TRUE(test_nvs_has_key("ctrl_state", "settings"));
  ASSERT_TRUE(test_nvs_has_key("ctrl_state", "preset5"));

  return 0;
}

static int test_update_advanced_settings_rounds_values_and_deletes_hidden_presets(void) {
  ctrl_state_t state;
  ctrl_state_t loaded;

  test_nvs_reset();
  ctrl_state_init(&state);
  state.preset_count = 6;
  state.values.temperature_c = 93.6f;
  state.values.infuse_s = 1.2f;
  state.values.pause_s = 2.7f;
  strcpy(state.presets[4].name, "Deleted Later");
  state.presets[4].values.temperature_c = 91.2f;
  strcpy(state.presets[5].name, "Deleted Too");
  state.presets[5].values.pause_s = 4.1f;
  ASSERT_EQ_INT(ESP_OK, ctrl_state_persist(&state));

  ASSERT_EQ_INT(ESP_OK, ctrl_state_update_advanced_settings(4, 0.5f, 0.5f));

  ctrl_state_init(&loaded);
  ASSERT_EQ_INT(ESP_OK, ctrl_state_load(&loaded));
  ASSERT_EQ_INT(4, loaded.preset_count);
  ASSERT_FLOAT_EQ(0.5f, loaded.temperature_step_c, 0.0001f);
  ASSERT_FLOAT_EQ(0.5f, loaded.time_step_s, 0.0001f);
  ASSERT_FLOAT_EQ(93.5f, loaded.values.temperature_c, 0.0001f);
  ASSERT_FLOAT_EQ(1.0f, loaded.values.infuse_s, 0.0001f);
  ASSERT_FLOAT_EQ(2.5f, loaded.values.pause_s, 0.0001f);
  ASSERT_STREQ("Preset 5", loaded.presets[4].name);
  ASSERT_STREQ("Preset 6", loaded.presets[5].name);
  ASSERT_FLOAT_EQ(93.0f, loaded.presets[4].values.temperature_c, 0.0001f);
  ASSERT_FLOAT_EQ(2.0f, loaded.presets[5].values.pause_s, 0.0001f);

  return 0;
}

static int test_step_helpers_accept_supported_values_only(void) {
  ASSERT_TRUE(ctrl_state_is_supported_preset_count(2));
  ASSERT_TRUE(ctrl_state_is_supported_preset_count(6));
  ASSERT_FALSE(ctrl_state_is_supported_preset_count(1));
  ASSERT_FALSE(ctrl_state_is_supported_preset_count(7));
  ASSERT_TRUE(ctrl_state_is_supported_edit_step(0.1f));
  ASSERT_TRUE(ctrl_state_is_supported_edit_step(0.5f));
  ASSERT_FALSE(ctrl_state_is_supported_edit_step(0.2f));
  ASSERT_TRUE(ctrl_state_value_matches_step(93.5f, 80.0f, 103.0f, 0.5f));
  ASSERT_FALSE(ctrl_state_value_matches_step(93.6f, 80.0f, 103.0f, 0.5f));
  ASSERT_FALSE(ctrl_state_value_matches_step(79.5f, 80.0f, 103.0f, 0.5f));
  return 0;
}

int run_controller_state_tests(void) {
  RUN_TEST(test_rotate_clamps_and_updates_active_screen);
  RUN_TEST(test_rotate_uses_configured_steps_and_active_preset_count);
  RUN_TEST(test_rotate_status_maps_left_to_standby_and_right_to_on);
  RUN_TEST(test_save_and_load_preset_roundtrip);
  RUN_TEST(test_setup_reset_flow_requires_arm_and_confirm);
  RUN_TEST(test_persist_and_reload_state_roundtrip);
  RUN_TEST(test_legacy_blob_is_loaded_and_migrated);
  RUN_TEST(test_versioned_schema_three_defaults_to_new_advanced_settings);
  RUN_TEST(test_update_advanced_settings_rounds_values_and_deletes_hidden_presets);
  RUN_TEST(test_step_helpers_accept_supported_values_only);
  return 0;
}
