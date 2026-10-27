#include "scale_bookoo.h"
#include "scale_adapter_core.h"
#include "test_support.h"

/* Synthetic fixtures from the published packet layout, not hardware captures. */
static const uint8_t positive[20] = {
  0x03,0x0b,0x00,0x75,0x30,0x01,0x2b,0x00,0x10,0xb0,
  0x2b,0x01,0x41,0x57,0x00,0x32,0x02,0x01,0x00,0xca,
};
static const uint8_t negative[20] = {
  0x03,0x0b,0x01,0x02,0x03,0x01,0x2d,0x01,0xe2,0x40,
  0x2d,0x01,0x41,0x57,0x00,0x32,0x02,0x01,0x00,0x8c,
};
static void repair_checksum(uint8_t data[20]) {
  data[19] = 0;
  for (size_t i = 0; i < 19; ++i) data[19] ^= data[i];
}
typedef union { uint64_t align; uint8_t bytes[512]; } driver_state_t;
typedef struct {
  lm_scale_event_t events[128];
  size_t count;
  lm_scale_adapter_core_t core;
  uint64_t now;
} sink_t;
static bool receive(void *ctx, const lm_scale_event_t *event) {
  sink_t *sink = ctx;
  if (sink->count == 128) return false;
  sink->events[sink->count++] = *event;
  return lm_scale_core_event(&sink->core, event, sink->now);
}
static void init(const lm_scale_driver_t *driver, driver_state_t *state, sink_t *sink) {
  memset(sink, 0, sizeof(*sink));
  lm_scale_core_init(&sink->core);
  if (driver->control_timeout_ms) sink->core.control_timeout_ms = driver->control_timeout_ms;
  sink->now = 100;
  driver->reset(state->bytes);
}
static int test_bookoo_registry_and_bridge_capability(void) {
  ASSERT_TRUE(lm_scale_driver_can_bridge(lm_scale_driver_find("acaia_legacy")));
  ASSERT_FALSE(lm_scale_driver_can_bridge(NULL));
  ASSERT_TRUE(lm_scale_driver_find(NULL) == NULL);
  ASSERT_TRUE(lm_scale_driver_find("unknown") == NULL);
  const char *ids[] = {"bookoo_mini", "bookoo_ultra"};
  for (size_t i = 0; i < 2; ++i) {
    const lm_scale_driver_t *d = lm_scale_driver_find(ids[i]);
    ASSERT_TRUE(d != NULL && d->state_size <= sizeof(driver_state_t));
    ASSERT_TRUE(lm_scale_driver_can_bridge(d));
    ASSERT_TRUE(d->control_written != NULL && d->observe_control != NULL);
    ASSERT_EQ_INT(2000, d->control_timeout_ms);
    ASSERT_TRUE(d->identify == NULL && d->subscribe == NULL && d->heartbeat == NULL);
    ASSERT_STREQ("00000ffe-0000-1000-8000-00805f9b34fb", d->service_uuid);
    ASSERT_STREQ("0000ff11-0000-1000-8000-00805f9b34fb", d->notify_uuid);
    ASSERT_STREQ("0000ff12-0000-1000-8000-00805f9b34fb", d->write_uuid);
  }
  return 0;
}
static int test_bookoo_golden_control_packets(void) {
  const uint8_t expected[][6] = {
    {3,10,1,0,0,8}, {3,10,4,0,0,13}, {3,10,5,0,0,12}, {3,10,6,0,0,15},
  };
  const char *ids[] = {"bookoo_mini", "bookoo_ultra"};
  for (size_t i = 0; i < 2; ++i) {
    const lm_scale_driver_t *d = lm_scale_driver_find(ids[i]);
    for (int control = LM_SCALE_TARE; control <= LM_SCALE_TIMER_RESET; ++control) {
      lm_scale_frame_t frame = d->control((lm_scale_control_t)control);
      ASSERT_EQ_INT(6, frame.size);
      ASSERT_EQ_INT(0, memcmp(expected[control], frame.data, 6));
    }
    ASSERT_EQ_INT(0, d->control((lm_scale_control_t)99).size);
  }
  lm_scale_frame_t combined = lm_scale_bookoo_tare_start();
  ASSERT_EQ_INT(6, combined.size);
  ASSERT_EQ_INT(0, memcmp((uint8_t[]){3,10,7,0,0,14}, combined.data, 6));
  return 0;
}
static int test_bookoo_measurement_units_signs_and_timer(void) {
  lm_scale_bookoo_measurement_t out;
  ASSERT_TRUE(lm_scale_bookoo_parse(positive, 20, &out));
  ASSERT_FLOAT_EQ(42.72, out.grams, .00001);
  ASSERT_EQ_U32(30000, out.timer_ms);
  ASSERT_FLOAT_EQ(3.21, out.flow_gps, .00001);
  ASSERT_TRUE(out.flow_valid);
  ASSERT_EQ_INT(87, out.battery);
  ASSERT_TRUE(lm_scale_bookoo_parse(negative, 20, &out));
  ASSERT_FLOAT_EQ(-1234.56, out.grams, .00001);
  ASSERT_EQ_U32(0x010203, out.timer_ms);
  ASSERT_FLOAT_EQ(-3.21, out.flow_gps, .00001);
  uint8_t data[20];
  memcpy(data, positive, 20);
  data[5] = 2; /* Ultra display in ounces: payload remains grams. */
  const uint8_t signs[] = {0, ' ', '+'};
  for (size_t i = 0; i < sizeof(signs); ++i) {
    data[6] = data[10] = signs[i];
    repair_checksum(data);
    ASSERT_TRUE(lm_scale_bookoo_parse(data, 20, &out));
    ASSERT_FLOAT_EQ(42.72, out.grams, .00001);
    ASSERT_FLOAT_EQ(3.21, out.flow_gps, .00001);
  }
  data[10] = 0xff;
  repair_checksum(data);
  ASSERT_TRUE(lm_scale_bookoo_parse(data, 20, &out));
  ASSERT_FALSE(out.flow_valid);
  ASSERT_FLOAT_EQ(42.72, out.grams, .00001);
  return 0;
}
static int test_bookoo_rejects_malformed_measurements_without_output(void) {
  uint8_t data[20];
  lm_scale_bookoo_measurement_t out = {.grams = 987.6, .timer_ms = 123};
  const size_t offsets[] = {0, 1, 5, 6, 13, 19};
  const uint8_t invalid[] = {4, 15, 3, 0xff, 101, 0};
  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    memcpy(data, positive, 20);
    data[offsets[i]] = invalid[i];
    if (offsets[i] != 19) repair_checksum(data);
    ASSERT_FALSE(lm_scale_bookoo_parse(data, 20, &out));
    ASSERT_FLOAT_EQ(987.6, out.grams, .00001);
    ASSERT_EQ_U32(123, out.timer_ms);
  }
  ASSERT_FALSE(lm_scale_bookoo_parse(positive, 19, &out));
  ASSERT_FALSE(lm_scale_bookoo_parse(positive, 21, &out));
  ASSERT_FALSE(lm_scale_bookoo_parse(NULL, 20, &out));
  ASSERT_FALSE(lm_scale_bookoo_parse(positive, 20, NULL));
  return 0;
}
static int test_bookoo_fragmented_and_concatenated_notifications(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("bookoo_mini");
  uint8_t trace[40];
  memcpy(trace, positive, 20);
  memcpy(trace + 20, negative, 20);
  for (size_t stride = 1; stride <= sizeof(trace); ++stride) {
    driver_state_t state;
    sink_t sink;
    init(d, &state, &sink);
    for (size_t pos = 0; pos < sizeof(trace); pos += stride) {
      size_t count = sizeof(trace) - pos < stride ? sizeof(trace) - pos : stride;
      ASSERT_TRUE(d->feed(state.bytes, trace + pos, count, receive, &sink));
    }
    ASSERT_EQ_INT(4, sink.count);
    ASSERT_EQ_INT(LM_SCALE_READY, sink.events[0].kind);
    ASSERT_EQ_INT(LM_SCALE_WEIGHT, sink.events[1].kind);
    ASSERT_EQ_INT(LM_SCALE_BATTERY, sink.events[2].kind);
    ASSERT_EQ_INT(LM_SCALE_WEIGHT, sink.events[3].kind);
    ASSERT_FLOAT_EQ(-1234.56, sink.core.grams, .00001);
    ASSERT_TRUE(lm_scale_core_fresh(&sink.core, 100));
  }
  return 0;
}
static int test_bookoo_events_do_not_create_ack_or_refresh_stale_weight(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("bookoo_ultra");
  driver_state_t state;
  sink_t sink;
  init(d, &state, &sink);
  uint8_t data[20];
  memcpy(data, positive, 20);
  data[1] = 13;
  repair_checksum(data);
  ASSERT_TRUE(d->feed(state.bytes, data, 20, receive, &sink));
  ASSERT_EQ_INT(0, sink.count);
  ASSERT_FALSE(sink.core.ready);
  ASSERT_TRUE(d->feed(state.bytes, positive, 20, receive, &sink));
  sink.now = 100 + LM_SCALE_FRESH_MS + 1;
  for (int type = 13; type <= 15; type += 2) {
    data[1] = type;
    repair_checksum(data);
    ASSERT_TRUE(d->feed(state.bytes, data, 20, receive, &sink));
  }
  ASSERT_EQ_INT(2, sink.count);
  ASSERT_EQ_INT(1, sink.core.weights);
  ASSERT_FALSE(lm_scale_core_fresh(&sink.core, sink.now));
  ASSERT_EQ_INT(0, sink.core.reply_count);
  return 0;
}
static int test_bookoo_bad_checksum_and_new_session_reset(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("bookoo_ultra");
  driver_state_t state;
  sink_t sink;
  init(d, &state, &sink);
  uint8_t data[20];
  memcpy(data, positive, 20);
  data[7] ^= 1;
  ASSERT_FALSE(d->feed(state.bytes, data, 20, receive, &sink));
  ASSERT_EQ_INT(0, sink.count);
  ASSERT_TRUE(d->feed(state.bytes, positive, 7, receive, &sink));
  d->reset(state.bytes); /* A disconnect cannot carry a partial old frame. */
  ASSERT_TRUE(d->feed(state.bytes, negative, 20, receive, &sink));
  ASSERT_EQ_INT(LM_SCALE_READY, sink.events[0].kind);
  init(d, &state, &sink);
  ASSERT_TRUE(d->feed(state.bytes, positive, 20, receive, &sink));
  ASSERT_EQ_INT(LM_SCALE_READY, sink.events[0].kind);
  return 0;
}
static bool measurement(const lm_scale_driver_t *d, driver_state_t *state, sink_t *sink,
                        uint64_t now, uint32_t timer_ms, double grams) {
  uint8_t data[20];
  memcpy(data, positive, 20);
  data[2] = timer_ms >> 16;
  data[3] = timer_ms >> 8;
  data[4] = timer_ms;
  uint32_t raw = (uint32_t)llround(fabs(grams) * 100);
  data[6] = grams < 0 ? '-' : '+';
  data[7] = raw >> 16;
  data[8] = raw >> 8;
  data[9] = raw;
  repair_checksum(data);
  sink->now = now;
  return d->feed(state->bytes, data, 20, receive, sink) &&
         d->observe_control(state->bytes, now, receive, sink);
}
static size_t ack_count(const sink_t *sink) {
  size_t count = 0;
  for (size_t i = 0; i < sink->count; ++i) count += sink->events[i].kind == LM_SCALE_ACK;
  return count;
}
static int test_bookoo_tare_needs_new_zero_measurement(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("bookoo_mini");
  driver_state_t state;
  sink_t sink;
  init(d, &state, &sink);
  ASSERT_FALSE(d->control_written(state.bytes, LM_SCALE_TARE, 100));
  ASSERT_TRUE(measurement(d, &state, &sink, 100, 30000, 42.72));
  ASSERT_FALSE(d->control_written(state.bytes, (lm_scale_control_t)99, 101));
  ASSERT_TRUE(d->control_written(state.bytes, LM_SCALE_TARE, 101));
  ASSERT_FALSE(d->control_written(state.bytes, LM_SCALE_TIMER_START, 102));
  ASSERT_TRUE(d->observe_control(state.bytes, 150, receive, &sink));
  ASSERT_EQ_INT(0, ack_count(&sink));
  ASSERT_TRUE(measurement(d, &state, &sink, 200, 30000, 42.72));
  ASSERT_EQ_INT(0, ack_count(&sink));
  ASSERT_TRUE(measurement(d, &state, &sink, 300, 30000, -.03));
  ASSERT_EQ_INT(1, ack_count(&sink));
  ASSERT_EQ_INT(LM_SCALE_TARE, sink.events[sink.count - 1].control);
  ASSERT_TRUE(measurement(d, &state, &sink, 400, 30000, .02));
  ASSERT_EQ_INT(1, ack_count(&sink));
  return 0;
}
static int test_bookoo_timer_state_confirmations_and_wrap(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("bookoo_ultra");
  driver_state_t state;
  sink_t sink;
  init(d, &state, &sink);
  ASSERT_TRUE(measurement(d, &state, &sink, 100, 1000, 1));
  ASSERT_TRUE(d->control_written(state.bytes, LM_SCALE_TIMER_RESET, 101));
  ASSERT_TRUE(measurement(d, &state, &sink, 150, 1000, 1));
  ASSERT_EQ_INT(0, ack_count(&sink));
  ASSERT_TRUE(measurement(d, &state, &sink, 200, 0, 1));
  ASSERT_EQ_INT(1, ack_count(&sink));
  ASSERT_EQ_INT(LM_SCALE_TIMER_RESET, sink.events[sink.count - 1].control);
  ASSERT_TRUE(d->control_written(state.bytes, LM_SCALE_TIMER_START, 201));
  ASSERT_TRUE(measurement(d, &state, &sink, 250, 0, 1));
  ASSERT_EQ_INT(1, ack_count(&sink));
  ASSERT_TRUE(measurement(d, &state, &sink, 300, 100, 1));
  ASSERT_EQ_INT(2, ack_count(&sink));
  ASSERT_EQ_INT(LM_SCALE_TIMER_START, sink.events[sink.count - 1].control);
  ASSERT_TRUE(d->control_written(state.bytes, LM_SCALE_TIMER_STOP, 301));
  ASSERT_TRUE(measurement(d, &state, &sink, 350, 200, 1));
  ASSERT_TRUE(measurement(d, &state, &sink, 450, 200, 1));
  ASSERT_EQ_INT(2, ack_count(&sink));
  ASSERT_TRUE(d->observe_control(state.bytes, 549, receive, &sink)); /* Silence is not a stopped timer. */
  ASSERT_EQ_INT(2, ack_count(&sink));
  ASSERT_TRUE(measurement(d, &state, &sink, 550, 300, 1)); /* Still running. */
  ASSERT_EQ_INT(2, ack_count(&sink));
  ASSERT_TRUE(measurement(d, &state, &sink, 650, 300, 1));
  ASSERT_TRUE(measurement(d, &state, &sink, 750, 300, 1));
  ASSERT_EQ_INT(3, ack_count(&sink));
  ASSERT_EQ_INT(LM_SCALE_TIMER_STOP, sink.events[sink.count - 1].control);
  ASSERT_TRUE(measurement(d, &state, &sink, 800, 0xfffff0, 1));
  ASSERT_TRUE(d->control_written(state.bytes, LM_SCALE_TIMER_START, 801));
  ASSERT_TRUE(measurement(d, &state, &sink, 850, 10, 1));
  ASSERT_EQ_INT(4, ack_count(&sink));
  return 0;
}
static int test_bookoo_reset_backward_timer_and_nonweight_do_not_ack_start(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("bookoo_ultra");
  driver_state_t state;
  sink_t sink;
  init(d, &state, &sink);
  ASSERT_TRUE(measurement(d, &state, &sink, 100, 30000, 0));
  ASSERT_TRUE(d->control_written(state.bytes, LM_SCALE_TIMER_START, 101));
  ASSERT_TRUE(measurement(d, &state, &sink, 200, 0, 0));
  ASSERT_EQ_INT(0, ack_count(&sink));
  uint8_t data[20];
  memcpy(data, positive, 20);
  for (int type = 13; type <= 15; type += 2) {
    data[1] = type;
    repair_checksum(data);
    ASSERT_TRUE(d->feed(state.bytes, data, 20, receive, &sink));
    ASSERT_TRUE(d->observe_control(state.bytes, 300, receive, &sink));
  }
  ASSERT_EQ_INT(0, ack_count(&sink));
  d->reset(state.bytes);
  ASSERT_TRUE(measurement(d, &state, &sink, 400, 31000, 0));
  ASSERT_EQ_INT(0, ack_count(&sink));
  return 0;
}
typedef struct {
  const lm_scale_driver_t *driver;
  driver_state_t *state;
  sink_t *sink;
  lm_scale_control_t writes[16];
  size_t count;
  bool fail;
} source_t;
static bool source_send(void *ctx, lm_scale_control_t control) {
  source_t *source = ctx;
  if (source->fail || source->count == 16) return false;
  source->writes[source->count++] = control;
  return source->driver->control_written(source->state->bytes, control, source->sink->now);
}
static bool tick_source(source_t *source, uint64_t now) {
  source->sink->now = now;
  return lm_scale_core_tick(&source->sink->core, now, source_send, source);
}
static bool gateway(sink_t *sink, uint8_t op, const uint8_t *data, size_t size) {
  lm_scale_frame_t frame = lm_scale_packet(op, data, size);
  return lm_scale_core_command(&sink->core, frame.data, frame.size, sink->now);
}
static bool pair_gateway(sink_t *sink) {
  const uint8_t identity[15] = {45,45,45,45,45,45,45,45,45,45,45,45,45,45,45};
  if (!gateway(sink, 11, identity, sizeof(identity))) return false;
  lm_scale_core_delivered(&sink->core);
  return gateway(sink, 12, (const uint8_t[]){9,0,1,1,2,2,5,3,4}, 9);
}
static int expect_gateway_ack(sink_t *sink, uint8_t kind) {
  lm_scale_frame_t frame;
  ASSERT_TRUE(lm_scale_core_peek(&sink->core, sink->now, &frame));
  lm_scale_frame_t expected = lm_scale_packet(12, (const uint8_t[]){4,11,0,kind}, 4);
  ASSERT_EQ_INT(expected.size, frame.size);
  ASSERT_EQ_INT(0, memcmp(expected.data, frame.data, frame.size));
  lm_scale_core_delivered(&sink->core);
  return 0;
}
static int test_bookoo_gateway_tare_timer_sequence_and_weight_forwarding(void) {
  const char *ids[] = {"bookoo_mini", "bookoo_ultra"};
  for (size_t i = 0; i < 2; ++i) {
    const lm_scale_driver_t *d = lm_scale_driver_find(ids[i]);
    driver_state_t state;
    sink_t sink;
    init(d, &state, &sink);
    source_t source = {.driver = d, .state = &state, .sink = &sink};
    ASSERT_TRUE(measurement(d, &state, &sink, 100, 30000, 50));
    ASSERT_TRUE(pair_gateway(&sink));
    ASSERT_TRUE(gateway(&sink, 4, (const uint8_t[]){0}, 1));
    ASSERT_TRUE(tick_source(&source, 101));
    lm_scale_frame_t frame;
    ASSERT_FALSE(lm_scale_core_peek(&sink.core, 101, &frame));
    ASSERT_TRUE(measurement(d, &state, &sink, 150, 30000, 50));
    ASSERT_TRUE(tick_source(&source, 151));
    ASSERT_EQ_INT(0, sink.core.reply_count);
    ASSERT_TRUE(measurement(d, &state, &sink, 200, 30000, 0));
    ASSERT_TRUE(tick_source(&source, 201));
    ASSERT_EQ_INT(0, expect_gateway_ack(&sink, 0x80));
    ASSERT_TRUE(lm_scale_core_peek(&sink.core, 201, &frame));
    ASSERT_EQ_INT(5, frame.data[4]); /* Forwarded Acaia weight event. */
    ASSERT_EQ_INT(0, frame.data[5]);
    lm_scale_core_delivered(&sink.core);
    ASSERT_TRUE(gateway(&sink, 38, (const uint8_t[]){0,0,0}, 3));
    ASSERT_TRUE(tick_source(&source, 250));
    ASSERT_EQ_INT(LM_SCALE_TIMER_RESET, source.writes[1]);
    ASSERT_TRUE(measurement(d, &state, &sink, 300, 0, 0));
    ASSERT_TRUE(tick_source(&source, 301));
    ASSERT_EQ_INT(3, source.count);
    ASSERT_EQ_INT(LM_SCALE_TIMER_START, source.writes[2]);
    ASSERT_EQ_INT(0, sink.core.reply_count);
    ASSERT_TRUE(measurement(d, &state, &sink, 400, 100, 2.9));
    ASSERT_TRUE(tick_source(&source, 401));
    ASSERT_EQ_INT(0, expect_gateway_ack(&sink, 0));
    ASSERT_TRUE(gateway(&sink, 38, (const uint8_t[]){0,0,1}, 3));
    ASSERT_TRUE(tick_source(&source, 450));
    ASSERT_TRUE(measurement(d, &state, &sink, 500, 200, 2.9));
    ASSERT_TRUE(measurement(d, &state, &sink, 600, 200, 2.9));
    ASSERT_TRUE(measurement(d, &state, &sink, 700, 200, 2.9));
    ASSERT_TRUE(tick_source(&source, 701));
    ASSERT_EQ_INT(0, expect_gateway_ack(&sink, 0x20));
    ASSERT_TRUE(gateway(&sink, 38, (const uint8_t[]){0,0,2}, 3));
    ASSERT_TRUE(tick_source(&source, 800));
    ASSERT_TRUE(measurement(d, &state, &sink, 850, 0, 2.9));
    ASSERT_TRUE(tick_source(&source, 851));
    ASSERT_EQ_INT(0, expect_gateway_ack(&sink, 0x40));
    ASSERT_EQ_INT(5, source.count);
    ASSERT_EQ_INT(0, sink.core.control_count);
    ASSERT_FLOAT_EQ(2.9, sink.core.grams, .0001);
    ASSERT_TRUE(lm_scale_core_peek(&sink.core, sink.now, &frame));
    lm_scale_frame_t expected = lm_scale_packet(12, (const uint8_t[]){8,5,0x22,1,0,0,2,0}, 8);
    ASSERT_EQ_INT(expected.size, frame.size);
    ASSERT_EQ_INT(0, memcmp(expected.data, frame.data, frame.size));
    ASSERT_TRUE(sink.core.error == NULL);
  }
  return 0;
}
static int test_bookoo_ignored_control_times_out_and_failed_write_stops(void) {
  const lm_scale_driver_t *d = lm_scale_driver_find("bookoo_mini");
  driver_state_t state;
  sink_t sink;
  init(d, &state, &sink);
  source_t source = {.driver = d, .state = &state, .sink = &sink};
  ASSERT_TRUE(measurement(d, &state, &sink, 100, 0, 50));
  ASSERT_TRUE(pair_gateway(&sink));
  ASSERT_TRUE(gateway(&sink, 4, (const uint8_t[]){0}, 1));
  ASSERT_TRUE(tick_source(&source, 101));
  for (uint64_t now = 200; now <= 2000; now += 300) {
    ASSERT_TRUE(measurement(d, &state, &sink, now, 0, 50));
    ASSERT_TRUE(tick_source(&source, now));
  }
  ASSERT_TRUE(measurement(d, &state, &sink, 2102, 0, 50));
  ASSERT_FALSE(tick_source(&source, 2102));
  ASSERT_CONTAINS(sink.core.error, "confirmation timed out");
  ASSERT_EQ_INT(0, ack_count(&sink));
  ASSERT_EQ_INT(0, sink.core.reply_count);
  ASSERT_EQ_INT(1, source.count);
  init(d, &state, &sink);
  source.fail = true;
  ASSERT_TRUE(measurement(d, &state, &sink, 100, 0, 50));
  ASSERT_TRUE(pair_gateway(&sink));
  ASSERT_TRUE(gateway(&sink, 4, (const uint8_t[]){0}, 1));
  ASSERT_FALSE(tick_source(&source, 101));
  ASSERT_CONTAINS(sink.core.error, "write failed");
  return 0;
}
int run_scale_bookoo_tests(void) {
  RUN_TEST(test_bookoo_tare_needs_new_zero_measurement);
  RUN_TEST(test_bookoo_timer_state_confirmations_and_wrap);
  RUN_TEST(test_bookoo_reset_backward_timer_and_nonweight_do_not_ack_start);
  RUN_TEST(test_bookoo_gateway_tare_timer_sequence_and_weight_forwarding);
  RUN_TEST(test_bookoo_ignored_control_times_out_and_failed_write_stops);
  RUN_TEST(test_bookoo_registry_and_bridge_capability);
  RUN_TEST(test_bookoo_golden_control_packets);
  RUN_TEST(test_bookoo_measurement_units_signs_and_timer);
  RUN_TEST(test_bookoo_rejects_malformed_measurements_without_output);
  RUN_TEST(test_bookoo_fragmented_and_concatenated_notifications);
  RUN_TEST(test_bookoo_events_do_not_create_ack_or_refresh_stale_weight);
  RUN_TEST(test_bookoo_bad_checksum_and_new_session_reset);
  return 0;
}
