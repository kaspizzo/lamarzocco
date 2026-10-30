#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LM_SCALE_DEBUG_SECTOR 4096U
#define LM_SCALE_DEBUG_RECORD_SIZE 256U
#define LM_SCALE_DEBUG_MESSAGE_SIZE 216U

typedef struct {
  uint32_t magic, crc;
  uint64_t sequence;
  int64_t unix_seconds;
  uint64_t uptime_ms;
  uint32_t boot_id;
  uint16_t message_length, reserved;
  char message[LM_SCALE_DEBUG_MESSAGE_SIZE];
} lm_scale_debug_record_t;
typedef struct {
  void *ctx;
  size_t size;
  bool (*read)(void *, size_t, void *, size_t);
  bool (*write)(void *, size_t, const void *, size_t);
  bool (*erase)(void *, size_t, size_t);
} lm_scale_debug_io_t;
typedef struct {
  lm_scale_debug_io_t io;
  size_t next_slot, count, capacity;
  uint64_t next_sequence;
  bool ready;
} lm_scale_debug_store_t;
typedef enum {
  LM_SCALE_DEBUG_OK, LM_SCALE_DEBUG_EMPTY, LM_SCALE_DEBUG_FOREIGN, LM_SCALE_DEBUG_IO_ERROR,
} lm_scale_debug_result_t;

/* Uses a dedicated, sector-aligned region. Never formats nonblank foreign data.
 * Each record has a CRC; torn writes are ignored and not programmed over. */
lm_scale_debug_result_t lm_scale_debug_store_open(lm_scale_debug_store_t *, const lm_scale_debug_io_t *, bool create);
bool lm_scale_debug_store_append(lm_scale_debug_store_t *, lm_scale_debug_record_t *);
bool lm_scale_debug_store_read(lm_scale_debug_store_t *, size_t chronological_slot, lm_scale_debug_record_t *, bool *valid);
bool lm_scale_debug_store_clear(lm_scale_debug_store_t *);
