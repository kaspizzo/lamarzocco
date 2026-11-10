#include "firmware_update.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "setup_portal_http.h"
#include "wifi_setup.h"
#include "cJSON.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Enable CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE for firmware updates"
#endif

static const char *TAG = "lm_ota";
static int64_t s_health_started, s_health_retry;
static atomic_bool s_boot_confirmed, s_update_committed;
static esp_timer_handle_t s_boot_guard;
/* Placed immediately after esp_app_desc by the IDF linker. Reject older
 * non-OTA applications and incompatible partition-layout ABIs before erase. */
#ifdef ESP_PLATFORM
__attribute__((section(".rodata_custom_desc"), used))
#endif
const char lm_ctrl_firmware_update_abi[16] = "LMCTRL-OTA-V1";
enum { DESC_END = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t),
       PREFIX_BYTES = DESC_END + sizeof(lm_ctrl_firmware_update_abi) };

static void image_id(const esp_app_desc_t *app, char id[65]) {
  for (unsigned i = 0; i < 32; ++i) snprintf(id + i * 2, 3, "%02x", app->app_elf_sha256[i]);
}
static esp_err_t reject(httpd_req_t *req, const char *status, const char *message) {
  httpd_resp_set_hdr(req, "Connection", "close");
  (void)lm_ctrl_setup_portal_send_json_result(req, status, false, message);
  return ESP_FAIL; /* An unread/truncated binary body must not become another request. */
}
static void startup_deadline(void *arg) {
  (void)arg;
  if (!s_boot_confirmed) {
    ESP_LOGE(TAG, "OTA startup was not confirmed within 120 seconds; restarting for rollback");
    esp_restart();
  }
}
esp_err_t lm_ctrl_firmware_boot_begin(void) {
  esp_ota_img_states_t state;
  esp_err_t err = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
  if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED) return ESP_OK;
  if (err != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return err;
  const esp_timer_create_args_t args = {.callback = startup_deadline, .name = "ota_boot_guard"};
  err = esp_timer_create(&args, &s_boot_guard);
  if (err != ESP_OK) return err;
  err = esp_timer_start_once(s_boot_guard, 120000000);
  if (err != ESP_OK) { esp_timer_delete(s_boot_guard); s_boot_guard = NULL; }
  return err;
}
void lm_ctrl_firmware_boot_ready(void) {
  s_health_started = esp_timer_get_time();
  s_health_retry = 0;
  s_boot_confirmed = false;
  s_update_committed = false;
}
void lm_ctrl_firmware_boot_tick(void) {
  int64_t now = esp_timer_get_time();
  if (s_boot_confirmed || !s_health_started || now - s_health_started < 10000000 || now < s_health_retry) return;
  s_health_retry = now + 5000000;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  esp_err_t err = esp_ota_get_state_partition(running, &state);
  if (err == ESP_ERR_NOT_FOUND && running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_MIN) {
    /* Fresh IDF flash with erased otadata: establish ota_0 only after health
     * checks. The migration helper normally provides its initial entry. */
    err = esp_ota_set_boot_partition(running);
    if (err == ESP_OK) { state = ESP_OTA_IMG_NEW; err = esp_ota_mark_app_valid_cancel_rollback(); }
    if (err == ESP_OK) state = ESP_OTA_IMG_VALID;
  }
  if (err == ESP_OK && (state == ESP_OTA_IMG_PENDING_VERIFY || state == ESP_OTA_IMG_UNDEFINED || state == ESP_OTA_IMG_NEW))
    err = esp_ota_mark_app_valid_cancel_rollback();
  else if (err == ESP_OK && state != ESP_OTA_IMG_VALID) err = ESP_ERR_INVALID_STATE;
  if (err == ESP_OK) {
    s_boot_confirmed = true;
    if (s_boot_guard) {
      (void)esp_timer_stop(s_boot_guard);
      (void)esp_timer_delete(s_boot_guard);
      s_boot_guard = NULL;
    }
    ESP_LOGI(TAG, "Boot confirmed after hardware/UI initialization and 10 seconds of main-loop operation");
  } else ESP_LOGE(TAG, "Boot confirmation failed: %s", esp_err_to_name(err));
}
esp_err_t lm_ctrl_firmware_status(httpd_req_t *req) {
  const esp_app_desc_t *app = esp_app_get_description();
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  char id[65];
  image_id(app, id);
  cJSON *json = cJSON_CreateObject();
  if (!json) return reject(req, "500 Internal Server Error", "Out of memory");
  cJSON_AddStringToObject(json, "image", id);
  cJSON_AddStringToObject(json, "version", app->version);
  cJSON_AddStringToObject(json, "slot", running ? running->label : "unknown");
  cJSON_AddBoolToObject(json, "ready", s_boot_confirmed && !s_update_committed);
  cJSON_AddNumberToObject(json, "capacity", next ? next->size : 0);
  char *body = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (!body) return reject(req, "500 Internal Server Error", "Out of memory");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_sendstr(req, body);
  free(body);
  return err;
}
/* The HTTP server serializes these handlers. Stream into the inactive slot,
 * retaining the current image and boot selection until full validation. */
esp_err_t lm_ctrl_firmware_upload(httpd_req_t *req) {
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  const esp_partition_t *running = esp_ota_get_running_partition();
  char content_type[48];
  size_t length = req->content_len;
  if (s_update_committed) return reject(req, "409 Conflict", "A verified update is waiting for restart.");
  if (!s_boot_confirmed) return reject(req, "409 Conflict", "Controller startup check is still pending. Retry shortly.");
  if (!next || !running || next == running || next->address == running->address)
    return reject(req, "409 Conflict", "USB OTA migration is required before wireless updates.");
  if (length < PREFIX_BYTES || length > next->size)
    return reject(req, "400 Bad Request", "Firmware size does not fit the inactive application slot.");
  if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
      strcmp(content_type, "application/octet-stream"))
    return reject(req, "400 Bad Request", "Upload an application .bin file as application/octet-stream.");
  uint8_t *buffer = malloc(4096);
  if (!buffer) return reject(req, "500 Internal Server Error", "Out of memory");
  esp_ota_handle_t handle = 0;
  bool open = false;
  size_t received = 0, prefix = 0;
  int64_t started = esp_timer_get_time();
  esp_err_t err = ESP_OK;
  const char *message = "Firmware upload interrupted or timed out; current firmware retained.";
  while (received < length) {
    if (esp_timer_get_time() - started > 120000000) { err = ESP_ERR_TIMEOUT; break; }
    size_t want = !open ? PREFIX_BYTES - prefix : 4096;
    if (want > length - received) want = length - received;
    int n = httpd_req_recv(req, (char *)buffer + (open ? 0 : prefix), want);
    if (n <= 0) { err = ESP_FAIL; break; }
    received += n;
    if (!open) {
      prefix += n;
      if (prefix < PREFIX_BYTES) continue;
      esp_image_header_t header;
      esp_app_desc_t app;
      memcpy(&header, buffer, sizeof(header));
      memcpy(&app, buffer + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(app));
      if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32S3 || header.hash_appended != 1 ||
          app.magic_word != ESP_APP_DESC_MAGIC_WORD ||
          strncmp(app.project_name, "lamarzocco_knob_controller", sizeof(app.project_name)) ||
          memcmp(buffer + DESC_END, lm_ctrl_firmware_update_abi, sizeof(lm_ctrl_firmware_update_abi))) {
        message = "This is not a compatible ESP32-S3 controller application image.";
        err = ESP_ERR_INVALID_ARG;
        break;
      }
      err = esp_ota_begin(next, length, &handle);
      if (err != ESP_OK) { message = "Could not prepare the inactive application slot."; break; }
      open = true;
      n = prefix;
    }
    err = esp_ota_write(handle, buffer, n);
    if (err != ESP_OK) { message = "Firmware flash write failed; current firmware retained."; break; }
  }
  free(buffer);
  if (err == ESP_OK) {
    err = esp_ota_end(handle); /* Validates image, target revision and appended digest. */
    open = false; /* esp_ota_end releases its handle even on error. */
    message = "Firmware validation failed; current firmware retained.";
  }
  if (open) esp_ota_abort(handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Upload rejected: %s", esp_err_to_name(err));
    return reject(req, "400 Bad Request", message);
  }
  esp_app_desc_t app;
  if (esp_ota_get_partition_description(next, &app) != ESP_OK || esp_ota_set_boot_partition(next) != ESP_OK)
    return reject(req, "500 Internal Server Error", "Could not select the validated firmware for boot.");
  s_update_committed = true;
  bool restarting = lm_ctrl_wifi_schedule_reboot() == ESP_OK;
  char id[65], body[256];
  image_id(&app, id);
  snprintf(body, sizeof(body), "{\"ok\":true,\"restarting\":%s,\"image\":\"%s\",\"message\":\"%s\"}",
    restarting ? "true" : "false", id,
    restarting ? "Firmware verified. Controller is restarting." : "Firmware verified. Use Restart Controller to finish installation.");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, body);
}
