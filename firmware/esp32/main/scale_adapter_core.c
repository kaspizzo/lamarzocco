#include "scale_adapter_core.h"
#include <math.h>
#include <string.h>

static bool fail(lm_scale_adapter_core_t *s, const char *error) { s->error = error; return false; }
void lm_scale_core_init(lm_scale_adapter_core_t *s) {
  memset(s, 0, sizeof(*s));
  s->control_timeout_ms = LM_SCALE_ACK_MS;
  s->decoder.commands = true;
}
void lm_scale_core_end_gateway(lm_scale_adapter_core_t *s) {
  s->paired = s->streaming = s->weight_pending = false;
  s->control_count = s->reply_count = 0;
  s->last_id = 0;
  memset(&s->decoder, 0, sizeof(s->decoder));
  s->decoder.commands = true;
}
bool lm_scale_core_fresh(const lm_scale_adapter_core_t *s, uint64_t now) {
  return !s->error && s->ready && s->has_weight && now >= s->updated && now - s->updated <= LM_SCALE_FRESH_MS;
}
static bool tare_pending(const lm_scale_adapter_core_t *s) {
  for (size_t i = 0; i < s->control_count; ++i)
    if (s->controls[i].command == LM_SCALE_TARE && !s->controls[i].delivering) return true;
  return false;
}
static bool reply(lm_scale_adapter_core_t *s, lm_scale_frame_t frame, uint32_t id, uint64_t now) {
  if (!frame.size || s->reply_count == LM_SCALE_REPLY_LIMIT) return fail(s, "Gateway reply queue full");
  s->replies[s->reply_count++] = (lm_scale_reply_t){.frame = frame, .transaction = id, .created = now};
  return true;
}
static bool ack(lm_scale_adapter_core_t *s, uint8_t kind, uint32_t id, uint64_t now) {
  return reply(s, lm_scale_packet(12, (const uint8_t[]){4, 11, 0, kind}, 4), id, now);
}
bool lm_scale_core_event(lm_scale_adapter_core_t *s, const lm_scale_event_t *e, uint64_t now) {
  if (s->error) return false;
  if (e->kind == LM_SCALE_READY || e->kind == LM_SCALE_BATTERY) {
    if (e->battery > 100) return fail(s, "Invalid battery value");
    s->battery = e->battery;
    if (e->kind == LM_SCALE_READY) s->ready = true;
  } else if (e->kind == LM_SCALE_WEIGHT) {
    if (!isfinite(e->grams) || fabs(e->grams) > 42949672.95) return fail(s, "Invalid weight");
    s->grams = e->grams;
    s->stable = e->stable;
    s->has_weight = true;
    s->updated = now;
    s->weights++;
    if (s->streaming && !tare_pending(s)) s->weight_pending = true;
  } else if (e->kind == LM_SCALE_ACK && s->control_count) {
    lm_scale_transaction_t *c = &s->controls[0];
    /* Late ACKs cannot complete an operation that has not been sent. */
    if (c->sent && !c->delivering && c->operation == e->control) c->acknowledged = true;
  }
  return true;
}
static bool enqueue_control(lm_scale_adapter_core_t *s, lm_scale_control_t command) {
  if (s->last_id && s->last_command == command) {
    for (size_t i = 0; i < s->control_count; ++i)
      if (s->controls[i].id == s->last_id) return true;
  }
  if (s->control_count == LM_SCALE_CONTROL_LIMIT) return fail(s, "Too many scale controls");
  if (++s->next_id == 0) ++s->next_id;
  s->last_id = s->next_id;
  s->last_command = command;
  s->controls[s->control_count++] = (lm_scale_transaction_t){
    .command = command, .operation = command == LM_SCALE_TIMER_START ? LM_SCALE_TIMER_RESET : command,
    .reset_before_start = command == LM_SCALE_TIMER_START, .id = s->next_id,
  };
  if (command == LM_SCALE_TARE) s->weight_pending = false;
  return true;
}
typedef struct { lm_scale_adapter_core_t *s; uint64_t now; } command_ctx_t;
static bool command_frame(void *ctx, const lm_scale_frame_t *frame) {
  command_ctx_t *c = ctx;
  lm_scale_adapter_core_t *s = c->s;
  const uint8_t *p = frame->data + 3;
  size_t size = frame->size - 5;
  uint8_t op = frame->data[2];
  if (op == 11 && size == 15) {
    if (s->control_count) return fail(s, "Pairing during an outstanding control");
    s->paired = true;
    s->streaming = s->weight_pending = false;
    /* Virtual gateway profile, with the real source battery and gram unit. */
    return reply(s, lm_scale_packet(8, (const uint8_t[]){9, s->battery, 2, 16, 1, 0, 1, 1, 0}, 9), 0, c->now);
  }
  if (!s->paired) return fail(s, "Gateway command before pairing");
  if (op == 0 && size == 2 && p[0] == 2 && p[1] == 0) return true;
  if (op == 4 && size == 1 && p[0] == 0) return enqueue_control(s, LM_SCALE_TARE);
  if (op == 12 && size == 9 && memcmp(p, (const uint8_t[]){9, 0, 1, 1, 2, 2, 5, 3, 4}, 9) == 0) {
    s->streaming = true;
    s->weight_pending = !tare_pending(s);
    return true;
  }
  if (op == 38 && size == 3) {
    if (p[0] == 0 && p[1] == 0 && p[2] <= 2) {
      const lm_scale_control_t actions[] = {LM_SCALE_TIMER_START, LM_SCALE_TIMER_STOP, LM_SCALE_TIMER_RESET};
      return enqueue_control(s, actions[p[2]]);
    }
    bool valid = (p[0] == 2 && p[1] == 0 && p[2] <= 1) ||
                 (p[0] == 4 && p[1] == 0 && p[2] == 0) || p[0] == 5 ||
                 (p[0] == 6 && p[1] == 0 && p[2] == 0) ||
                 (p[0] == 7 && p[1] <= 3 && p[2] == 50);
    if (valid) {
      if (p[0] == 6) s->streaming = s->weight_pending = false;
      return ack(s, (uint8_t)(p[0] << 5), 0, c->now);
    }
  }
  return fail(s, "Unsupported gateway scale command");
}
bool lm_scale_core_command(lm_scale_adapter_core_t *s, const uint8_t *data, size_t size, uint64_t now) {
  if (!lm_scale_core_fresh(s, now)) return fail(s, "Source weight stale or disconnected");
  command_ctx_t ctx = {.s = s, .now = now};
  if (!lm_scale_decode(&s->decoder, data, size, command_frame, &ctx))
    return s->error ? false : fail(s, "Invalid gateway frame");
  return true;
}
bool lm_scale_core_tick(lm_scale_adapter_core_t *s, uint64_t now, lm_scale_send_control_cb send, void *ctx) {
  if (s->error) return false;
  if (s->has_weight && !lm_scale_core_fresh(s, now)) return fail(s, "Source weight stream timed out");
  if (s->reply_count && now - s->replies[0].created > LM_SCALE_FRESH_MS) return fail(s, "Gateway backpressure timeout");
  if (!s->control_count) return true;
  lm_scale_transaction_t *c = &s->controls[0];
  if (c->delivering) return true;
  if (!lm_scale_core_fresh(s, now)) return fail(s, "Source unavailable for control");
  if (c->sent) {
    if (now - c->started > s->control_timeout_ms) return fail(s, "Real scale control confirmation timed out");
    if (!c->acknowledged) return true;
    if (c->reset_before_start) {
      c->reset_before_start = c->sent = c->acknowledged = false;
      c->operation = LM_SCALE_TIMER_START;
    } else {
      if (c->command == LM_SCALE_TARE && s->weights <= c->weights_before) return true;
      uint8_t kind = c->command == LM_SCALE_TARE ? 0x80 :
                     c->command == LM_SCALE_TIMER_START ? 0 : c->command == LM_SCALE_TIMER_STOP ? 0x20 : 0x40;
      if (!ack(s, kind, c->id, now)) return false;
      c->delivering = true;
      if (c->command == LM_SCALE_TARE && s->streaming) s->weight_pending = true;
      return true;
    }
  }
  c->started = now;
  c->weights_before = s->weights;
  c->sent = true;
  if (!send(ctx, c->operation)) return fail(s, "Source control write failed");
  return true;
}
bool lm_scale_core_peek(lm_scale_adapter_core_t *s, uint64_t now, lm_scale_frame_t *frame) {
  if (!lm_scale_core_fresh(s, now)) return false;
  if (s->reply_count) { *frame = s->replies[0].frame; return true; }
  if (!s->weight_pending || !s->streaming || tare_pending(s)) return false;
  uint32_t raw = (uint32_t)llround(fabs(s->grams) * 100.0);
  uint8_t p[] = {8, 5, raw & 255, (raw >> 8) & 255, (raw >> 16) & 255, (raw >> 24) & 255,
                 2, (s->grams < 0 ? 2 : 0) | (s->stable ? 1 : 0)};
  *frame = lm_scale_packet(12, p, sizeof(p));
  return true;
}
void lm_scale_core_delivered(lm_scale_adapter_core_t *s) {
  if (!s->reply_count) { s->weight_pending = false; return; }
  uint32_t id = s->replies[0].transaction;
  memmove(s->replies, s->replies + 1, --s->reply_count * sizeof(s->replies[0]));
  if (id && s->control_count && s->controls[0].id == id) {
    memmove(s->controls, s->controls + 1, --s->control_count * sizeof(s->controls[0]));
  }
}
