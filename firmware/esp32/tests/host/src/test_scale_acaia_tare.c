#include "scale_adapter_core.h"
#include "test_support.h"
#include <math.h>
#include <string.h>

typedef union { uint64_t alignment; uint8_t bytes[512]; } tare_state_t;
typedef struct {
  const lm_scale_driver_t *driver;
  tare_state_t source;
  lm_scale_adapter_core_t core;
  uint64_t now;
  unsigned observed, wire;
} tare_test_t;
static bool receive(void *ctx, const lm_scale_event_t *e) {
  tare_test_t *t = ctx;
  if (e->kind == LM_SCALE_ACK) {
    if (e->ack_wire_valid) ++t->wire;
    else ++t->observed;
  }
  return lm_scale_core_event(&t->core, e, t->now);
}
static bool feed(tare_test_t *t, lm_scale_frame_t f, uint64_t now) {
  t->now = now;
  return t->driver->feed(t->source.bytes, f.data, f.size, receive, t) &&
         t->driver->observe_control(t->source.bytes, now, receive, t);
}
static lm_scale_frame_t measurement(double grams) {
  unsigned raw = (unsigned)llround(fabs(grams) * 100);
  return lm_scale_packet(12, (const uint8_t[]){8,5,raw & 255,(raw >> 8) & 255,0,0,2,grams < 0 ? 2 : 0}, 8);
}
static bool command(tare_test_t *t, uint8_t opcode, const uint8_t *payload, size_t n, uint64_t now) {
  t->now = now;
  lm_scale_frame_t f = lm_scale_packet(opcode, payload, n);
  return lm_scale_core_command(&t->core, f.data, f.size, now);
}
static bool send(void *ctx, lm_scale_control_t control) {
  tare_test_t *t = ctx;
  return t->driver->control(control).size != 0 &&
         t->driver->control_written(t->source.bytes, control, t->now);
}
static bool tick(tare_test_t *t, uint64_t now) {
  t->now = now;
  return lm_scale_core_tick(&t->core, now, send, t);
}
static bool wire_ack(tare_test_t *t, uint8_t code, uint64_t now) {
  return feed(t, lm_scale_packet(12, (const uint8_t[]){4,11,0,code}, 4), now);
}
static bool init(tare_test_t *t, const char *id) {
  memset(t, 0, sizeof(*t));
  t->driver = lm_scale_driver_find(id);
  if (!t->driver || !t->driver->control_written || !t->driver->observe_control) return false;
  lm_scale_core_init(&t->core);
  t->driver->reset(t->source.bytes);
  if (!feed(t, lm_scale_packet(8, (const uint8_t[]){9,100,2,16,1,0,1,1,0}, 9), 100) ||
      !feed(t, measurement(25.0), 100)) return false;
  const uint8_t identity[] = "---------------";
  if (!command(t, 11, identity, sizeof(identity) - 1, 100)) return false;
  lm_scale_core_delivered(&t->core);
  return true;
}
static bool tare(tare_test_t *t, uint64_t now) {
  return command(t, 4, (const uint8_t[]){0}, 1, now) && tick(t, now);
}
static bool timer(tare_test_t *t, uint8_t action, uint64_t now) {
  return command(t, 38, (const uint8_t[]){0,0,action}, 3, now) && tick(t, now);
}
static int test_acaia_repeated_brew_start_tare_without_wire_ack(void) {
  const char *ids[] = {"acaia_legacy", "acaia_2021"};
  for (size_t i = 0; i < 2; ++i) {
    tare_test_t t;
    ASSERT_TRUE(init(&t, ids[i]));
    ASSERT_TRUE(timer(&t, 0, 200));
    ASSERT_TRUE(wire_ack(&t, 0x41, 210));
    ASSERT_TRUE(tick(&t, 210));
    ASSERT_TRUE(wire_ack(&t, 0x01, 220));
    ASSERT_TRUE(tick(&t, 220));
    lm_scale_core_delivered(&t.core);
    ASSERT_TRUE(tare(&t, 230));
    ASSERT_TRUE(wire_ack(&t, 0x80, 240));
    ASSERT_TRUE(feed(&t, measurement(.22), 250)); /* Explicit ACK preserves inflow handling. */
    ASSERT_TRUE(tick(&t, 250));
    ASSERT_EQ_INT(1, t.core.reply_count);
    ASSERT_EQ_INT(0, t.observed);
    lm_scale_core_delivered(&t.core);
    ASSERT_TRUE(timer(&t, 1, 400));
    ASSERT_TRUE(wire_ack(&t, 0x21, 410));
    ASSERT_TRUE(tick(&t, 410));
    lm_scale_core_delivered(&t.core);
    ASSERT_TRUE(timer(&t, 0, 500));
    ASSERT_TRUE(wire_ack(&t, 0x41, 510));
    ASSERT_TRUE(tick(&t, 510));
    ASSERT_TRUE(wire_ack(&t, 0x01, 520));
    ASSERT_TRUE(tick(&t, 520));
    lm_scale_core_delivered(&t.core);
    ASSERT_TRUE(tare(&t, 530));
    /* Model the reported second start: first post-write sample after 248 ms,
     * physical zero, no dedicated TARE ACK. Values are synthetic, not logged. */
    ASSERT_TRUE(feed(&t, measurement(0), 778));
    ASSERT_FALSE(t.core.controls[0].acknowledged);
    ASSERT_TRUE(feed(&t, measurement(0), 878));
    ASSERT_EQ_INT(1, t.observed);
    ASSERT_TRUE(t.core.controls[0].acknowledged);
    ASSERT_FALSE(t.core.last_ack_wire_valid);
    ASSERT_TRUE(tick(&t, 878));
    ASSERT_EQ_INT(1, t.core.reply_count);
    lm_scale_core_delivered(&t.core);
    ASSERT_TRUE(feed(&t, measurement(1.42), 928));
    ASSERT_EQ_INT(1, t.observed); /* No repeated confirmation or virtual zeroing. */
    ASSERT_FLOAT_EQ(1.42, t.core.grams, .00001);
    ASSERT_TRUE(tick(&t, 928));
  }
  return 0;
}
static int test_acaia_observed_tare_needs_new_separate_zero_samples(void) {
  const char *ids[] = {"acaia_legacy", "acaia_2021"};
  for (size_t i = 0; i < 2; ++i) {
    tare_test_t t;
    ASSERT_TRUE(init(&t, ids[i]));
    ASSERT_TRUE(feed(&t, measurement(0), 150));
    ASSERT_TRUE(tare(&t, 200));
    ASSERT_TRUE(t.driver->observe_control(t.source.bytes, 210, receive, &t));
    ASSERT_EQ_INT(0, t.observed); /* Cached zero is insufficient. */
    lm_scale_frame_t f = measurement(0);
    uint8_t combined[2 * LM_SCALE_FRAME_MAX];
    memcpy(combined, f.data, f.size);
    memcpy(combined + f.size, f.data, f.size);
    t.now = 220;
    ASSERT_TRUE(t.driver->feed(t.source.bytes, combined, 2 * f.size, receive, &t));
    ASSERT_TRUE(t.driver->observe_control(t.source.bytes, 220, receive, &t));
    ASSERT_EQ_INT(0, t.observed); /* Multiple frames in one notification are not independent observations. */
    ASSERT_TRUE(t.driver->observe_control(t.source.bytes, 230, receive, &t));
    ASSERT_EQ_INT(0, t.observed); /* Time alone is insufficient. */
    ASSERT_TRUE(feed(&t, measurement(0), 240));
    ASSERT_EQ_INT(0, t.observed); /* Less than 50 ms since the first zero. */
    ASSERT_TRUE(feed(&t, measurement(.06), 260));
    ASSERT_TRUE(feed(&t, measurement(-.05), 280));
    ASSERT_EQ_INT(0, t.observed); /* Nonzero reading reset the observation window. */
    ASSERT_TRUE(feed(&t, measurement(.05), 340));
    ASSERT_EQ_INT(1, t.observed);
    ASSERT_TRUE(tick(&t, 340));
  }
  return 0;
}
static int test_acaia_fresh_tare_button_and_zero_confirm_but_buttons_alone_do_not(void) {
  const char *ids[] = {"acaia_legacy", "acaia_2021"};
  for (size_t i = 0; i < 2; ++i) {
    tare_test_t t;
    ASSERT_TRUE(init(&t, ids[i]));
    ASSERT_TRUE(feed(&t, lm_scale_packet(12, (const uint8_t[]){3,8,0}, 3), 150));
    ASSERT_TRUE(tare(&t, 200));
    ASSERT_TRUE(feed(&t, measurement(0), 220));
    ASSERT_EQ_INT(0, t.observed); /* Pre-write key cannot be reused. */
    ASSERT_TRUE(feed(&t, measurement(.22), 230));
    ASSERT_TRUE(feed(&t, lm_scale_packet(12, (const uint8_t[]){3,8,0}, 3), 240));
    ASSERT_EQ_INT(0, t.observed);
    ASSERT_TRUE(feed(&t, measurement(.22), 250));
    ASSERT_EQ_INT(0, t.observed); /* A key plus nonzero weight is insufficient. */
    ASSERT_TRUE(feed(&t, measurement(0), 260));
    ASSERT_EQ_INT(1, t.observed);
    ASSERT_TRUE(tick(&t, 260));
    ASSERT_TRUE(init(&t, ids[i]));
    ASSERT_TRUE(tare(&t, 200));
    ASSERT_TRUE(feed(&t, lm_scale_packet(12, (const uint8_t[]){10,8,8,5,0,0,0,0,2,0}, 10), 220));
    ASSERT_EQ_INT(0, t.observed); /* Timer start key does not confirm tare. */
    ASSERT_TRUE(feed(&t, lm_scale_packet(12, (const uint8_t[]){10,8,0,5,0,0,0,0,2,0}, 10), 240));
    ASSERT_EQ_INT(1, t.observed);
  }
  return 0;
}
static int test_acaia_inflight_frame_late_zero_and_timer_controls_cannot_bypass_guards(void) {
  const char *ids[] = {"acaia_legacy", "acaia_2021"};
  for (size_t i = 0; i < 2; ++i) {
    tare_test_t t;
    ASSERT_TRUE(init(&t, ids[i]));
    lm_scale_frame_t f = measurement(0);
    t.now = 150;
    ASSERT_TRUE(t.driver->feed(t.source.bytes, f.data, 6, receive, &t));
    ASSERT_TRUE(tare(&t, 200));
    t.now = 220;
    ASSERT_TRUE(t.driver->feed(t.source.bytes, f.data + 6, f.size - 6, receive, &t));
    ASSERT_TRUE(t.driver->observe_control(t.source.bytes, 220, receive, &t));
    ASSERT_TRUE(feed(&t, measurement(0), 300));
    ASSERT_EQ_INT(0, t.observed); /* Finishing a pre-write frame was not a qualifying zero. */
    ASSERT_TRUE(feed(&t, measurement(0), 360));
    ASSERT_EQ_INT(1, t.observed);
    ASSERT_TRUE(init(&t, ids[i]));
    ASSERT_TRUE(tare(&t, 200));
    ASSERT_TRUE(feed(&t, measurement(0), 601));
    ASSERT_TRUE(feed(&t, measurement(0), 660));
    ASSERT_FALSE(tick(&t, 660)); /* Evidence after 450 ms still fails the core deadline. */
    ASSERT_TRUE(init(&t, ids[i]));
    ASSERT_TRUE(timer(&t, 1, 200));
    ASSERT_TRUE(feed(&t, measurement(0), 300));
    ASSERT_TRUE(feed(&t, measurement(0), 360));
    ASSERT_EQ_INT(0, t.observed);
    ASSERT_FALSE(tick(&t, 651)); /* Timer STOP still requires its own wire ACK. */
  }
  return 0;
}
static int test_acaia_observed_tare_still_rejects_corruption_and_session_reuse(void) {
  const char *ids[] = {"acaia_legacy", "acaia_2021"};
  for (size_t i = 0; i < 2; ++i) {
    tare_test_t t;
    ASSERT_TRUE(init(&t, ids[i]));
    ASSERT_TRUE(tare(&t, 200));
    lm_scale_frame_t f = lm_scale_packet(12, (const uint8_t[]){10,8,0,5,0,0,0,0,2,0}, 10);
    f.data[f.size - 1] ^= 1;
    ASSERT_FALSE(feed(&t, f, 220));
    ASSERT_EQ_INT(0, t.observed);
    t.driver->reset(t.source.bytes);
    ASSERT_FALSE(t.driver->control_written(t.source.bytes, LM_SCALE_TARE, 300));
    ASSERT_TRUE(feed(&t, measurement(0), 310));
    ASSERT_TRUE(feed(&t, measurement(0), 370));
    ASSERT_EQ_INT(0, t.observed); /* Pending control and key evidence were cleared at reconnect. */
    ASSERT_TRUE(init(&t, ids[i]));
    ASSERT_TRUE(tare(&t, 200));
    ASSERT_FALSE(t.driver->control_written(t.source.bytes, LM_SCALE_TIMER_START, 210));
    ASSERT_TRUE(feed(&t, lm_scale_packet(12, (const uint8_t[]){4,11,0,0xe0}, 4), 220));
    ASSERT_EQ_INT(0, t.observed);
    ASSERT_FALSE(t.core.controls[0].acknowledged);
  }
  return 0;
}
int run_scale_acaia_tare_tests(void) {
  RUN_TEST(test_acaia_repeated_brew_start_tare_without_wire_ack);
  RUN_TEST(test_acaia_observed_tare_needs_new_separate_zero_samples);
  RUN_TEST(test_acaia_fresh_tare_button_and_zero_confirm_but_buttons_alone_do_not);
  RUN_TEST(test_acaia_inflight_frame_late_zero_and_timer_controls_cannot_bypass_guards);
  RUN_TEST(test_acaia_observed_tare_still_rejects_corruption_and_session_reuse);
  return 0;
}
