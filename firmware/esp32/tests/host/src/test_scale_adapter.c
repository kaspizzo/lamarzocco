#include "scale_adapter_core.h"
#include "scale_registration_policy.h"
#include "scale_link_lifecycle.h"
#include "test_support.h"
#include <math.h>
#include <string.h>

static const uint8_t pair_reply[] = {0xef,0xdd,8,9,0x13,2,0x10,1,0,1,1,0,0x0d,0x24};
static const uint8_t weight[] = {0xef,0xdd,12,8,5,0,0,0,0,2,0,0x0a,5};
static lm_scale_event_t events[24];
static lm_scale_frame_t event_frames[24];
static size_t event_count;
static lm_scale_control_t sent[24];
static size_t sent_count;
static bool collect(void *ctx, const lm_scale_event_t *e) {
  (void)ctx;
  if (event_count == 24) return false;
  events[event_count] = *e;
  if (e->frame) {
    event_frames[event_count] = *e->frame;
    events[event_count].frame = &event_frames[event_count];
  }
  ++event_count;
  return true;
}
static bool send(void *ctx, lm_scale_control_t c) {
  (void)ctx;
  if (sent_count == 24) return false;
  sent[sent_count++] = c;
  return true;
}
static bool command(lm_scale_adapter_core_t *s, uint8_t opcode, const uint8_t *p, size_t n, uint64_t now) {
  lm_scale_frame_t f = lm_scale_packet(opcode, p, n);
  return lm_scale_core_command(s, f.data, f.size, now);
}
static void make_ready(lm_scale_adapter_core_t *s) {
  lm_scale_core_init(s);
  lm_scale_event_t e = {.kind = LM_SCALE_READY, .battery = 19};
  lm_scale_core_event(s, &e, 100);
  e.kind = LM_SCALE_WEIGHT;
  lm_scale_core_event(s, &e, 100);
  const uint8_t p[15] = {45,45,45,45,45,45,45,45,45,45,45,45,45,45,45};
  command(s, 11, p, sizeof(p), 100);
  lm_scale_core_delivered(s);
  sent_count = 0;
}
static bool control(lm_scale_adapter_core_t *s, uint8_t action, uint64_t now) {
  return command(s, 38, (const uint8_t[]){0,0,action}, 3, now);
}
static void acknowledge(lm_scale_adapter_core_t *s, lm_scale_control_t c, uint64_t now) {
  const lm_scale_event_t e = {.kind = LM_SCALE_ACK, .control = c};
  lm_scale_core_event(s, &e, now);
}
static int test_recorded_source_fragmentation_and_driver_contract(void) {
  const lm_scale_driver_t *driver = lm_scale_driver_find("acaia_legacy");
  ASSERT_TRUE(driver != NULL);
  ASSERT_TRUE(lm_scale_driver_find("unknown") == NULL);
  uint8_t trace[sizeof(pair_reply) + sizeof(weight)];
  memcpy(trace, pair_reply, sizeof(pair_reply));
  memcpy(trace + sizeof(pair_reply), weight, sizeof(weight));
  for (size_t stride = 1; stride <= sizeof(trace); ++stride) {
    union { uint64_t alignment; uint8_t data[512]; } state;
    driver->reset(state.data);
    event_count = 0;
    for (size_t pos = 0; pos < sizeof(trace); pos += stride) {
      size_t n = sizeof(trace) - pos < stride ? sizeof(trace) - pos : stride;
      ASSERT_TRUE(driver->feed(state.data, trace + pos, n, collect, NULL));
    }
    ASSERT_EQ_INT(2, event_count);
    ASSERT_EQ_INT(LM_SCALE_READY, events[0].kind);
    ASSERT_EQ_INT(19, events[0].battery);
    ASSERT_EQ_INT(LM_SCALE_WEIGHT, events[1].kind);
    ASSERT_TRUE(fabs(events[1].grams) < .001);
  }
  lm_scale_frame_t start = driver->control(LM_SCALE_TIMER_START);
  lm_scale_frame_t stop = driver->control(LM_SCALE_TIMER_STOP);
  lm_scale_frame_t reset = driver->control(LM_SCALE_TIMER_RESET);
  ASSERT_EQ_INT(13, start.data[2]);
  ASSERT_EQ_INT(0, start.data[4]);
  ASSERT_EQ_INT(2, stop.data[4]);
  ASSERT_EQ_INT(1, reset.data[4]);
  return 0;
}
static int test_compound_source_ack_weight_buttons_and_malformed_tail(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("acaia_legacy");
  union { uint64_t alignment; uint8_t data[512]; } state;
  d->reset(state.data);
  const uint8_t p[] = {11,11,0,0x80,5,22,0,0,0,2,0};
  lm_scale_frame_t frame = lm_scale_packet(12, p, sizeof(p));
  event_count = 0;
  ASSERT_TRUE(d->feed(state.data, frame.data, frame.size, collect, NULL));
  ASSERT_EQ_INT(2, event_count);
  ASSERT_EQ_INT(LM_SCALE_TARE, events[0].control);
  ASSERT_TRUE(fabs(events[1].grams - .22) < .0001);
  frame = lm_scale_packet(12, (const uint8_t[]){10,8,3,5,0,0,0,0,2,0}, 10);
  ASSERT_TRUE(d->feed(state.data, frame.data, frame.size, collect, NULL));
  ASSERT_EQ_INT(3, event_count);
  frame = lm_scale_packet(12, (const uint8_t[]){10,5,0,0,0,0,2,0,7,0}, 10);
  ASSERT_FALSE(d->feed(state.data, frame.data, frame.size, collect, NULL));
  ASSERT_EQ_INT(3, event_count); /* No partial weight emitted from a malformed frame. */
  return 0;
}
static int test_source_checksum_and_gram_unit_are_required(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("acaia_legacy");
  union { uint64_t alignment; uint8_t data[512]; } state;
  d->reset(state.data);
  uint8_t corrupt[sizeof(weight)];
  memcpy(corrupt, weight, sizeof(weight));
  corrupt[5] ^= 1;
  event_count = 0;
  ASSERT_FALSE(d->feed(state.data, corrupt, sizeof(corrupt), collect, NULL));
  ASSERT_EQ_INT(0, event_count);
  d->reset(state.data);
  lm_scale_frame_t f = lm_scale_packet(8, (const uint8_t[]){9,19,1,16,1,0,1,1,0}, 9);
  ASSERT_FALSE(d->feed(state.data, f.data, f.size, collect, NULL));
  return 0;
}
static int test_gateway_handshake_and_settings_ack(void) {
  lm_scale_adapter_core_t s;
  make_ready(&s);
  ASSERT_TRUE(command(&s, 38, (const uint8_t[]){2,0,1}, 3, 100));
  ASSERT_TRUE(command(&s, 38, (const uint8_t[]){4,0,0}, 3, 100));
  ASSERT_TRUE(command(&s, 38, (const uint8_t[]){7,3,50}, 3, 100));
  lm_scale_frame_t f;
  ASSERT_TRUE(lm_scale_core_peek(&s, 100, &f));
  ASSERT_EQ_INT(0x40, f.data[6]);
  lm_scale_core_delivered(&s);
  ASSERT_TRUE(lm_scale_core_peek(&s, 100, &f));
  ASSERT_EQ_INT(0x80, f.data[6]);
  lm_scale_core_delivered(&s);
  ASSERT_TRUE(lm_scale_core_peek(&s, 100, &f));
  ASSERT_EQ_INT(0xe0, f.data[6]);
  ASSERT_EQ_INT(0, sent_count); /* Virtual setup does not change the source mode. */
  return 0;
}
static int test_start_resets_once_and_coalesces_until_ack_delivery(void) {
  lm_scale_adapter_core_t s;
  make_ready(&s);
  ASSERT_TRUE(control(&s, 0, 100));
  ASSERT_TRUE(lm_scale_core_tick(&s, 100, send, NULL));
  ASSERT_EQ_INT(LM_SCALE_TIMER_RESET, sent[0]);
  ASSERT_TRUE(control(&s, 0, 101));
  acknowledge(&s, LM_SCALE_TIMER_RESET, 110);
  ASSERT_TRUE(lm_scale_core_tick(&s, 110, send, NULL));
  ASSERT_EQ_INT(2, sent_count);
  ASSERT_EQ_INT(LM_SCALE_TIMER_START, sent[1]);
  ASSERT_EQ_INT(0, s.reply_count);
  acknowledge(&s, LM_SCALE_TIMER_START, 120);
  ASSERT_TRUE(lm_scale_core_tick(&s, 120, send, NULL));
  ASSERT_EQ_INT(1, s.reply_count);
  ASSERT_TRUE(control(&s, 0, 130));
  ASSERT_EQ_INT(1, s.control_count);
  lm_scale_core_delivered(&s);
  ASSERT_TRUE(control(&s, 0, 140));
  ASSERT_TRUE(lm_scale_core_tick(&s, 140, send, NULL));
  ASSERT_EQ_INT(3, sent_count);
  ASSERT_EQ_INT(LM_SCALE_TIMER_RESET, sent[2]);
  return 0;
}
static int test_start_stop_start_order_and_control_limit(void) {
  lm_scale_adapter_core_t s;
  make_ready(&s);
  ASSERT_TRUE(control(&s, 0, 100));
  ASSERT_TRUE(control(&s, 1, 100));
  ASSERT_TRUE(control(&s, 0, 100));
  ASSERT_EQ_INT(3, s.control_count);
  ASSERT_EQ_INT(LM_SCALE_TIMER_STOP, s.controls[1].command);
  ASSERT_TRUE(control(&s, 0, 100));
  ASSERT_EQ_INT(3, s.control_count);
  ASSERT_TRUE(control(&s, 1, 100));
  ASSERT_FALSE(control(&s, 0, 100));
  return 0;
}
static int test_tare_requires_ack_and_new_measurement_preserves_flowing_water(void) {
  lm_scale_adapter_core_t s;
  make_ready(&s);
  ASSERT_TRUE(command(&s, 12, (const uint8_t[]){9,0,1,1,2,2,5,3,4}, 9, 100));
  ASSERT_TRUE(command(&s, 4, (const uint8_t[]){0}, 1, 100));
  ASSERT_TRUE(lm_scale_core_tick(&s, 100, send, NULL));
  lm_scale_frame_t f;
  ASSERT_FALSE(lm_scale_core_peek(&s, 100, &f));
  acknowledge(&s, LM_SCALE_TARE, 110);
  ASSERT_TRUE(lm_scale_core_tick(&s, 110, send, NULL));
  ASSERT_EQ_INT(0, s.reply_count);
  const lm_scale_event_t e = {.kind = LM_SCALE_WEIGHT, .grams = .22};
  ASSERT_TRUE(lm_scale_core_event(&s, &e, 120));
  ASSERT_FALSE(lm_scale_core_peek(&s, 120, &f));
  ASSERT_TRUE(lm_scale_core_tick(&s, 120, send, NULL));
  ASSERT_TRUE(lm_scale_core_peek(&s, 120, &f));
  ASSERT_EQ_INT(0x80, f.data[6]);
  lm_scale_core_delivered(&s);
  ASSERT_TRUE(lm_scale_core_peek(&s, 120, &f));
  ASSERT_EQ_INT(22, f.data[5]);
  return 0;
}
static int test_missing_real_ack_and_missing_post_tare_weight_fail(void) {
  for (unsigned ack = 0; ack < 2; ++ack) {
    lm_scale_adapter_core_t s;
    make_ready(&s);
    ASSERT_TRUE(command(&s, 4, (const uint8_t[]){0}, 1, 100));
    ASSERT_TRUE(lm_scale_core_tick(&s, 100, send, NULL));
    if (ack) acknowledge(&s, LM_SCALE_TARE, 110);
    ASSERT_FALSE(lm_scale_core_tick(&s, 551, send, NULL));
    ASSERT_EQ_INT(0, s.reply_count);
  }
  return 0;
}
static int test_stale_session_cannot_supply_weight_after_reconnect(void) {
  lm_scale_adapter_core_t s;
  make_ready(&s);
  ASSERT_TRUE(lm_scale_core_fresh(&s, 1100));
  ASSERT_FALSE(lm_scale_core_fresh(&s, 1101));
  ASSERT_FALSE(control(&s, 0, 1101));
  lm_scale_core_init(&s);
  const lm_scale_event_t e = {.kind = LM_SCALE_WEIGHT, .grams = 32};
  ASSERT_TRUE(lm_scale_core_event(&s, &e, 1200));
  ASSERT_FALSE(lm_scale_core_fresh(&s, 1200)); /* New source identity still required. */
  return 0;
}
static int test_gateway_fragmentation_negative_weight_and_corrupt_command(void) {
  lm_scale_adapter_core_t s;
  make_ready(&s);
  lm_scale_frame_t f = lm_scale_packet(38, (const uint8_t[]){2,0,1}, 3);
  for (size_t i = 0; i < f.size; ++i) ASSERT_TRUE(lm_scale_core_command(&s, f.data + i, 1, 100));
  ASSERT_EQ_INT(1, s.reply_count);
  lm_scale_core_delivered(&s);
  const lm_scale_event_t e = {.kind = LM_SCALE_WEIGHT, .grams = -312.6, .stable = true};
  ASSERT_TRUE(lm_scale_core_event(&s, &e, 110));
  ASSERT_TRUE(command(&s, 12, (const uint8_t[]){9,0,1,1,2,2,5,3,4}, 9, 110));
  ASSERT_TRUE(lm_scale_core_peek(&s, 110, &f));
  ASSERT_EQ_INT(0x1c, f.data[5]);
  ASSERT_EQ_INT(0x7a, f.data[6]);
  ASSERT_EQ_INT(3, f.data[10]);
  f = lm_scale_packet(4, (const uint8_t[]){0}, 1);
  f.data[f.size - 1] ^= 1;
  ASSERT_FALSE(lm_scale_core_command(&s, f.data, f.size, 110));
  return 0;
}
static int test_registration_distinguishes_absence_failure_and_other_scales(void) {
  ASSERT_EQ_INT(LM_SCALE_REGISTRATION_MISSING, lm_scale_registration_classify("[]", "LUNAR-C"));
  ASSERT_EQ_INT(LM_SCALE_REGISTRATION_UNKNOWN, lm_scale_registration_classify("timeout", "LUNAR-C"));
  ASSERT_EQ_INT(LM_SCALE_REGISTRATION_UNKNOWN, lm_scale_registration_classify("{}", "LUNAR-C"));
  ASSERT_EQ_INT(LM_SCALE_REGISTRATION_UNKNOWN, lm_scale_registration_classify("[]truncated-response", "LUNAR-C"));
  ASSERT_EQ_INT(LM_SCALE_REGISTRATION_UNKNOWN, lm_scale_registration_classify("[{}]", "LUNAR-C"));
  ASSERT_EQ_INT(LM_SCALE_REGISTRATION_OTHER, lm_scale_registration_classify("[{\"name\":\"Other\"}]", "LUNAR-C"));
  ASSERT_EQ_INT(LM_SCALE_REGISTRATION_PRESENT, lm_scale_registration_classify("[{\"name\":\"LUNAR-C\",\"connected\":false}]", "LUNAR-C"));
  ASSERT_EQ_U32(30000, lm_scale_registration_retry_ms(0));
  ASSERT_EQ_U32(60000, lm_scale_registration_retry_ms(1));
  ASSERT_EQ_U32(240000, lm_scale_registration_retry_ms(50));
  return 0;
}
static int test_repeated_stop_keeps_original_disconnect_deadline(void) {
  lm_scale_link_lifecycle_t link = {0};
  ASSERT_TRUE(lm_scale_link_begin_stop(&link, 100, true));
  ASSERT_EQ_U32(350, link.gateway_close_at);
  for (uint64_t now = 125; now <= 500; now += 25) {
    ASSERT_FALSE(lm_scale_link_begin_stop(&link, now, true));
    ASSERT_EQ_U32(350, link.gateway_close_at);
    ASSERT_FALSE(lm_scale_link_allows_io(&link));
  }
  return 0;
}

static int test_shutdown_rejects_late_source_io_until_all_callbacks_drain(void) {
  lm_scale_link_lifecycle_t link = {0};
  ASSERT_TRUE(lm_scale_link_begin_stop(&link, 100, false));
  /* No gateway exists, but the source may still deliver pending GATT callbacks. */
  ASSERT_EQ_U32(0, link.gateway_close_at);
  ASSERT_FALSE(lm_scale_link_allows_io(&link));
  ASSERT_FALSE(lm_scale_link_finish_stop(&link, true, false, false, false));
  ASSERT_FALSE(lm_scale_link_finish_stop(&link, false, false, true, false));
  ASSERT_FALSE(lm_scale_link_finish_stop(&link, false, false, false, true));
  ASSERT_FALSE(lm_scale_link_finish_stop(&link, false, true, false, false));
  ASSERT_TRUE(lm_scale_link_finish_stop(&link, false, false, false, false));
  ASSERT_TRUE(lm_scale_link_allows_io(&link));
  ASSERT_TRUE(lm_scale_link_begin_stop(&link, 1000, true));
  ASSERT_EQ_U32(1250, link.gateway_close_at);
  return 0;
}

static int test_gateway_backpressure_expires_even_with_fresh_weights(void) {
  lm_scale_adapter_core_t s;
  make_ready(&s);
  ASSERT_TRUE(command(&s, 38, (const uint8_t[]){2,0,1}, 3, 100));
  const lm_scale_event_t e = {.kind = LM_SCALE_WEIGHT, .grams = 12.34};
  ASSERT_TRUE(lm_scale_core_event(&s, &e, 1101));
  ASSERT_TRUE(lm_scale_core_fresh(&s, 1101));
  ASSERT_FALSE(lm_scale_core_tick(&s, 1101, send, NULL));
  ASSERT_EQ_INT(0, sent_count);
  return 0;
}

static int test_source_measurements_coalesce_to_latest_value(void) {
  lm_scale_adapter_core_t s;
  make_ready(&s);
  ASSERT_TRUE(command(&s, 12, (const uint8_t[]){9,0,1,1,2,2,5,3,4}, 9, 100));
  for (unsigned i = 1; i <= 100; ++i) {
    const lm_scale_event_t e = {.kind = LM_SCALE_WEIGHT, .grams = i / 100.0};
    ASSERT_TRUE(lm_scale_core_event(&s, &e, 100 + i));
  }
  lm_scale_frame_t f;
  ASSERT_TRUE(lm_scale_core_peek(&s, 200, &f));
  ASSERT_EQ_INT(100, f.data[5]);
  ASSERT_EQ_INT(0, s.reply_count);
  lm_scale_core_delivered(&s);
  ASSERT_FALSE(lm_scale_core_peek(&s, 200, &f));
  return 0;
}

static int test_captured_startup_body_waits_for_gram_handshake_across_fragment_boundaries(void) {
  /* The uploaded log contains only this body+checksum, not its opcode. Test
   * plausible headers without treating any reconstructed header as captured. */
  const uint8_t tail[] = {0x07,0x02,0x14,0x02,0x3c,0x14,0x00,0x57,0x18};
  const uint8_t opcodes[] = {7,8,12,255};
  const lm_scale_driver_t *d = lm_scale_driver_find("acaia_legacy");
  for (size_t op = 0; op < sizeof(opcodes); ++op) {
    lm_scale_frame_t startup = lm_scale_packet(opcodes[op], tail, sizeof(tail) - 2);
    ASSERT_TRUE(!memcmp(startup.data + 3, tail, sizeof(tail)));
    for (size_t split = 1; split < startup.size; ++split) {
      union { uint64_t alignment; uint8_t data[512]; } state;
      d->reset(state.data);
      event_count = 0;
      ASSERT_TRUE(d->feed(state.data, startup.data, split, collect, NULL));
      ASSERT_EQ_INT(0, event_count);
      ASSERT_TRUE(d->feed(state.data, startup.data + split, startup.size - split, collect, NULL));
      ASSERT_EQ_INT(1, event_count);
      ASSERT_EQ_INT(LM_SCALE_PROTOCOL_NOTE, events[0].kind);
      ASSERT_EQ_INT(startup.size, events[0].frame->size);
      ASSERT_TRUE(!memcmp(events[0].frame->data, startup.data, startup.size));
      lm_scale_adapter_core_t core;
      lm_scale_core_init(&core);
      ASSERT_TRUE(lm_scale_core_event(&core, &events[0], 100));
      ASSERT_FALSE(core.ready);
      ASSERT_FALSE(lm_scale_core_fresh(&core, 100));
      ASSERT_TRUE(d->feed(state.data, startup.data, startup.size, collect, NULL));
      ASSERT_EQ_INT(1, event_count); /* Auxiliary logging is once per opcode/session. */
      uint8_t handshake_and_weight[sizeof(pair_reply) + sizeof(weight)];
      memcpy(handshake_and_weight, pair_reply, sizeof(pair_reply));
      memcpy(handshake_and_weight + sizeof(pair_reply), weight, sizeof(weight));
      ASSERT_TRUE(d->feed(state.data, handshake_and_weight, sizeof(handshake_and_weight), collect, NULL));
      ASSERT_EQ_INT(3, event_count);
      ASSERT_EQ_INT(LM_SCALE_READY, events[1].kind);
      ASSERT_EQ_INT(LM_SCALE_WEIGHT, events[2].kind);
      ASSERT_TRUE(lm_scale_core_event(&core, &events[1], 110));
      ASSERT_TRUE(lm_scale_core_event(&core, &events[2], 110));
      ASSERT_TRUE(lm_scale_core_fresh(&core, 110));
    }
  }
  return 0;
}

static int test_short_settings_require_grams_and_unknown_live_settings_fail_closed(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("acaia_legacy");
  union { uint64_t alignment; uint8_t data[512]; } state;
  d->reset(state.data);
  event_count = 0;
  lm_scale_frame_t f = lm_scale_packet(8, (const uint8_t[]){7,19,2,2,1,0,1}, 7);
  ASSERT_TRUE(d->feed(state.data, f.data, f.size, collect, NULL));
  ASSERT_EQ_INT(1, event_count);
  ASSERT_EQ_INT(LM_SCALE_READY, events[0].kind);
  f = lm_scale_packet(8, (const uint8_t[]){7,2,20,2,60,20,0}, 7);
  ASSERT_FALSE(d->feed(state.data, f.data, f.size, collect, NULL));
  ASSERT_EQ_INT(1, event_count);
  lm_scale_source_diagnostics_t info;
  d->diagnostics(state.data, &info);
  ASSERT_STREQ("Unrecognized source settings after gram handshake", info.error);
  d->reset(state.data);
  f = lm_scale_packet(8, (const uint8_t[]){7,19,5,2,1,0,1}, 7);
  ASSERT_FALSE(d->feed(state.data, f.data, f.size, collect, NULL));
  d->diagnostics(state.data, &info);
  ASSERT_STREQ("Source settings do not report grams", info.error);
  return 0;
}

static int test_source_failure_diagnostics_keep_full_bad_frame_and_partial_prefix(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("acaia_legacy");
  union { uint64_t alignment; uint8_t data[512]; } state;
  d->reset(state.data);
  event_count = 0;
  uint8_t bad[sizeof(weight)];
  memcpy(bad, weight, sizeof(bad));
  bad[sizeof(bad) - 1] ^= 1;
  ASSERT_TRUE(d->feed(state.data, bad, 3, collect, NULL));
  ASSERT_FALSE(d->feed(state.data, bad + 3, sizeof(bad) - 3, collect, NULL));
  lm_scale_source_diagnostics_t info;
  d->diagnostics(state.data, &info);
  ASSERT_TRUE(info.complete);
  ASSERT_FALSE(info.checksum_valid);
  ASSERT_EQ_INT(sizeof(bad), info.failure_frame->size);
  ASSERT_TRUE(!memcmp(bad, info.failure_frame->data, sizeof(bad)));
  ASSERT_STREQ("Source frame checksum mismatch", info.error);
  ASSERT_EQ_INT(0, event_count);
  d->reset(state.data);
  ASSERT_TRUE(d->feed(state.data, pair_reply, sizeof(pair_reply), collect, NULL));
  const uint8_t too_long[] = {0xef,0xdd,8,127};
  ASSERT_FALSE(d->feed(state.data, too_long, sizeof(too_long), collect, NULL));
  d->diagnostics(state.data, &info);
  ASSERT_FALSE(info.complete);
  ASSERT_EQ_INT(sizeof(too_long), info.failure_frame->size);
  ASSERT_TRUE(!memcmp(too_long, info.failure_frame->data, sizeof(too_long)));
  return 0;
}

static int test_unknown_ack_is_observed_but_never_completes_control(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("acaia_legacy");
  union { uint64_t alignment; uint8_t data[512]; } state;
  d->reset(state.data);
  event_count = 0;
  lm_scale_frame_t f = lm_scale_packet(12, (const uint8_t[]){4,11,7,0xe0}, 4);
  ASSERT_TRUE(d->feed(state.data, f.data, f.size, collect, NULL));
  ASSERT_EQ_INT(1, event_count);
  ASSERT_EQ_INT(LM_SCALE_CONTROL_UNKNOWN, events[0].control);
  lm_scale_adapter_core_t core;
  make_ready(&core);
  ASSERT_TRUE(control(&core, 0, 100));
  ASSERT_TRUE(lm_scale_core_tick(&core, 100, send, NULL));
  ASSERT_TRUE(lm_scale_core_event(&core, &events[0], 110));
  ASSERT_FALSE(core.controls[0].acknowledged);
  ASSERT_EQ_I64(1, core.ack_count);
  ASSERT_TRUE(d->feed(state.data, weight, sizeof(weight), collect, NULL));
  lm_scale_source_diagnostics_t info;
  d->diagnostics(state.data, &info);
  ASSERT_TRUE(info.last_ack_frame != NULL);
  ASSERT_EQ_INT(f.size, info.last_ack_frame->size);
  ASSERT_TRUE(!memcmp(f.data, info.last_ack_frame->data, f.size));
  return 0;
}

static int test_maximum_source_frame_is_preserved_across_notifications(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("acaia_legacy");
  union { uint64_t alignment; uint8_t data[512]; } state;
  d->reset(state.data);
  event_count = 0;
  uint8_t payload[LM_SCALE_FRAME_MAX - 5];
  memset(payload, 0x5a, sizeof(payload));
  payload[0] = sizeof(payload);
  lm_scale_frame_t f = lm_scale_packet(7, payload, sizeof(payload));
  ASSERT_EQ_INT(LM_SCALE_FRAME_MAX, f.size);
  ASSERT_TRUE(d->feed(state.data, f.data, 64, collect, NULL));
  ASSERT_EQ_INT(0, event_count);
  ASSERT_TRUE(d->feed(state.data, f.data + 64, f.size - 64, collect, NULL));
  ASSERT_EQ_INT(1, event_count);
  ASSERT_EQ_INT(f.size, events[0].frame->size);
  ASSERT_TRUE(!memcmp(f.data, events[0].frame->data, f.size));
  f.data[f.size - 1] ^= 1;
  ASSERT_TRUE(d->feed(state.data, f.data, 64, collect, NULL));
  ASSERT_FALSE(d->feed(state.data, f.data + 64, f.size - 64, collect, NULL));
  lm_scale_source_diagnostics_t info;
  d->diagnostics(state.data, &info);
  ASSERT_EQ_INT(LM_SCALE_FRAME_MAX, info.failure_frame->size);
  ASSERT_TRUE(!memcmp(f.data, info.failure_frame->data, f.size));
  ASSERT_TRUE(info.complete);
  ASSERT_FALSE(info.checksum_valid);
  return 0;
}

static int test_control_timeout_snapshot_distinguishes_operation_ack_and_post_tare_weight(void) {
  lm_scale_adapter_core_t s;
  lm_scale_control_info_t info;
  make_ready(&s);
  ASSERT_TRUE(control(&s, 0, 100));
  ASSERT_TRUE(lm_scale_core_control_info(&s, 100, &info));
  ASSERT_FALSE(info.transaction.sent);
  ASSERT_EQ_I64(0, info.weights_after);
  ASSERT_TRUE(lm_scale_core_tick(&s, 100, send, NULL));
  const lm_scale_event_t wrong = {.kind = LM_SCALE_ACK, .control = LM_SCALE_TIMER_STOP,
    .ack_wire_valid = true, .ack_id = 9, .ack_code = 0x21};
  ASSERT_TRUE(lm_scale_core_event(&s, &wrong, 130));
  ASSERT_TRUE(lm_scale_core_control_info(&s, 551, &info));
  ASSERT_EQ_INT(LM_SCALE_TIMER_START, info.transaction.command);
  ASSERT_EQ_INT(LM_SCALE_TIMER_RESET, info.transaction.operation);
  ASSERT_EQ_I64(451, info.elapsed_ms);
  ASSERT_EQ_INT(450, info.timeout_ms);
  ASSERT_FALSE(info.transaction.acknowledged);
  ASSERT_FALSE(info.last_ack_matched);
  ASSERT_EQ_INT(0x21, info.last_ack_code);
  ASSERT_EQ_INT(9, info.last_ack_id);
  ASSERT_FALSE(lm_scale_core_tick(&s, 551, send, NULL));
  ASSERT_TRUE(lm_scale_core_control_info(&s, 551, &info)); /* Still available after failure. */
  make_ready(&s);
  ASSERT_TRUE(command(&s, 4, (const uint8_t[]){0}, 1, 100));
  ASSERT_TRUE(lm_scale_core_tick(&s, 100, send, NULL));
  acknowledge(&s, LM_SCALE_TARE, 110);
  ASSERT_TRUE(lm_scale_core_control_info(&s, 551, &info));
  ASSERT_TRUE(info.transaction.acknowledged);
  ASSERT_EQ_I64(10, info.ack_delay_ms);
  ASSERT_EQ_I64(0, info.weights_after);
  ASSERT_TRUE(info.last_ack_matched);
  ASSERT_FALSE(lm_scale_core_tick(&s, 551, send, NULL));
  return 0;
}

int run_scale_adapter_tests(void) {
  RUN_TEST(test_captured_startup_body_waits_for_gram_handshake_across_fragment_boundaries);
  RUN_TEST(test_short_settings_require_grams_and_unknown_live_settings_fail_closed);
  RUN_TEST(test_source_failure_diagnostics_keep_full_bad_frame_and_partial_prefix);
  RUN_TEST(test_unknown_ack_is_observed_but_never_completes_control);
  RUN_TEST(test_maximum_source_frame_is_preserved_across_notifications);
  RUN_TEST(test_control_timeout_snapshot_distinguishes_operation_ack_and_post_tare_weight);
  RUN_TEST(test_repeated_stop_keeps_original_disconnect_deadline);
  RUN_TEST(test_shutdown_rejects_late_source_io_until_all_callbacks_drain);
  RUN_TEST(test_gateway_backpressure_expires_even_with_fresh_weights);
  RUN_TEST(test_source_measurements_coalesce_to_latest_value);
  RUN_TEST(test_recorded_source_fragmentation_and_driver_contract);
  RUN_TEST(test_compound_source_ack_weight_buttons_and_malformed_tail);
  RUN_TEST(test_source_checksum_and_gram_unit_are_required);
  RUN_TEST(test_gateway_handshake_and_settings_ack);
  RUN_TEST(test_start_resets_once_and_coalesces_until_ack_delivery);
  RUN_TEST(test_start_stop_start_order_and_control_limit);
  RUN_TEST(test_tare_requires_ack_and_new_measurement_preserves_flowing_water);
  RUN_TEST(test_missing_real_ack_and_missing_post_tare_weight_fail);
  RUN_TEST(test_stale_session_cannot_supply_weight_after_reconnect);
  RUN_TEST(test_gateway_fragmentation_negative_weight_and_corrupt_command);
  RUN_TEST(test_registration_distinguishes_absence_failure_and_other_scales);
  return 0;
}
