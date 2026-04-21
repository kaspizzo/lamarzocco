#include "controller_heat_session.h"
#include "test_support.h"

static int test_heat_session_starts_once_with_initial_eta(void) {
  lm_ctrl_heat_session_t session = {0};

  ASSERT_TRUE(lm_ctrl_heat_session_apply(&session, true, true, 5000000LL, 2000LL, 1000LL, 7000LL));
  ASSERT_TRUE(session.heating);
  ASSERT_EQ_I64(5000000LL, session.session_start_local_us);
  ASSERT_EQ_I64(11000000LL, session.deadline_local_us);
  ASSERT_EQ_I64(7000LL, session.source_ready_epoch_ms);
  ASSERT_EQ_I64(6000000LL, lm_ctrl_heat_session_remaining_us(&session, 5000000LL));
  ASSERT_EQ_INT(1000, (int)lm_ctrl_heat_session_progress_permille(&session, 5000000LL));
  return 0;
}

static int test_heat_session_keeps_anchor_when_eta_shortens(void) {
  lm_ctrl_heat_session_t session = {0};

  ASSERT_TRUE(lm_ctrl_heat_session_apply(&session, true, true, 5000000LL, 2000LL, 1000LL, 7000LL));
  ASSERT_TRUE(lm_ctrl_heat_session_apply(&session, true, true, 7000000LL, 3000LL, 3000LL, 6500LL));
  ASSERT_EQ_I64(5000000LL, session.session_start_local_us);
  ASSERT_EQ_I64(10500000LL, session.deadline_local_us);
  ASSERT_EQ_I64(6500LL, session.source_ready_epoch_ms);
  ASSERT_EQ_INT(636, (int)lm_ctrl_heat_session_progress_permille(&session, 7000000LL));
  return 0;
}

static int test_heat_session_keeps_anchor_when_eta_extends(void) {
  lm_ctrl_heat_session_t session = {0};

  ASSERT_TRUE(lm_ctrl_heat_session_apply(&session, true, true, 5000000LL, 2000LL, 1000LL, 7000LL));
  ASSERT_TRUE(lm_ctrl_heat_session_apply(&session, true, true, 7000000LL, 3000LL, 3000LL, 12000LL));
  ASSERT_EQ_I64(5000000LL, session.session_start_local_us);
  ASSERT_EQ_I64(16000000LL, session.deadline_local_us);
  ASSERT_EQ_INT(818, (int)lm_ctrl_heat_session_progress_permille(&session, 7000000LL));
  return 0;
}

static int test_heat_session_preserves_eta_when_heating_continues_without_eta(void) {
  lm_ctrl_heat_session_t session = {0};

  ASSERT_TRUE(lm_ctrl_heat_session_apply(&session, true, true, 5000000LL, 2000LL, 1000LL, 7000LL));
  ASSERT_TRUE(lm_ctrl_heat_session_apply(&session, true, false, 7000000LL, 3000LL, 0LL, 0LL));
  ASSERT_EQ_I64(5000000LL, session.session_start_local_us);
  ASSERT_EQ_I64(11000000LL, session.deadline_local_us);
  ASSERT_EQ_INT(666, (int)lm_ctrl_heat_session_progress_permille(&session, 7000000LL));
  return 0;
}

static int test_heat_session_resets_when_heating_stops(void) {
  lm_ctrl_heat_session_t session = {0};

  ASSERT_TRUE(lm_ctrl_heat_session_apply(&session, true, true, 5000000LL, 2000LL, 1000LL, 7000LL));
  ASSERT_FALSE(lm_ctrl_heat_session_apply(&session, false, false, 7000000LL, 3000LL, 0LL, 0LL));
  ASSERT_FALSE(session.heating);
  ASSERT_EQ_I64(0LL, session.session_start_local_us);
  ASSERT_EQ_I64(0LL, session.deadline_local_us);
  ASSERT_EQ_I64(0LL, session.source_ready_epoch_ms);
  return 0;
}

static int test_heat_session_shifts_eta_without_wall_clock_from_previous_ready_time(void) {
  lm_ctrl_heat_session_t session = {0};

  ASSERT_TRUE(lm_ctrl_heat_session_apply(&session, true, true, 5000000LL, 2000LL, 1000LL, 7000LL));
  ASSERT_TRUE(lm_ctrl_heat_session_apply(&session, true, true, 7000000LL, 0LL, 0LL, 6000LL));
  ASSERT_EQ_I64(5000000LL, session.session_start_local_us);
  ASSERT_EQ_I64(10000000LL, session.deadline_local_us);
  ASSERT_EQ_INT(600, (int)lm_ctrl_heat_session_progress_permille(&session, 7000000LL));
  return 0;
}

int run_controller_heat_session_tests(void) {
  RUN_TEST(test_heat_session_starts_once_with_initial_eta);
  RUN_TEST(test_heat_session_keeps_anchor_when_eta_shortens);
  RUN_TEST(test_heat_session_keeps_anchor_when_eta_extends);
  RUN_TEST(test_heat_session_preserves_eta_when_heating_continues_without_eta);
  RUN_TEST(test_heat_session_resets_when_heating_stops);
  RUN_TEST(test_heat_session_shifts_eta_without_wall_clock_from_previous_ready_time);
  return 0;
}
