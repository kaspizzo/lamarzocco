#include "scale_protocol.h"
#include <string.h>

lm_scale_frame_t lm_scale_packet(uint8_t opcode, const uint8_t *payload, size_t size) {
  lm_scale_frame_t frame = {0};
  if (size == 0 || size > LM_SCALE_FRAME_MAX - 5 || payload == NULL) return frame;
  frame.data[0] = 0xef;
  frame.data[1] = 0xdd;
  frame.data[2] = opcode;
  memcpy(frame.data + 3, payload, size);
  for (size_t i = 0; i < size; ++i) frame.data[3 + size + (i & 1)] += payload[i];
  frame.size = size + 5;
  return frame;
}

bool lm_scale_decode(lm_scale_decoder_t *d, const uint8_t *data, size_t size,
                     lm_scale_frame_cb callback, void *ctx) {
  if (!d || !data || !callback) return false;
  for (size_t i = 0; i < size; ++i) {
    if (d->used == sizeof(d->data)) return false;
    d->data[d->used++] = data[i];
    if (d->used == 1 && d->data[0] != 0xef) { d->used = 0; continue; }
    if (d->used == 2 && d->data[1] != 0xdd) {
      d->used = data[i] == 0xef ? 1 : 0;
      continue;
    }
    if (d->used < 4) continue;
    size_t length = d->data[3];
    if (d->commands) {
      switch (d->data[2]) {
        case 0: case 13: length = 2; break;
        case 4: length = 1; break;
        case 11: length = 15; break;
        case 38: length = 3; break;
        case 12: break;
        default: return false;
      }
    }
    if (length == 0 || length > LM_SCALE_FRAME_MAX - 5) return false;
    if (d->used < length + 5) continue;
    lm_scale_frame_t frame = lm_scale_packet(d->data[2], d->data + 3, length);
    if (memcmp(frame.data, d->data, frame.size) != 0) return false;
    d->used = 0;
    if (!callback(ctx, &frame)) return false;
  }
  return true;
}
