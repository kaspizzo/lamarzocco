#pragma once
#include "scale_protocol.h"

/* Source-facing contract. Drivers translate their protocol into these events;
 * only the gateway facade knows the virtual Acaia wire format. All callbacks
 * execute on the NimBLE host task and must not block. */
typedef enum {
  LM_SCALE_TARE, LM_SCALE_TIMER_START, LM_SCALE_TIMER_STOP, LM_SCALE_TIMER_RESET,
} lm_scale_control_t;
typedef enum {
  LM_SCALE_READY, LM_SCALE_WEIGHT, LM_SCALE_BATTERY, LM_SCALE_ACK,
} lm_scale_event_kind_t;
typedef struct {
  lm_scale_event_kind_t kind;
  double grams;
  uint8_t battery;
  bool stable;
  lm_scale_control_t control;
} lm_scale_event_t;
typedef bool (*lm_scale_event_cb)(void *ctx, const lm_scale_event_t *event);
typedef struct {
  const char *id;
  /* Completion comes from a protocol ACK or observed scale state, never just
   * ATT write success. This capability does not imply hardware validation. */
  bool confirmed_controls;
  uint32_t control_timeout_ms; /* Zero keeps the gateway's default timeout. */
  const char *service_uuid;
  const char *notify_uuid;
  const char *write_uuid;
  size_t state_size;
  void (*reset)(void *state);
  bool (*feed)(void *state, const uint8_t *data, size_t size, lm_scale_event_cb cb, void *ctx);
  lm_scale_frame_t (*identify)(void);
  lm_scale_frame_t (*subscribe)(void);
  lm_scale_frame_t (*heartbeat)(void);
  lm_scale_frame_t (*control)(lm_scale_control_t control);
  /* Optional for protocols that report state instead of explicit ACKs. Arm
   * after queuing a write; inspect new measurements after feed(), on the same
   * host task. now is monotonic milliseconds. Failure ends the source session. */
  bool (*control_written)(void *state, lm_scale_control_t control, uint64_t now);
  bool (*observe_control)(void *state, uint64_t now, lm_scale_event_cb cb, void *ctx);
} lm_scale_driver_t;
const lm_scale_driver_t *lm_scale_driver_find(const char *id);
static inline bool lm_scale_driver_can_bridge(const lm_scale_driver_t *driver) {
  return driver != NULL && driver->confirmed_controls;
}
