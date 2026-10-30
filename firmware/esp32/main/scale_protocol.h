#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LM_SCALE_FRAME_MAX 125

typedef struct {
  uint8_t data[LM_SCALE_FRAME_MAX];
  size_t size;
} lm_scale_frame_t;

typedef enum {
  LM_SCALE_DECODE_OK, LM_SCALE_DECODE_LENGTH, LM_SCALE_DECODE_CHECKSUM, LM_SCALE_DECODE_REJECTED,
} lm_scale_decode_error_t;
typedef struct {
  uint8_t data[LM_SCALE_FRAME_MAX];
  size_t used;
  bool commands;
  /* Last assembled frame (including header/checksum), or an explicitly
   * incomplete prefix on framing failure. Never just the last BLE fragment. */
  lm_scale_frame_t last_frame;
  bool frame_complete, checksum_valid;
  lm_scale_decode_error_t error;
} lm_scale_decoder_t;

typedef bool (*lm_scale_frame_cb)(void *ctx, const lm_scale_frame_t *frame);
lm_scale_frame_t lm_scale_packet(uint8_t opcode, const uint8_t *payload, size_t size);
bool lm_scale_decode(lm_scale_decoder_t *decoder, const uint8_t *data, size_t size,
                     lm_scale_frame_cb callback, void *ctx);
