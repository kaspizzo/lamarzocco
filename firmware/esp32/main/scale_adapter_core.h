#pragma once
#include "scale_driver.h"

#define LM_SCALE_CONTROL_LIMIT 4
#define LM_SCALE_REPLY_LIMIT 12
#define LM_SCALE_FRESH_MS 1000
#define LM_SCALE_ACK_MS 450

typedef bool (*lm_scale_send_control_cb)(void *ctx, lm_scale_control_t control);
typedef struct {
  lm_scale_control_t command;
  lm_scale_control_t operation;
  uint32_t id;
  uint64_t started;
  uint64_t weights_before;
  bool sent, acknowledged, delivering, reset_before_start;
} lm_scale_transaction_t;
typedef struct {
  lm_scale_frame_t frame;
  uint32_t transaction;
  uint64_t created;
} lm_scale_reply_t;
typedef struct {
  bool ready, has_weight, paired, streaming, weight_pending;
  double grams;
  bool stable;
  uint8_t battery;
  uint64_t updated, weights;
  uint32_t next_id, last_id;
  uint32_t control_timeout_ms;
  lm_scale_control_t last_command;
  lm_scale_decoder_t decoder;
  lm_scale_transaction_t controls[LM_SCALE_CONTROL_LIMIT];
  size_t control_count;
  lm_scale_reply_t replies[LM_SCALE_REPLY_LIMIT];
  size_t reply_count;
  const char *error;
} lm_scale_adapter_core_t;

void lm_scale_core_init(lm_scale_adapter_core_t *s);
void lm_scale_core_end_gateway(lm_scale_adapter_core_t *s);
bool lm_scale_core_fresh(const lm_scale_adapter_core_t *s, uint64_t now);
bool lm_scale_core_event(lm_scale_adapter_core_t *s, const lm_scale_event_t *event, uint64_t now);
bool lm_scale_core_command(lm_scale_adapter_core_t *s, const uint8_t *data, size_t size, uint64_t now);
bool lm_scale_core_tick(lm_scale_adapter_core_t *s, uint64_t now, lm_scale_send_control_cb send, void *ctx);
bool lm_scale_core_peek(lm_scale_adapter_core_t *s, uint64_t now, lm_scale_frame_t *frame);
void lm_scale_core_delivered(lm_scale_adapter_core_t *s);
