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
  test_ctrl_recipe_values_t presets[CTRL_PRESET_COUNT];
} test_ctrl_persisted_state_t;

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
  ASSERT_FALSE(state.reset_confirm_yes);

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

  action = ctrl_confirm_setup_reset(&state);
  ASSERT_EQ_INT(CTRL_ACTION_NONE, action.type);
  ASSERT_EQ_INT(CTRL_SCREEN_SETUP, state.screen);

  ctrl_open_setup_reset(&state);
  ctrl_rotate(&state, 24);
  ctrl_rotate(&state, 1);
  ASSERT_TRUE(state.reset_confirm_yes);

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
  strncpy(state.presets[1].name, "Morning Flat", sizeof(state.presets[1].name) - 1);
  state.presets[1].values.temperature_c = 94.2f;

  ASSERT_EQ_INT(ESP_OK, ctrl_state_persist(&state));

  ctrl_state_init(&loaded);
  ASSERT_EQ_INT(ESP_OK, ctrl_state_load(&loaded));
  ASSERT_FLOAT_EQ(95.7f, loaded.values.temperature_c, 0.0001f);
  ASSERT_FLOAT_EQ(1.8f, loaded.values.infuse_s, 0.0001f);
  ASSERT_FLOAT_EQ(2.9f, loaded.values.pause_s, 0.0001f);
  ASSERT_STREQ("Morning Flat", loaded.presets[1].name);
  ASSERT_FLOAT_EQ(94.2f, loaded.presets[1].values.temperature_c, 0.0001f);

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
  ASSERT_STREQ("Preset 1", state.presets[0].name);
  ASSERT_FLOAT_EQ(91.1f, state.presets[0].values.temperature_c, 0.0001f);
  ASSERT_STREQ("Preset 4", state.presets[3].name);
  ASSERT_FLOAT_EQ(42.0f, state.presets[3].values.bbw_dose_2_g, 0.0001f);

  ASSERT_TRUE(test_nvs_has_key("ctrl_state", "schema"));
  ASSERT_TRUE(test_nvs_has_key("ctrl_state", "current"));
  ASSERT_TRUE(test_nvs_has_key("ctrl_state", "preset0"));
  ASSERT_FALSE(test_nvs_has_key("ctrl_state", "persisted"));

  return 0;
}

int run_controller_state_tests(void) {
  RUN_TEST(test_rotate_clamps_and_updates_active_screen);
  RUN_TEST(test_save_and_load_preset_roundtrip);
  RUN_TEST(test_setup_reset_flow_requires_arm_and_confirm);
  RUN_TEST(test_persist_and_reload_state_roundtrip);
  RUN_TEST(test_legacy_blob_is_loaded_and_migrated);
  return 0;
}
