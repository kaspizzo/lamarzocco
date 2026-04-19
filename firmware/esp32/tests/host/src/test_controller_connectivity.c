#include "controller_connectivity.h"
#include "test_support.h"

static int test_compute_access_distinguishes_remote_path_states(void) {
  lm_ctrl_controller_access_t access = {0};
  lm_ctrl_wifi_info_t wifi_info = {0};
  lm_ctrl_machine_link_info_t machine_info = {0};

  lm_ctrl_controller_compute_access(&wifi_info, &machine_info, 0, &access);
  ASSERT_EQ_INT(LM_CTRL_REMOTE_PATH_HIDDEN, access.remote_path_state);
  ASSERT_EQ_U32(0, access.readable_mask);
  ASSERT_FALSE(access.preset_load_enabled);

  wifi_info.has_cloud_credentials = true;
  wifi_info.has_machine_selection = true;
  wifi_info.sta_connecting = true;
  lm_ctrl_controller_compute_access(&wifi_info, &machine_info, 0, &access);
  ASSERT_EQ_INT(LM_CTRL_REMOTE_PATH_CONNECTING, access.remote_path_state);

  wifi_info.sta_connecting = false;
  wifi_info.sta_connected = true;
  lm_ctrl_controller_compute_access(&wifi_info, &machine_info, 0, &access);
  ASSERT_EQ_INT(LM_CTRL_REMOTE_PATH_NETWORK, access.remote_path_state);

  wifi_info.has_machine_selection = true;
  wifi_info.cloud_connected = true;
  lm_ctrl_controller_compute_access(&wifi_info, &machine_info, 0, &access);
  ASSERT_EQ_INT(LM_CTRL_REMOTE_PATH_CLOUD, access.remote_path_state);

  wifi_info.machine_cloud_online = true;
  lm_ctrl_controller_compute_access(&wifi_info, &machine_info, 0, &access);
  ASSERT_EQ_INT(LM_CTRL_REMOTE_PATH_MACHINE_ONLINE, access.remote_path_state);

  return 0;
}

static int test_compute_access_gates_fields_by_transport(void) {
  lm_ctrl_controller_access_t access = {0};
  lm_ctrl_wifi_info_t wifi_info = {
    .has_cloud_credentials = true,
    .sta_connected = true,
    .cloud_connected = true,
    .has_machine_selection = true,
  };
  lm_ctrl_machine_link_info_t machine_info = {0};

  lm_ctrl_controller_compute_access(&wifi_info, &machine_info, CTRL_FEATURE_BBW, &access);
  ASSERT_EQ_U32(0, access.readable_mask);
  ASSERT_EQ_U32(0, access.editable_mask);
  ASSERT_FALSE(access.preset_load_enabled);

  machine_info.authenticated = true;
  lm_ctrl_controller_compute_access(&wifi_info, &machine_info, CTRL_FEATURE_BBW, &access);
  ASSERT_TRUE((access.readable_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0);
  ASSERT_TRUE((access.readable_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0);
  ASSERT_TRUE((access.readable_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0);
  ASSERT_TRUE((access.readable_mask & LM_CTRL_MACHINE_FIELD_PREBREWING) == 0);
  ASSERT_TRUE((access.readable_mask & LM_CTRL_MACHINE_FIELD_BBW) == 0);
  ASSERT_FALSE(access.preset_load_enabled);

  machine_info.authenticated = false;
  wifi_info.machine_cloud_online = true;
  lm_ctrl_controller_compute_access(&wifi_info, &machine_info, CTRL_FEATURE_BBW, &access);
  ASSERT_TRUE((access.readable_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0);
  ASSERT_TRUE((access.readable_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0);
  ASSERT_TRUE((access.readable_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0);
  ASSERT_TRUE((access.readable_mask & LM_CTRL_MACHINE_FIELD_PREBREWING) == LM_CTRL_MACHINE_FIELD_PREBREWING);
  ASSERT_TRUE((access.readable_mask & LM_CTRL_MACHINE_FIELD_BBW) == LM_CTRL_MACHINE_FIELD_BBW);
  ASSERT_TRUE(access.preset_load_enabled);

  return 0;
}

static int test_field_presentation_hides_stale_values_when_unreadable(void) {
  ASSERT_EQ_INT(
    LM_CTRL_FIELD_PRESENTATION_UNAVAILABLE,
    lm_ctrl_controller_field_presentation(
      0,
      LM_CTRL_MACHINE_FIELD_INFUSE,
      CTRL_FOCUS_INFUSE
    )
  );
  ASSERT_EQ_INT(
    LM_CTRL_FIELD_PRESENTATION_LOADING,
    lm_ctrl_controller_field_presentation(
      LM_CTRL_MACHINE_FIELD_TEMPERATURE,
      0,
      CTRL_FOCUS_TEMPERATURE
    )
  );
  ASSERT_EQ_INT(
    LM_CTRL_FIELD_PRESENTATION_READY,
    lm_ctrl_controller_field_presentation(
      LM_CTRL_MACHINE_FIELD_TEMPERATURE,
      LM_CTRL_MACHINE_FIELD_TEMPERATURE,
      CTRL_FOCUS_TEMPERATURE
    )
  );
  ASSERT_FALSE(
    lm_ctrl_controller_field_is_editable(
      LM_CTRL_MACHINE_FIELD_TEMPERATURE,
      CTRL_FOCUS_INFUSE
    )
  );
  ASSERT_TRUE(
    lm_ctrl_controller_field_is_editable(
      LM_CTRL_MACHINE_FIELD_PREBREWING,
      CTRL_FOCUS_INFUSE
    )
  );

  return 0;
}

static int test_remote_path_indicator_state_matches_ui_semantics(void) {
  ASSERT_EQ_INT(LM_CTRL_INDICATOR_HIDDEN, lm_ctrl_remote_path_indicator_state(LM_CTRL_REMOTE_PATH_HIDDEN));
  ASSERT_EQ_INT(LM_CTRL_INDICATOR_CROSSED, lm_ctrl_remote_path_indicator_state(LM_CTRL_REMOTE_PATH_CONNECTING));
  ASSERT_EQ_INT(LM_CTRL_INDICATOR_CROSSED, lm_ctrl_remote_path_indicator_state(LM_CTRL_REMOTE_PATH_NETWORK));
  ASSERT_EQ_INT(LM_CTRL_INDICATOR_CROSSED, lm_ctrl_remote_path_indicator_state(LM_CTRL_REMOTE_PATH_CLOUD));
  ASSERT_EQ_INT(LM_CTRL_INDICATOR_ACTIVE, lm_ctrl_remote_path_indicator_state(LM_CTRL_REMOTE_PATH_MACHINE_ONLINE));
  return 0;
}

int run_controller_connectivity_tests(void) {
  RUN_TEST(test_compute_access_distinguishes_remote_path_states);
  RUN_TEST(test_compute_access_gates_fields_by_transport);
  RUN_TEST(test_field_presentation_hides_stale_values_when_unreadable);
  RUN_TEST(test_remote_path_indicator_state_matches_ui_semantics);
  return 0;
}
