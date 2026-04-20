#include "controller_shot_timer.h"
#include "test_support.h"

static int test_live_shot_becomes_visible_and_updates_seconds(void) {
  lm_ctrl_shot_timer_state_t state;
  const lm_ctrl_shot_timer_info_t info = {
    .websocket_connected = true,
    .brew_active = true,
    .available = true,
    .seconds = 27,
  };

  lm_ctrl_shot_timer_reset(&state);
  ASSERT_TRUE(lm_ctrl_shot_timer_update(&state, &info));
  ASSERT_TRUE(lm_ctrl_shot_timer_visible(&state));
  ASSERT_FALSE(lm_ctrl_shot_timer_dismissable(&state));
  ASSERT_EQ_INT(LM_CTRL_SHOT_TIMER_LIVE, state.mode);
  ASSERT_EQ_U32(27U, state.seconds);
  return 0;
}

static int test_live_shot_freezes_into_sticky_when_brew_stops(void) {
  lm_ctrl_shot_timer_state_t state;
  const lm_ctrl_shot_timer_info_t live_info = {
    .websocket_connected = true,
    .brew_active = true,
    .available = true,
    .seconds = 32,
  };
  const lm_ctrl_shot_timer_info_t idle_info = {
    .websocket_connected = true,
    .brew_active = false,
    .available = false,
    .seconds = 0,
  };

  lm_ctrl_shot_timer_reset(&state);
  ASSERT_TRUE(lm_ctrl_shot_timer_update(&state, &live_info));
  ASSERT_TRUE(lm_ctrl_shot_timer_update(&state, &idle_info));
  ASSERT_TRUE(lm_ctrl_shot_timer_visible(&state));
  ASSERT_TRUE(lm_ctrl_shot_timer_dismissable(&state));
  ASSERT_EQ_INT(LM_CTRL_SHOT_TIMER_STICKY, state.mode);
  ASSERT_EQ_U32(32U, state.seconds);
  return 0;
}

static int test_sticky_shot_can_be_dismissed(void) {
  lm_ctrl_shot_timer_state_t state = {
    .mode = LM_CTRL_SHOT_TIMER_STICKY,
    .seconds = 18,
  };

  ASSERT_TRUE(lm_ctrl_shot_timer_dismiss(&state));
  ASSERT_FALSE(lm_ctrl_shot_timer_visible(&state));
  ASSERT_FALSE(lm_ctrl_shot_timer_dismissable(&state));
  ASSERT_EQ_INT(LM_CTRL_SHOT_TIMER_HIDDEN, state.mode);
  ASSERT_EQ_U32(0U, state.seconds);
  return 0;
}

static int test_live_shot_cannot_be_dismissed(void) {
  lm_ctrl_shot_timer_state_t state = {
    .mode = LM_CTRL_SHOT_TIMER_LIVE,
    .seconds = 14,
  };

  ASSERT_FALSE(lm_ctrl_shot_timer_dismiss(&state));
  ASSERT_TRUE(lm_ctrl_shot_timer_visible(&state));
  ASSERT_FALSE(lm_ctrl_shot_timer_dismissable(&state));
  ASSERT_EQ_INT(LM_CTRL_SHOT_TIMER_LIVE, state.mode);
  ASSERT_EQ_U32(14U, state.seconds);
  return 0;
}

static int test_new_live_shot_replaces_sticky_timer_immediately(void) {
  lm_ctrl_shot_timer_state_t state = {
    .mode = LM_CTRL_SHOT_TIMER_STICKY,
    .seconds = 29,
  };
  const lm_ctrl_shot_timer_info_t live_info = {
    .websocket_connected = true,
    .brew_active = true,
    .available = true,
    .seconds = 3,
  };

  ASSERT_TRUE(lm_ctrl_shot_timer_update(&state, &live_info));
  ASSERT_TRUE(lm_ctrl_shot_timer_visible(&state));
  ASSERT_FALSE(lm_ctrl_shot_timer_dismissable(&state));
  ASSERT_EQ_INT(LM_CTRL_SHOT_TIMER_LIVE, state.mode);
  ASSERT_EQ_U32(3U, state.seconds);
  return 0;
}

int run_controller_shot_timer_tests(void) {
  RUN_TEST(test_live_shot_becomes_visible_and_updates_seconds);
  RUN_TEST(test_live_shot_freezes_into_sticky_when_brew_stops);
  RUN_TEST(test_sticky_shot_can_be_dismissed);
  RUN_TEST(test_live_shot_cannot_be_dismissed);
  RUN_TEST(test_new_live_shot_replaces_sticky_timer_immediately);
  return 0;
}
