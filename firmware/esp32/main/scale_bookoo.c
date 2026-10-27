#include "scale_bookoo.h"
#include <math.h>
#include <string.h>

/* Protocol sources (no third-party implementation copied):
 * https://github.com/BooKooCode/OpenSource/blob/main/bookoo_mini_scale/protocols.md
 * https://github.com/BooKooCode/OpenSource/blob/main/bookoo_ultra_scale/protocols.md
 * The manufacturer leaves sign-byte values unspecified. ASCII +/- and the
 * positive space/zero encodings follow the interoperability convention in
 * Cheerpipe/Open-Brew-by-Weight, GenericFf11.cpp. Unknown weight signs fail. */
typedef struct {
  uint8_t data[20];
  size_t used;
  bool ready;
  lm_scale_bookoo_measurement_t last;
  uint64_t samples, observed_samples, written_at, plateau_at;
  uint32_t baseline_timer, plateau_timer;
  lm_scale_control_t pending;
  bool control_pending, have_plateau;
} bookoo_t;

static uint8_t checksum(const uint8_t *data, size_t size) {
  uint8_t value = 0;
  for (size_t i = 0; i < size; ++i) value ^= data[i];
  return value;
}
static bool valid_sign(uint8_t sign) {
  return sign == '+' || sign == '-' || sign == ' ' || sign == 0;
}
static uint32_t u24_be(const uint8_t *data) {
  return ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
}
bool lm_scale_bookoo_parse(const uint8_t *data, size_t size, lm_scale_bookoo_measurement_t *out) {
  if (!data || !out || size != 20 || data[0] != 3 || data[1] != 11 ||
      checksum(data, 19) != data[19] || !valid_sign(data[6]) || data[13] > 100 || data[5] > 2)
    return false;
  lm_scale_bookoo_measurement_t parsed = {
    .grams = u24_be(data + 7) / 100.0,
    .timer_ms = u24_be(data + 2),
    .battery = data[13],
    .flow_valid = valid_sign(data[10]),
  };
  if (data[6] == '-') parsed.grams = -parsed.grams;
  if (parsed.flow_valid) {
    parsed.flow_gps = (((uint16_t)data[11] << 8) | data[12]) / 100.0;
    if (data[10] == '-') parsed.flow_gps = -parsed.flow_gps;
  }
  *out = parsed;
  return true;
}
static void reset(void *state) { memset(state, 0, sizeof(bookoo_t)); }
static bool feed(void *state, const uint8_t *data, size_t size, lm_scale_event_cb emit, void *ctx) {
  if (!state || !data || !emit) return false;
  bookoo_t *s = state;
  for (size_t i = 0; i < size; ++i) {
    if (s->used == sizeof(s->data)) return false;
    s->data[s->used++] = data[i];
    if (s->used == 1 && s->data[0] != 3) return false;
    if (s->used < sizeof(s->data)) continue;
    s->used = 0;
    if (checksum(s->data, 19) != s->data[19]) return false;
    /* Ultra powder-weight and automatic-mode event packets are not live
     * measurements or generic control ACKs. They must not refresh weight age. */
    if (s->data[1] == 15 || s->data[1] == 13) continue;
    lm_scale_bookoo_measurement_t parsed;
    if (!lm_scale_bookoo_parse(s->data, sizeof(s->data), &parsed)) return false;
    lm_scale_event_t event = {.kind = s->ready ? LM_SCALE_BATTERY : LM_SCALE_READY, .battery = parsed.battery};
    if (!emit(ctx, &event)) return false;
    s->ready = true;
    event = (lm_scale_event_t){.kind = LM_SCALE_WEIGHT, .grams = parsed.grams};
    if (!emit(ctx, &event)) return false;
    s->last = parsed;
    ++s->samples;
  }
  return true;
}
static lm_scale_frame_t command(uint8_t opcode) {
  lm_scale_frame_t frame = {.data = {3, 10, opcode, 0, 0}, .size = 6};
  frame.data[5] = checksum(frame.data, 5);
  return frame;
}
static lm_scale_frame_t control(lm_scale_control_t control) {
  switch (control) {
    case LM_SCALE_TARE: return command(1);
    case LM_SCALE_TIMER_START: return command(4);
    case LM_SCALE_TIMER_STOP: return command(5);
    case LM_SCALE_TIMER_RESET: return command(6);
    default: return (lm_scale_frame_t){0};
  }
}
lm_scale_frame_t lm_scale_bookoo_tare_start(void) { return command(7); }

static bool control_written(void *state, lm_scale_control_t control, uint64_t now) {
  if (!state || control < LM_SCALE_TARE || control > LM_SCALE_TIMER_RESET) return false;
  bookoo_t *s = state;
  if (!s->ready || !s->samples || s->control_pending) return false;
  s->pending = control;
  s->control_pending = true;
  s->observed_samples = s->samples;
  s->baseline_timer = s->last.timer_ms;
  s->written_at = now;
  s->have_plateau = false;
  return true;
}
static bool observe_control(void *state, uint64_t now, lm_scale_event_cb emit, void *ctx) {
  if (!state || !emit) return false;
  bookoo_t *s = state;
  if (!s->control_pending || s->observed_samples == s->samples || now < s->written_at) return true;
  s->observed_samples = s->samples;
  bool complete = false;
  switch (s->pending) {
    case LM_SCALE_TARE:
      /* Best-effort state confirmation, not a documented BOOKOO ACK. A tare
       * during fast inflow may never reach this threshold and will time out. */
      complete = fabs(s->last.grams) <= .10;
      break;
    case LM_SCALE_TIMER_RESET:
      complete = s->last.timer_ms == 0;
      break;
    case LM_SCALE_TIMER_START: {
      uint32_t delta = (s->last.timer_ms - s->baseline_timer) & 0xffffff;
      /* Reject a backward jump/reset; allow the 24-bit timer to wrap. */
      complete = delta > 0 && delta < 0x800000;
      break;
    }
    case LM_SCALE_TIMER_STOP:
      /* Require a plateau across separate notifications for 200 ms. A single
       * repeated timer value may just be the scale's display quantization. */
      if (!s->have_plateau || s->last.timer_ms != s->plateau_timer) {
        s->have_plateau = true;
        s->plateau_timer = s->last.timer_ms;
        s->plateau_at = now;
      } else complete = now >= s->plateau_at && now - s->plateau_at >= 200;
      break;
  }
  if (!complete) return true;
  const lm_scale_event_t event = {.kind = LM_SCALE_ACK, .control = s->pending};
  s->control_pending = false;
  return emit(ctx, &event);
}

/* CCCD subscription starts reception; no Acaia identify/stream/heartbeat
 * messages are sent. Firmware-dependent Ultra keepalive/shutdown/mode writes
 * are deliberately absent. Command completion is inferred from new measurement
 * state above; this experimental bridge still needs hardware validation. */
#define BOOKOO_DRIVER(driver_id) { \
  .id = driver_id, .confirmed_controls = true, .control_timeout_ms = 2000, \
  .service_uuid = "00000ffe-0000-1000-8000-00805f9b34fb", \
  .notify_uuid = "0000ff11-0000-1000-8000-00805f9b34fb", \
  .write_uuid = "0000ff12-0000-1000-8000-00805f9b34fb", \
  .state_size = sizeof(bookoo_t), .reset = reset, .feed = feed, .control = control, \
  .control_written = control_written, .observe_control = observe_control, \
}
const lm_scale_driver_t lm_scale_bookoo_mini_driver = BOOKOO_DRIVER("bookoo_mini");
const lm_scale_driver_t lm_scale_bookoo_ultra_driver = BOOKOO_DRIVER("bookoo_ultra");
