#include "scale_driver.h"
#include <math.h>
#include <string.h>

typedef struct {
  lm_scale_decoder_t decoder;
  lm_scale_event_cb emit;
  void *ctx;
} acaia_t;

static bool source_frame(void *ctx, const lm_scale_frame_t *frame) {
  acaia_t *s = ctx;
  const uint8_t *p = frame->data + 3;
  size_t size = frame->size - 5;
  if (p[0] != size) return false;
  if (frame->data[2] == 8) {
    if (size != 9 || (p[2] & 0x7f) != 2 || (p[1] & 0x7f) > 100) return false;
    const lm_scale_event_t e = {.kind = LM_SCALE_READY, .battery = p[1] & 0x7f};
    return s->emit(s->ctx, &e);
  }
  if (frame->data[2] != 12) return false;
  /* Validate the entire compound frame before emitting anything. */
  for (unsigned pass = 0; pass < 2; ++pass) {
    for (size_t pos = 1; pos < size;) {
      uint8_t kind = p[pos];
      if (kind == 8) {
        if (pos + 2 >= size || (p[pos + 2] != 5 && p[pos + 2] != 7)) return false;
        pos += 2;
        continue;
      }
      size_t n = kind == 5 ? 7 : kind == 6 ? 2 : kind == 7 ? 4 : kind == 11 ? 3 : 0;
      if (!n || pos + n > size) return false;
      lm_scale_event_t e = {0};
      bool emit = true;
      if (kind == 5) {
        if (p[pos + 5] > 6) return false;
        uint32_t raw = (uint32_t)p[pos + 1] | ((uint32_t)p[pos + 2] << 8) |
                       ((uint32_t)p[pos + 3] << 16) | ((uint32_t)p[pos + 4] << 24);
        e.kind = LM_SCALE_WEIGHT;
        e.grams = raw / pow(10.0, p[pos + 5]);
        if (p[pos + 6] & 2) e.grams = -e.grams;
        e.stable = (p[pos + 6] & 1) != 0;
      } else if (kind == 6) {
        if ((p[pos + 1] & 0x7f) > 100) return false;
        e.kind = LM_SCALE_BATTERY;
        e.battery = p[pos + 1] & 0x7f;
      } else if (kind == 11) {
        e.kind = LM_SCALE_ACK;
        switch (p[pos + 2]) {
          case 0x80: e.control = LM_SCALE_TARE; break;
          case 0x00: case 0x01: e.control = LM_SCALE_TIMER_START; break;
          case 0x20: case 0x21: e.control = LM_SCALE_TIMER_STOP; break;
          case 0x40: case 0x41: e.control = LM_SCALE_TIMER_RESET; break;
          default: emit = false; break;
        }
      } else emit = false; /* Timer readings are independent of weight. */
      if (pass && emit && !s->emit(s->ctx, &e)) return false;
      pos += n;
    }
  }
  return true;
}
static void reset(void *state) { memset(state, 0, sizeof(acaia_t)); }
static bool feed(void *state, const uint8_t *data, size_t size, lm_scale_event_cb cb, void *ctx) {
  acaia_t *s = state;
  s->emit = cb;
  s->ctx = ctx;
  return lm_scale_decode(&s->decoder, data, size, source_frame, s);
}
static lm_scale_frame_t identify(void) {
  const uint8_t p[15] = {45,45,45,45,45,45,45,45,45,45,45,45,45,45,45};
  return lm_scale_packet(11, p, sizeof(p));
}
static lm_scale_frame_t subscribe(void) {
  const uint8_t p[] = {9, 0, 1, 1, 2, 2, 5, 3, 4};
  return lm_scale_packet(12, p, sizeof(p));
}
static lm_scale_frame_t heartbeat(void) {
  const uint8_t p[] = {2, 0};
  return lm_scale_packet(0, p, sizeof(p));
}
static lm_scale_frame_t control(lm_scale_control_t c) {
  if (c == LM_SCALE_TARE) return lm_scale_packet(4, (const uint8_t[]){0}, 1);
  uint8_t action = c == LM_SCALE_TIMER_START ? 0 : c == LM_SCALE_TIMER_STOP ? 2 : 1;
  return lm_scale_packet(13, (const uint8_t[]){0, action}, 2);
}
const lm_scale_driver_t lm_scale_acaia_legacy_driver = {
  .id = "acaia_legacy", .confirmed_controls = true,
  .service_uuid = "00001820-0000-1000-8000-00805f9b34fb",
  .notify_uuid = "00002a80-0000-1000-8000-00805f9b34fb",
  .write_uuid = "00002a80-0000-1000-8000-00805f9b34fb",
  .state_size = sizeof(acaia_t), .reset = reset, .feed = feed,
  .identify = identify, .subscribe = subscribe, .heartbeat = heartbeat, .control = control,
};
