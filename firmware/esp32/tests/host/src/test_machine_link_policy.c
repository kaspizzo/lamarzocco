#include "machine_link_policy.h"
#include "test_support.h"

static int test_cloud_write_mask_covers_cloud_capable_machine_fields(void) {
  ASSERT_TRUE((LM_CTRL_MACHINE_FIELD_CLOUD_WRITE_MASK & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0);
  ASSERT_TRUE((LM_CTRL_MACHINE_FIELD_CLOUD_WRITE_MASK & LM_CTRL_MACHINE_FIELD_STEAM) != 0);
  ASSERT_TRUE((LM_CTRL_MACHINE_FIELD_CLOUD_WRITE_MASK & LM_CTRL_MACHINE_FIELD_STANDBY) != 0);
  ASSERT_TRUE((LM_CTRL_MACHINE_FIELD_CLOUD_WRITE_MASK & LM_CTRL_MACHINE_FIELD_PREBREWING) == LM_CTRL_MACHINE_FIELD_PREBREWING);
  ASSERT_TRUE((LM_CTRL_MACHINE_FIELD_CLOUD_WRITE_MASK & LM_CTRL_MACHINE_FIELD_BBW) == LM_CTRL_MACHINE_FIELD_BBW);
  return 0;
}

static int test_cloud_power_wakeup_preference_only_applies_to_known_remote_standby(void) {
  ctrl_values_t desired_values = {
    .standby_on = false,
  };
  ctrl_values_t remote_values = {
    .standby_on = true,
  };

  ASSERT_TRUE(
    lm_ctrl_machine_should_prefer_cloud_power_wakeup(
      &desired_values,
      &remote_values,
      LM_CTRL_MACHINE_FIELD_STANDBY,
      LM_CTRL_MACHINE_FIELD_STANDBY
    )
  );

  remote_values.standby_on = false;
  ASSERT_FALSE(
    lm_ctrl_machine_should_prefer_cloud_power_wakeup(
      &desired_values,
      &remote_values,
      LM_CTRL_MACHINE_FIELD_STANDBY,
      LM_CTRL_MACHINE_FIELD_STANDBY
    )
  );

  remote_values.standby_on = true;
  ASSERT_FALSE(
    lm_ctrl_machine_should_prefer_cloud_power_wakeup(
      &desired_values,
      &remote_values,
      LM_CTRL_MACHINE_FIELD_NONE,
      LM_CTRL_MACHINE_FIELD_STANDBY
    )
  );

  desired_values.standby_on = true;
  ASSERT_FALSE(
    lm_ctrl_machine_should_prefer_cloud_power_wakeup(
      &desired_values,
      &remote_values,
      LM_CTRL_MACHINE_FIELD_STANDBY,
      LM_CTRL_MACHINE_FIELD_STANDBY
    )
  );

  desired_values.standby_on = false;
  ASSERT_FALSE(
    lm_ctrl_machine_should_prefer_cloud_power_wakeup(
      &desired_values,
      &remote_values,
      LM_CTRL_MACHINE_FIELD_STANDBY,
      LM_CTRL_MACHINE_FIELD_TEMPERATURE
    )
  );

  return 0;
}

static int test_periodic_ble_sync_is_allowed_with_binding_before_auth(void) {
  ASSERT_TRUE(lm_ctrl_machine_should_request_periodic_ble_sync(true, false));
  ASSERT_TRUE(lm_ctrl_machine_should_request_periodic_ble_sync(false, true));
  ASSERT_TRUE(lm_ctrl_machine_should_request_periodic_ble_sync(true, true));
  ASSERT_FALSE(lm_ctrl_machine_should_request_periodic_ble_sync(false, false));
  return 0;
}

int run_machine_link_policy_tests(void) {
  RUN_TEST(test_cloud_write_mask_covers_cloud_capable_machine_fields);
  RUN_TEST(test_cloud_power_wakeup_preference_only_applies_to_known_remote_standby);
  RUN_TEST(test_periodic_ble_sync_is_allowed_with_binding_before_auth);
  return 0;
}
