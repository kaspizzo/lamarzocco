#include "wifi_reconnect_policy.h"
#include "test_support.h"

static int test_first_disconnect_uses_base_delay_without_portal_fallback(void) {
  lm_ctrl_wifi_reconnect_plan_t plan = lm_ctrl_wifi_reconnect_plan_next(0, false);

  ASSERT_EQ_INT(1, (int)plan.disconnect_count);
  ASSERT_EQ_U32(LM_CTRL_WIFI_RECONNECT_BASE_DELAY_MS, plan.retry_delay_ms);
  ASSERT_FALSE(plan.should_enable_setup_ap);
  return 0;
}

static int test_retry_delay_grows_exponentially_until_portal_fallback(void) {
  lm_ctrl_wifi_reconnect_plan_t second = lm_ctrl_wifi_reconnect_plan_next(1, false);
  lm_ctrl_wifi_reconnect_plan_t third = lm_ctrl_wifi_reconnect_plan_next(2, false);
  lm_ctrl_wifi_reconnect_plan_t fourth = lm_ctrl_wifi_reconnect_plan_next(3, false);

  ASSERT_EQ_INT(2, (int)second.disconnect_count);
  ASSERT_EQ_U32(2000U, second.retry_delay_ms);
  ASSERT_FALSE(second.should_enable_setup_ap);

  ASSERT_EQ_INT(3, (int)third.disconnect_count);
  ASSERT_EQ_U32(4000U, third.retry_delay_ms);
  ASSERT_FALSE(third.should_enable_setup_ap);

  ASSERT_EQ_INT(4, (int)fourth.disconnect_count);
  ASSERT_EQ_U32(8000U, fourth.retry_delay_ms);
  ASSERT_TRUE(fourth.should_enable_setup_ap);
  return 0;
}

static int test_existing_portal_suppresses_extra_fallback_request(void) {
  lm_ctrl_wifi_reconnect_plan_t plan = lm_ctrl_wifi_reconnect_plan_next(6, true);

  ASSERT_EQ_INT(7, (int)plan.disconnect_count);
  ASSERT_FALSE(plan.should_enable_setup_ap);
  return 0;
}

static int test_retry_delay_and_disconnect_count_saturate(void) {
  lm_ctrl_wifi_reconnect_plan_t plan = lm_ctrl_wifi_reconnect_plan_next(20, false);

  ASSERT_EQ_INT(LM_CTRL_WIFI_RECONNECT_MAX_DISCONNECT_COUNT, (int)plan.disconnect_count);
  ASSERT_EQ_U32(LM_CTRL_WIFI_RECONNECT_MAX_DELAY_MS, plan.retry_delay_ms);
  ASSERT_TRUE(plan.should_enable_setup_ap);
  return 0;
}

int run_wifi_reconnect_policy_tests(void) {
  RUN_TEST(test_first_disconnect_uses_base_delay_without_portal_fallback);
  RUN_TEST(test_retry_delay_grows_exponentially_until_portal_fallback);
  RUN_TEST(test_existing_portal_suppresses_extra_fallback_request);
  RUN_TEST(test_retry_delay_and_disconnect_count_saturate);
  return 0;
}
