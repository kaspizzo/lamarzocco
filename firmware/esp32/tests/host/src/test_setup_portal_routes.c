#include "setup_portal_routes.h"
#include "test_support.h"

static int test_parse_advanced_form_reads_supported_values(void) {
  lm_ctrl_setup_portal_advanced_form_t form = {0};

  ASSERT_TRUE(
    lm_ctrl_setup_portal_parse_advanced_form(
      "preset_count=6&temperature_step_c=0.5&time_step_s=0.1&preset_reduce_confirm=1",
      &form
    )
  );
  ASSERT_EQ_INT(6, form.preset_count);
  ASSERT_FLOAT_EQ(0.5f, form.temperature_step_c, 0.0001f);
  ASSERT_FLOAT_EQ(0.1f, form.time_step_s, 0.0001f);
  ASSERT_TRUE(form.preset_reduce_confirmed);

  ASSERT_FALSE(lm_ctrl_setup_portal_parse_advanced_form("preset_count=abc&temperature_step_c=0.5&time_step_s=0.5", &form));
  ASSERT_FALSE(lm_ctrl_setup_portal_parse_advanced_form("preset_count=4&temperature_step_c=0.5", &form));
  return 0;
}

static int test_validate_advanced_form_rejects_invalid_values_and_missing_confirm(void) {
  ctrl_state_t state;
  lm_ctrl_setup_portal_advanced_form_t form = {0};
  char error_text[128];

  ctrl_state_init(&state);
  state.preset_count = 6;

  form = (lm_ctrl_setup_portal_advanced_form_t){
    .preset_count = 1,
    .temperature_step_c = 0.1f,
    .time_step_s = 0.1f,
  };
  ASSERT_FALSE(lm_ctrl_setup_portal_validate_advanced_form(&form, &state, error_text, sizeof(error_text)));
  ASSERT_CONTAINS(error_text, "Preset count");

  form = (lm_ctrl_setup_portal_advanced_form_t){
    .preset_count = 6,
    .temperature_step_c = 0.2f,
    .time_step_s = 0.1f,
  };
  ASSERT_FALSE(lm_ctrl_setup_portal_validate_advanced_form(&form, &state, error_text, sizeof(error_text)));
  ASSERT_CONTAINS(error_text, "Temperature step");

  form = (lm_ctrl_setup_portal_advanced_form_t){
    .preset_count = 4,
    .temperature_step_c = 0.5f,
    .time_step_s = 0.5f,
    .preset_reduce_confirmed = false,
  };
  ASSERT_FALSE(lm_ctrl_setup_portal_validate_advanced_form(&form, &state, error_text, sizeof(error_text)));
  ASSERT_CONTAINS(error_text, "Confirm the preset deletion");

  return 0;
}

static int test_validate_advanced_form_accepts_confirmed_reduction_and_regular_updates(void) {
  ctrl_state_t state;
  lm_ctrl_setup_portal_advanced_form_t form = {0};
  char error_text[128];

  ctrl_state_init(&state);
  state.preset_count = 4;

  form = (lm_ctrl_setup_portal_advanced_form_t){
    .preset_count = 6,
    .temperature_step_c = 0.5f,
    .time_step_s = 0.5f,
    .preset_reduce_confirmed = false,
  };
  ASSERT_TRUE(lm_ctrl_setup_portal_validate_advanced_form(&form, &state, error_text, sizeof(error_text)));
  ASSERT_STREQ("", error_text);

  state.preset_count = 6;
  form = (lm_ctrl_setup_portal_advanced_form_t){
    .preset_count = 4,
    .temperature_step_c = 0.5f,
    .time_step_s = 0.5f,
    .preset_reduce_confirmed = true,
  };
  ASSERT_TRUE(lm_ctrl_setup_portal_validate_advanced_form(&form, &state, error_text, sizeof(error_text)));
  ASSERT_STREQ("", error_text);

  return 0;
}

int run_setup_portal_route_tests(void) {
  RUN_TEST(test_parse_advanced_form_reads_supported_values);
  RUN_TEST(test_validate_advanced_form_rejects_invalid_values_and_missing_confirm);
  RUN_TEST(test_validate_advanced_form_accepts_confirmed_reduction_and_regular_updates);
  return 0;
}
