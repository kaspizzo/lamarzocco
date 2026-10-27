#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LM_SCALE_FRAME_MAX 125

typedef struct {
  uint8_t data[LM_SCALE_FRAME_MAX];
  size_t size;
} lm_scale_frame_t;

typedef struct {
  uint8_t data[LM_SCALE_FRAME_MAX];
  size_t used;
  bool commands;
} lm_scale_decoder_t;

typedef bool (*lm_scale_frame_cb)(void *ctx, const lm_scale_frame_t *frame);
lm_scale_frame_t lm_scale_packet(uint8_t opcode, const uint8_t *payload, size_t size);
bool lm_scale_decode(lm_scale_decoder_t *decoder, const uint8_t *data, size_t size,
                     lm_scale_frame_cb callback, void *ctx);
