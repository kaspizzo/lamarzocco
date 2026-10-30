#include "scale_debug.h"
#include "scale_debug_store.h"
#include "lm_ctrl_nvs_keys.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DEBUG_BYTES (1024U * 1024U)
#define CLOCK_VALID_AFTER INT64_C(1704067200)
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static lm_scale_debug_info_t s_info;
static const esp_partition_t *s_partition;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_store_lock;
static lm_scale_debug_store_t s_store;
static uint32_t s_boot_id, s_generation, s_rate_count;
static uint64_t s_rate_minute;

static bool flash_read(void *ctx, size_t off, void *data, size_t size) {
  const esp_partition_t *p = ctx;
  return esp_partition_read(p, p->size - DEBUG_BYTES + off, data, size) == ESP_OK;
}
static bool flash_write(void *ctx, size_t off, const void *data, size_t size) {
  const esp_partition_t *p = ctx;
  return esp_partition_write(p, p->size - DEBUG_BYTES + off, data, size) == ESP_OK;
}
static bool flash_erase(void *ctx, size_t off, size_t size) {
  const esp_partition_t *p = ctx;
  return esp_partition_erase_range(p, p->size - DEBUG_BYTES + off, size) == ESP_OK;
}
static void publish(const char *status) {
  portENTER_CRITICAL(&s_lock);
  s_info.ready = s_store.ready;
  s_info.retained = s_store.count;
  if (status) snprintf(s_info.status, sizeof(s_info.status), "%s", status);
  portEXIT_CRITICAL(&s_lock);
}
static lm_scale_debug_result_t open_store(bool create) {
  if (s_store.ready) return LM_SCALE_DEBUG_OK;
  lm_scale_debug_io_t io = {.ctx = (void *)s_partition, .size = DEBUG_BYTES,
    .read = flash_read, .write = flash_write, .erase = flash_erase};
  lm_scale_debug_result_t result = lm_scale_debug_store_open(&s_store, &io, create);
  publish(result == LM_SCALE_DEBUG_FOREIGN ? "Storage reservation is not empty; no data overwritten" :
          result == LM_SCALE_DEBUG_IO_ERROR ? "Debug flash read/write failed" :
          result == LM_SCALE_DEBUG_EMPTY ? "No stored debug log" : NULL);
  return result;
}
static lm_scale_debug_record_t make_record(void) {
  portENTER_CRITICAL(&s_lock);
  bool clock_synced = s_info.clock_synced;
  portEXIT_CRITICAL(&s_lock);
  return (lm_scale_debug_record_t){.unix_seconds = clock_synced ? time(NULL) : 0,
    .uptime_ms = esp_timer_get_time() / 1000, .boot_id = s_boot_id};
}
static bool append(lm_scale_debug_record_t *r) {
  if (!lm_scale_debug_store_append(&s_store, r)) {
    s_store.ready = false;
    publish("Debug flash write failed; toggle capture to retry");
    return false;
  }
  publish(NULL);
  return true;
}
static void debug_worker(void *arg) {
  (void)arg;
  uint32_t generation = 0;
  uint32_t dropped_reported = 0;
  uint64_t dropped_minute = UINT64_MAX;
  bool recording = false;
  for (;;) {
    lm_scale_debug_record_t record;
    bool have_record = xQueueReceive(s_queue, &record, pdMS_TO_TICKS(250)) == pdTRUE;
    bool enabled;
    uint32_t current;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_lock);
    enabled = s_info.enabled;
    current = s_generation;
    portEXIT_CRITICAL(&s_lock);
    if (current != generation) {
      generation = current;
      if (enabled && open_store(true) == LM_SCALE_DEBUG_OK) {
        const esp_app_desc_t *app = esp_app_get_description();
        char elf_hash[13];
        for (size_t i = 0; i < 6; ++i) snprintf(elf_hash + i * 2, 3, "%02x", app->app_elf_sha256[i]);
        lm_scale_debug_record_t boot = make_record();
        snprintf(boot.message, sizeof(boot.message), "capture started reset=%d version=%.32s elf=%s",
                 (int)esp_reset_reason(), app->version, elf_hash);
        boot.message_length = strlen(boot.message);
        recording = append(&boot);
        if (recording) publish("Recording scale connection events");
      } else {
        recording = false;
        if (!enabled && !s_store.ready) (void)open_store(false);
        if (!enabled) publish("Capture off; existing log retained");
      }
    }
    /* Recheck under the IO lock so clear/disable cannot leave an old queued
     * event in a freshly cleared log. The BLE host never takes this mutex. */
    portENTER_CRITICAL(&s_lock);
    enabled = s_info.enabled;
    portEXIT_CRITICAL(&s_lock);
    if (have_record && enabled && recording && s_store.ready) recording = append(&record);
    portENTER_CRITICAL(&s_lock);
    uint32_t dropped = s_info.dropped;
    portEXIT_CRITICAL(&s_lock);
    uint64_t minute = (esp_timer_get_time() / 1000) / 60000;
    if (enabled && recording && s_store.ready && dropped != dropped_reported && minute != dropped_minute) {
      lm_scale_debug_record_t loss = make_record();
      snprintf(loss.message, sizeof(loss.message), "capture overload: dropped_this_boot=%lu", (unsigned long)dropped);
      loss.message_length = strlen(loss.message);
      recording = append(&loss);
      dropped_reported = dropped;
      dropped_minute = minute;
    }
    xSemaphoreGive(s_store_lock);
  }
}
esp_err_t lm_scale_debug_init(void) {
  if (s_queue) return ESP_OK;
  s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
  if (!s_partition || s_partition->size < DEBUG_BYTES) {
    snprintf(s_info.status, sizeof(s_info.status), "Debug storage partition unavailable");
    return ESP_ERR_NOT_FOUND;
  }
  uint8_t enabled = 0;
  nvs_handle_t nvs;
  if (nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
    (void)nvs_get_u8(nvs, "scale_debug", &enabled);
    nvs_close(nvs);
  }
  s_info.enabled = enabled == 1;
  s_info.capacity = (DEBUG_BYTES - LM_SCALE_DEBUG_SECTOR) / LM_SCALE_DEBUG_RECORD_SIZE;
  s_boot_id = esp_random();
  s_generation = 1;
  s_store_lock = xSemaphoreCreateMutex();
  s_queue = xQueueCreate(16, sizeof(lm_scale_debug_record_t));
  if (!s_store_lock || !s_queue || xTaskCreate(debug_worker, "lm_scale_debug", 4096, NULL, 2, NULL) != pdPASS) {
    if (s_queue) vQueueDelete(s_queue);
    if (s_store_lock) vSemaphoreDelete(s_store_lock);
    s_queue = NULL;
    s_store_lock = NULL;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
esp_err_t lm_scale_debug_configure(bool enabled) {
  if (!s_queue) return ESP_ERR_INVALID_STATE;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_u8(nvs, "scale_debug", enabled);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err != ESP_OK) return err;
  portENTER_CRITICAL(&s_lock);
  s_info.enabled = enabled;
  ++s_generation;
  portEXIT_CRITICAL(&s_lock);
  return ESP_OK;
}
void lm_scale_debug_get_info(lm_scale_debug_info_t *info) {
  if (!info) return;
  portENTER_CRITICAL(&s_lock);
  *info = s_info;
  portEXIT_CRITICAL(&s_lock);
}
void lm_scale_debug_clock_synchronized(void) {
  bool valid = time(NULL) >= CLOCK_VALID_AFTER;
  portENTER_CRITICAL(&s_lock);
  s_info.clock_synced = valid;
  portEXIT_CRITICAL(&s_lock);
  lm_scale_debug_record("clock synchronized via NTP valid=%u", valid);
}
void lm_scale_debug_record(const char *format, ...) {
  if (!s_queue || !format) return;
  uint64_t minute = (esp_timer_get_time() / 1000) / 60000;
  portENTER_CRITICAL(&s_lock);
  bool accept = s_info.enabled;
  if (s_rate_minute != minute) { s_rate_minute = minute; s_rate_count = 0; }
  if (accept && s_rate_count >= 60) { ++s_info.dropped; accept = false; }
  if (accept) ++s_rate_count;
  portEXIT_CRITICAL(&s_lock);
  if (!accept) return;
  lm_scale_debug_record_t r = make_record();
  va_list args;
  va_start(args, format);
  vsnprintf(r.message, sizeof(r.message), format, args);
  va_end(args);
  for (char *p = r.message; *p; ++p) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
  r.message_length = strlen(r.message);
  if (xQueueSend(s_queue, &r, 0) != pdTRUE) {
    portENTER_CRITICAL(&s_lock);
    ++s_info.dropped;
    portEXIT_CRITICAL(&s_lock);
  }
}
esp_err_t lm_scale_debug_export(lm_scale_debug_export_cb write, void *ctx) {
  if (!s_queue || !write) return ESP_ERR_INVALID_STATE;
  if (xSemaphoreTake(s_store_lock, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
  esp_err_t err = ESP_FAIL;
  if (open_store(false) != LM_SCALE_DEBUG_OK) { err = ESP_ERR_NOT_FOUND; goto done; }
  /* First valid UTC anchor per boot reconstructs pre-NTP events. A bounded
   * map avoids allocating the full log. Without an anchor uptime is retained. */
  struct { uint32_t boot; int64_t epoch; uint64_t uptime; } anchors[64];
  size_t anchor_count = 0;
  lm_scale_debug_record_t r;
  bool valid;
  for (size_t i = 0; i < s_store.capacity; ++i) {
    if (!lm_scale_debug_store_read(&s_store, i, &r, &valid)) goto done;
    if (!valid || r.unix_seconds < CLOCK_VALID_AFTER) continue;
    size_t j = 0;
    while (j < anchor_count && anchors[j].boot != r.boot_id) ++j;
    if (j == anchor_count && anchor_count < 64) {
      anchors[j].boot = r.boot_id;
      anchors[j].epoch = r.unix_seconds;
      anchors[j].uptime = r.uptime_ms;
      ++anchor_count;
    }
  }
  char line[384];
  lm_scale_debug_info_t info;
  lm_scale_debug_get_info(&info);
  snprintf(line, sizeof(line), "# Scale connection log; UTC; oldest to newest\n# retained=%u capacity=%u dropped_this_boot=%u; * = UTC estimated from same boot's NTP anchor\n",
           (unsigned)s_store.count, (unsigned)s_store.capacity, (unsigned)info.dropped);
  if (!write(ctx, line)) goto done;
  for (size_t i = 0; i < s_store.capacity; ++i) {
    if (!lm_scale_debug_store_read(&s_store, i, &r, &valid)) goto done;
    if (!valid) continue;
    /* Keep downloads plain text with one event per line, including records
     * created by an older diagnostic build with non-text metadata. */
    for (size_t j = 0; j < r.message_length; ++j)
      if ((unsigned char)r.message[j] < 32 || (unsigned char)r.message[j] > 126) r.message[j] = '?';
    int64_t epoch = r.unix_seconds;
    bool estimated = false;
    if (epoch < CLOCK_VALID_AFTER) {
      for (size_t j = 0; j < anchor_count; ++j) if (anchors[j].boot == r.boot_id) {
        epoch = anchors[j].epoch + ((int64_t)r.uptime_ms - (int64_t)anchors[j].uptime) / 1000;
        estimated = true;
        break;
      }
    }
    char utc[32] = "UTC-unknown";
    if (epoch >= CLOCK_VALID_AFTER) {
      time_t seconds = epoch;
      struct tm t;
      gmtime_r(&seconds, &t);
      strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &t);
    }
    snprintf(line, sizeof(line), "%s%s seq=%llu boot=%08lx uptime_ms=%llu %s\n",
             utc, estimated ? "*" : "", (unsigned long long)r.sequence,
             (unsigned long)r.boot_id, (unsigned long long)r.uptime_ms, r.message);
    if (!write(ctx, line)) goto done;
  }
  err = ESP_OK;
done:
  xSemaphoreGive(s_store_lock);
  return err;
}
esp_err_t lm_scale_debug_clear(void) {
  if (!s_queue) return ESP_ERR_INVALID_STATE;
  esp_err_t err = lm_scale_debug_configure(false);
  if (err != ESP_OK) return err;
  if (xSemaphoreTake(s_store_lock, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
  lm_scale_debug_result_t result = open_store(false);
  if (result == LM_SCALE_DEBUG_EMPTY) err = ESP_OK;
  else if (result != LM_SCALE_DEBUG_OK) err = ESP_ERR_INVALID_STATE;
  else err = lm_scale_debug_store_clear(&s_store) ? ESP_OK : ESP_FAIL;
  xQueueReset(s_queue);
  publish(err == ESP_OK ? "Capture off; log cleared" : "Could not clear debug log");
  xSemaphoreGive(s_store_lock);
  return err;
}
