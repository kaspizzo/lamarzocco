#include "scale_driver.h"
#include <math.h>
#include <string.h>

typedef struct {
  lm_scale_decoder_t decoder;
  lm_scale_frame_t last_ack_frame;
  lm_scale_event_cb emit;
  void *ctx;
  const char *error;
  bool ready;
  uint8_t noted_opcodes[32];
} acaia_t;
_Static_assert(sizeof(acaia_t) <= 512, "Acaia state must fit the BLE runtime buffer");

static bool reject(acaia_t *s, const char *reason) { s->error = reason; return false; }
static bool emit(acaia_t *s, const lm_scale_event_t *event) {
  return s->emit(s->ctx, event) || reject(s, "Source event rejected by adapter");
}
static bool note_once(acaia_t *s, const lm_scale_frame_t *frame, const char *reason) {
  uint8_t op = frame->data[2];
  uint8_t mask = 1U << (op & 7);
  if (s->noted_opcodes[op >> 3] & mask) return true;
  s->noted_opcodes[op >> 3] |= mask;
  const lm_scale_event_t note = {.kind = LM_SCALE_PROTOCOL_NOTE, .frame = frame, .note = reason};
  return emit(s, &note);
}

static bool source_frame(void *ctx, const lm_scale_frame_t *frame) {
  acaia_t *s = ctx;
  const uint8_t *p = frame->data + 3;
  size_t size = frame->size - 5;
  if (p[0] != size) return reject(s, "Source payload length mismatch");
  if (frame->data[2] == 8) {
    /* Both settings layouts expose battery/unit in the same first fields.
     * Never interpret the captured seven-byte startup body as grams merely
     * because its checksum is valid: its missing header/layout is unknown. */
    if (size == 7 || size == 9) {
      uint8_t unit = p[2] & 0x7f;
      if (unit == 2) {
        if ((p[1] & 0x7f) > 100) return reject(s, "Invalid source battery value");
        const lm_scale_event_t e = {.kind = LM_SCALE_READY, .battery = p[1] & 0x7f};
        if (!emit(s, &e)) return false;
        s->ready = true;
        return true;
      }
      if (size == 9 || unit == 5) return reject(s, "Source settings do not report grams");
    }
    if (s->ready) return reject(s, "Unrecognized source settings after gram handshake");
    return note_once(s, frame, "Unrecognized startup settings; awaiting validated grams");
  }
  /* Valid ancillary replies are not measurements, confirmations or readiness.
   * This matches the source-facing bridge behavior; handshake timeout and the
   * one-second weight freshness guard still apply. */
  if (frame->data[2] != 12) return note_once(s, frame, "Ancillary source frame ignored");
  /* Validate the entire compound frame before emitting anything. */
  for (unsigned pass = 0; pass < 2; ++pass) {
    for (size_t pos = 1; pos < size;) {
      uint8_t kind = p[pos];
      if (kind == 8) {
        if (pos + 2 >= size || (p[pos + 2] != 5 && p[pos + 2] != 7))
          return reject(s, "Invalid source button event");
        pos += 2;
        continue;
      }
      size_t n = kind == 5 ? 7 : kind == 6 ? 2 : kind == 7 ? 4 : kind == 11 ? 3 : 0;
      if (!n) {
        if (!s->ready) return note_once(s, frame, "Unknown startup event; awaiting validated grams");
        return reject(s, "Unknown source event after gram handshake");
      }
      if (pos + n > size) return reject(s, "Truncated source event");
      lm_scale_event_t e = {0};
      bool emit_event = true;
      if (kind == 5) {
        if (p[pos + 5] > 6) return reject(s, "Invalid source weight exponent");
        uint32_t raw = (uint32_t)p[pos + 1] | ((uint32_t)p[pos + 2] << 8) |
                       ((uint32_t)p[pos + 3] << 16) | ((uint32_t)p[pos + 4] << 24);
        e.kind = LM_SCALE_WEIGHT;
        e.grams = raw / pow(10.0, p[pos + 5]);
        if (p[pos + 6] & 2) e.grams = -e.grams;
        e.stable = (p[pos + 6] & 1) != 0;
      } else if (kind == 6) {
        if ((p[pos + 1] & 0x7f) > 100) return reject(s, "Invalid source battery event");
        e.kind = LM_SCALE_BATTERY;
        e.battery = p[pos + 1] & 0x7f;
      } else if (kind == 11) {
        e.kind = LM_SCALE_ACK;
        e.ack_wire_valid = true;
        e.ack_id = p[pos + 1];
        e.ack_code = p[pos + 2];
        switch (p[pos + 2]) {
          case 0x80: e.control = LM_SCALE_TARE; break;
          case 0x00: case 0x01: e.control = LM_SCALE_TIMER_START; break;
          case 0x20: case 0x21: e.control = LM_SCALE_TIMER_STOP; break;
          case 0x40: case 0x41: e.control = LM_SCALE_TIMER_RESET; break;
          default: e.control = LM_SCALE_CONTROL_UNKNOWN; break;
        }
      } else emit_event = false; /* Timer readings are independent of weight. */
      if (pass && emit_event) {
        if (e.kind == LM_SCALE_ACK) s->last_ack_frame = *frame;
        if (!s->emit(s->ctx, &e)) return reject(s, "Source event rejected by adapter");
      }
      pos += n;
    }
  }
  return true;
}
static void reset(void *state) { memset(state, 0, sizeof(acaia_t)); }
static bool feed(void *state, const uint8_t *data, size_t size, lm_scale_event_cb cb, void *ctx) {
  acaia_t *s = state;
  s->error = NULL;
  s->emit = cb;
  s->ctx = ctx;
  return lm_scale_decode(&s->decoder, data, size, source_frame, s);
}
static void diagnostics(void *state, lm_scale_source_diagnostics_t *out) {
  acaia_t *s = state;
  *out = (lm_scale_source_diagnostics_t){.last_ack_frame = s->last_ack_frame.size ? &s->last_ack_frame : NULL};
  if (s->decoder.error == LM_SCALE_DECODE_OK) return;
  out->failure_frame = &s->decoder.last_frame;
  out->complete = s->decoder.frame_complete;
  out->checksum_valid = s->decoder.checksum_valid;
  out->error = s->error ? s->error : s->decoder.error == LM_SCALE_DECODE_CHECKSUM ?
    "Source frame checksum mismatch" : "Invalid source frame length";
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
  if (c > LM_SCALE_TIMER_RESET) return (lm_scale_frame_t){0};
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
  .diagnostics = diagnostics,
};
