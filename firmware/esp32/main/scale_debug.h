#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool enabled, ready, clock_synced;
  uint32_t retained, capacity, dropped;
  char status[96];
} lm_scale_debug_info_t;
typedef bool (*lm_scale_debug_export_cb)(void *ctx, const char *text);
esp_err_t lm_scale_debug_init(void);
esp_err_t lm_scale_debug_configure(bool enabled);
void lm_scale_debug_get_info(lm_scale_debug_info_t *);
void lm_scale_debug_clock_synchronized(void);
/* Explicit scale diagnostics only. Never pass account tokens or machine GATT
 * payloads. Queues without waiting; all flash IO belongs to the worker task. */
void lm_scale_debug_record(const char *format, ...) __attribute__((format(printf, 1, 2)));
esp_err_t lm_scale_debug_export(lm_scale_debug_export_cb, void *);
/* Disables capture and clears only a recognized debug region. */
esp_err_t lm_scale_debug_clear(void);
