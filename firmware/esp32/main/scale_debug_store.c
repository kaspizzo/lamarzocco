#include "scale_debug_store.h"
#include <string.h>

#define HEADER_MAGIC UINT64_C(0x31474244454c4353)
#define RECORD_MAGIC UINT32_C(0x31474244)
typedef struct { uint64_t magic; uint32_t version, crc; } store_header_t;
_Static_assert(sizeof(lm_scale_debug_record_t) == LM_SCALE_DEBUG_RECORD_SIZE, "Fixed flash record size");

static uint32_t crc32(const void *data, size_t length) {
  uint32_t crc = UINT32_MAX;
  const uint8_t *p = data;
  while (length--) {
    crc ^= *p++;
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
static bool blank(const void *data, size_t length) {
  const uint8_t *p = data;
  while (length--) if (*p++ != 0xff) return false;
  return true;
}
static bool valid_record(const lm_scale_debug_record_t *r) {
  return r->magic == RECORD_MAGIC && r->sequence && r->message_length < sizeof(r->message) &&
    r->message[r->message_length] == 0 &&
    r->crc == crc32((const uint8_t *)r + 8, sizeof(*r) - 8);
}
static size_t offset(size_t slot) { return LM_SCALE_DEBUG_SECTOR + slot * LM_SCALE_DEBUG_RECORD_SIZE; }
static bool write_header(lm_scale_debug_store_t *s) {
  store_header_t h = {.magic = HEADER_MAGIC, .version = 1};
  h.crc = crc32(&h, offsetof(store_header_t, crc));
  return s->io.write(s->io.ctx, 0, &h, sizeof(h));
}
lm_scale_debug_result_t lm_scale_debug_store_open(lm_scale_debug_store_t *s, const lm_scale_debug_io_t *io, bool create) {
  if (!s || !io || !io->read || !io->write || !io->erase ||
      io->size < 2 * LM_SCALE_DEBUG_SECTOR || io->size % LM_SCALE_DEBUG_SECTOR) return LM_SCALE_DEBUG_IO_ERROR;
  *s = (lm_scale_debug_store_t){.io = *io, .next_sequence = 1,
    .capacity = (io->size - LM_SCALE_DEBUG_SECTOR) / LM_SCALE_DEBUG_RECORD_SIZE};
  store_header_t h;
  if (!io->read(io->ctx, 0, &h, sizeof(h))) return LM_SCALE_DEBUG_IO_ERROR;
  if (blank(&h, sizeof(h))) {
    /* Check the entire reservation before claiming it, including its tail. */
    uint8_t buffer[LM_SCALE_DEBUG_RECORD_SIZE];
    for (size_t at = 0; at < io->size; at += sizeof(buffer)) {
      if (!io->read(io->ctx, at, buffer, sizeof(buffer))) return LM_SCALE_DEBUG_IO_ERROR;
      if (!blank(buffer, sizeof(buffer))) return LM_SCALE_DEBUG_FOREIGN;
    }
    if (!create) return LM_SCALE_DEBUG_EMPTY;
    if (!write_header(s)) return LM_SCALE_DEBUG_IO_ERROR;
  } else if (h.magic != HEADER_MAGIC || h.version != 1 || h.crc != crc32(&h, offsetof(store_header_t, crc))) {
    return LM_SCALE_DEBUG_FOREIGN;
  }
  lm_scale_debug_record_t r;
  for (size_t slot = 0; slot < s->capacity; ++slot) {
    if (!io->read(io->ctx, offset(slot), &r, sizeof(r))) return LM_SCALE_DEBUG_IO_ERROR;
    if (!valid_record(&r)) continue;
    ++s->count;
    if (r.sequence >= s->next_sequence) {
      s->next_sequence = r.sequence + 1;
      s->next_slot = (slot + 1) % s->capacity;
    }
  }
  s->ready = true;
  return LM_SCALE_DEBUG_OK;
}
bool lm_scale_debug_store_append(lm_scale_debug_store_t *s, lm_scale_debug_record_t *r) {
  if (!s || !s->ready || !r || r->message_length >= sizeof(r->message)) return false;
  lm_scale_debug_record_t old;
  const size_t per_sector = LM_SCALE_DEBUG_SECTOR / sizeof(old);
  /* Skip a partly programmed slot left by power loss, until blank space or a
   * sector boundary that can be erased without disturbing newer records. */
  while (s->next_slot % per_sector) {
    if (!s->io.read(s->io.ctx, offset(s->next_slot), &old, sizeof(old))) return false;
    if (blank(&old, sizeof(old))) break;
    s->next_slot = (s->next_slot + 1) % s->capacity;
  }
  if (s->next_slot % per_sector == 0) {
    size_t removed = 0;
    for (size_t i = 0; i < per_sector; ++i) {
      if (!s->io.read(s->io.ctx, offset(s->next_slot + i), &old, sizeof(old))) return false;
      removed += valid_record(&old);
    }
    if (!s->io.erase(s->io.ctx, offset(s->next_slot), LM_SCALE_DEBUG_SECTOR)) return false;
    s->count -= removed;
  }
  r->magic = RECORD_MAGIC;
  r->sequence = s->next_sequence;
  r->message[r->message_length] = 0;
  r->crc = crc32((const uint8_t *)r + 8, sizeof(*r) - 8);
  if (!s->io.write(s->io.ctx, offset(s->next_slot), r, sizeof(*r))) return false;
  ++s->count;
  ++s->next_sequence;
  s->next_slot = (s->next_slot + 1) % s->capacity;
  return true;
}
bool lm_scale_debug_store_read(lm_scale_debug_store_t *s, size_t slot, lm_scale_debug_record_t *r, bool *valid) {
  if (!s || !s->ready || slot >= s->capacity || !r || !valid ||
      !s->io.read(s->io.ctx, offset((s->next_slot + slot) % s->capacity), r, sizeof(*r))) return false;
  *valid = valid_record(r);
  return true;
}
bool lm_scale_debug_store_clear(lm_scale_debug_store_t *s) {
  if (!s || !s->ready || !s->io.erase(s->io.ctx, 0, s->io.size)) return false;
  s->next_slot = s->count = 0;
  s->next_sequence = 1;
  s->ready = write_header(s);
  return s->ready;
}
