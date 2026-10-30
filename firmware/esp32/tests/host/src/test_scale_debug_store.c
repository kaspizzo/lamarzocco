#include "scale_debug_store.h"
#include "test_support.h"
#include <string.h>

static uint8_t flash[3 * LM_SCALE_DEBUG_SECTOR];
static unsigned writes, erases;
static bool tear_next_write;
static bool read_flash(void *ctx, size_t off, void *out, size_t n) {
  (void)ctx;
  if (off + n > sizeof(flash)) return false;
  memcpy(out, flash + off, n);
  return true;
}
static bool write_flash(void *ctx, size_t off, const void *data, size_t n) {
  (void)ctx;
  if (off + n > sizeof(flash)) return false;
  const uint8_t *bytes = data;
  size_t limit = tear_next_write ? n / 2 : n;
  bool torn = tear_next_write;
  tear_next_write = false;
  for (size_t i = 0; i < limit; ++i) {
    if ((flash[off + i] & bytes[i]) != bytes[i]) return false;
    flash[off + i] &= bytes[i];
  }
  ++writes;
  return !torn;
}
static bool erase_flash(void *ctx, size_t off, size_t n) {
  (void)ctx;
  if (off % LM_SCALE_DEBUG_SECTOR || n % LM_SCALE_DEBUG_SECTOR || off + n > sizeof(flash)) return false;
  memset(flash + off, 0xff, n);
  ++erases;
  return true;
}
static lm_scale_debug_io_t io(void) {
  return (lm_scale_debug_io_t){.size = sizeof(flash), .read = read_flash, .write = write_flash, .erase = erase_flash};
}
static void reset_flash(void) {
  memset(flash, 0xff, sizeof(flash));
  writes = erases = 0;
  tear_next_write = false;
}
static lm_scale_debug_record_t event(void) {
  lm_scale_debug_record_t r = {.unix_seconds = 1793250000, .uptime_ms = 1234, .boot_id = 7};
  strcpy(r.message, "source disconnected reason=531");
  r.message_length = strlen(r.message);
  return r;
}
static int test_debug_storage_never_claims_foreign_or_nonblank_data(void) {
  reset_flash();
  lm_scale_debug_store_t s;
  lm_scale_debug_io_t backend = io();
  ASSERT_EQ_INT(LM_SCALE_DEBUG_EMPTY, lm_scale_debug_store_open(&s, &backend, false));
  ASSERT_EQ_INT(0, writes);
  flash[sizeof(flash) - 1] = 0;
  ASSERT_EQ_INT(LM_SCALE_DEBUG_FOREIGN, lm_scale_debug_store_open(&s, &backend, true));
  ASSERT_EQ_INT(0, writes);
  ASSERT_EQ_INT(0, erases);
  ASSERT_EQ_INT(0, flash[sizeof(flash) - 1]);
  reset_flash();
  memcpy(flash, "OTHERFS", 7);
  ASSERT_EQ_INT(LM_SCALE_DEBUG_FOREIGN, lm_scale_debug_store_open(&s, &backend, true));
  ASSERT_EQ_INT(0, erases);
  ASSERT_FALSE(lm_scale_debug_store_clear(&s));
  return 0;
}
static int test_debug_ring_wraps_and_recovers_chronological_events_after_reboot(void) {
  reset_flash();
  lm_scale_debug_store_t s;
  lm_scale_debug_io_t backend = io();
  ASSERT_EQ_INT(LM_SCALE_DEBUG_OK, lm_scale_debug_store_open(&s, &backend, true));
  for (unsigned i = 0; i < 40; ++i) {
    lm_scale_debug_record_t r = event();
    ASSERT_TRUE(lm_scale_debug_store_append(&s, &r));
    ASSERT_EQ_I64(i + 1, r.sequence);
  }
  ASSERT_EQ_INT(32, s.capacity);
  ASSERT_EQ_INT(24, s.count); /* Oldest sector reclaimed; eight new slots used. */
  ASSERT_EQ_INT(3, erases);
  ASSERT_EQ_INT(LM_SCALE_DEBUG_OK, lm_scale_debug_store_open(&s, &backend, false));
  uint64_t expected = 17;
  for (size_t i = 0; i < s.capacity; ++i) {
    lm_scale_debug_record_t r;
    bool valid;
    ASSERT_TRUE(lm_scale_debug_store_read(&s, i, &r, &valid));
    if (!valid) continue;
    ASSERT_EQ_I64(expected++, r.sequence);
    ASSERT_EQ_I64(1793250000, r.unix_seconds);
    ASSERT_STREQ("source disconnected reason=531", r.message);
  }
  ASSERT_EQ_I64(41, expected);
  return 0;
}
static int test_debug_torn_write_is_skipped_and_capture_continues_after_reboot(void) {
  reset_flash();
  lm_scale_debug_store_t s;
  lm_scale_debug_io_t backend = io();
  ASSERT_EQ_INT(LM_SCALE_DEBUG_OK, lm_scale_debug_store_open(&s, &backend, true));
  lm_scale_debug_record_t r = event();
  ASSERT_TRUE(lm_scale_debug_store_append(&s, &r));
  tear_next_write = true;
  ASSERT_FALSE(lm_scale_debug_store_append(&s, &r));
  ASSERT_EQ_INT(LM_SCALE_DEBUG_OK, lm_scale_debug_store_open(&s, &backend, false));
  ASSERT_EQ_INT(1, s.count);
  ASSERT_TRUE(lm_scale_debug_store_append(&s, &r));
  ASSERT_EQ_I64(2, r.sequence);
  ASSERT_EQ_INT(LM_SCALE_DEBUG_OK, lm_scale_debug_store_open(&s, &backend, false));
  ASSERT_EQ_INT(2, s.count);
  return 0;
}
static int test_debug_corruption_and_clear_do_not_return_bad_records(void) {
  reset_flash();
  lm_scale_debug_store_t s;
  lm_scale_debug_io_t backend = io();
  ASSERT_EQ_INT(LM_SCALE_DEBUG_OK, lm_scale_debug_store_open(&s, &backend, true));
  lm_scale_debug_record_t r = event();
  ASSERT_TRUE(lm_scale_debug_store_append(&s, &r));
  flash[LM_SCALE_DEBUG_SECTOR + offsetof(lm_scale_debug_record_t, message)] ^= 1;
  ASSERT_EQ_INT(LM_SCALE_DEBUG_OK, lm_scale_debug_store_open(&s, &backend, false));
  ASSERT_EQ_INT(0, s.count);
  ASSERT_TRUE(lm_scale_debug_store_append(&s, &r));
  ASSERT_TRUE(lm_scale_debug_store_clear(&s));
  ASSERT_EQ_INT(LM_SCALE_DEBUG_OK, lm_scale_debug_store_open(&s, &backend, false));
  ASSERT_EQ_INT(0, s.count);
  return 0;
}
int run_scale_debug_store_tests(void) {
  RUN_TEST(test_debug_storage_never_claims_foreign_or_nonblank_data);
  RUN_TEST(test_debug_ring_wraps_and_recovers_chronological_events_after_reboot);
  RUN_TEST(test_debug_torn_write_is_skipped_and_capture_continues_after_reboot);
  RUN_TEST(test_debug_corruption_and_clear_do_not_return_bad_records);
  return 0;
}
