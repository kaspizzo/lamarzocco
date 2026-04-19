#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "machine_link.h"
#include "machine_link_policy.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"

#define LM_CTRL_MACHINE_STATUS_TEXT_LEN 128
#define LM_CTRL_MACHINE_RESPONSE_MAX 512
#define LM_CTRL_MACHINE_CMD_TIMEOUT_MS 5000
#define LM_CTRL_MACHINE_DISCOVERY_TIMEOUT_MS 15000
#define LM_CTRL_MACHINE_CONNECT_TIMEOUT_MS 15000
#define LM_CTRL_MACHINE_MTU_TIMEOUT_MS 5000
#define LM_CTRL_MACHINE_RETRY_DELAY_MS 1500
#define LM_CTRL_MACHINE_DEBOUNCE_MS 250
#define LM_CTRL_MACHINE_SCAN_TIMEOUT_MS 12000
#define LM_CTRL_MACHINE_WORKER_STACK_SIZE 16384
#define LM_CTRL_MACHINE_RESPONSE_RETRY_COUNT 4
#define LM_CTRL_MACHINE_RESPONSE_RETRY_DELAY_MS 250
#define LM_CTRL_MACHINE_VERIFY_RETRY_COUNT 6
#define LM_CTRL_MACHINE_VERIFY_RETRY_DELAY_MS 500
#define LM_CTRL_MACHINE_BLE_BACKOFF_US (30LL * 1000LL * 1000LL)
#define LM_CTRL_MACHINE_CLOUD_ACK_TIMEOUT_US (12LL * 1000LL * 1000LL)
#define LM_CTRL_MACHINE_WORKER_IDLE_WAIT_MS 500
#define LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS 8
#define LM_CTRL_BLE_VERBOSE_DIAGNOSTICS 0
#define LM_CTRL_MACHINE_FIELD_BLE_MASK   (LM_CTRL_MACHINE_FIELD_TEMPERATURE | LM_CTRL_MACHINE_FIELD_STEAM | LM_CTRL_MACHINE_FIELD_STANDBY)

#define LM_CTRL_MACHINE_UUID_READ "0a0b7847-e12b-09a8-b04b-8e0922a9abab"
#define LM_CTRL_MACHINE_UUID_WRITE "0b0b7847-e12b-09a8-b04b-8e0922a9abab"
#define LM_CTRL_MACHINE_UUID_AUTH "0d0b7847-e12b-09a8-b04b-8e0922a9abab"
#define LM_CTRL_MACHINE_READ_CAPABILITIES "machineCapabilities"
#define LM_CTRL_MACHINE_READ_MACHINE_MODE "machineMode"
#define LM_CTRL_MACHINE_READ_TANK_STATUS "tankStatus"
#define LM_CTRL_MACHINE_READ_BOILERS "boilers"
#define LM_CTRL_MACHINE_READ_SMART_STAND_BY "smartStandBy"
#define LM_CTRL_MACHINE_MODE_BREWING "BrewingMode"
#define LM_CTRL_MACHINE_MODE_STANDBY "StandBy"
#define LM_CTRL_MACHINE_BOILER_COFFEE "CoffeeBoiler1"
#define LM_CTRL_MACHINE_BOILER_STEAM "SteamBoiler"

typedef struct {
  bool initialized;
  bool host_ready;
  bool scanning;
  bool connect_after_scan;
  bool connect_in_progress;
  bool connected;
  bool handles_ready;
  bool authenticated;
  uint16_t conn_handle;
  ble_addr_t candidate_addr;
  ble_addr_t fallback_addr;
  bool fallback_addr_valid;
  uint8_t fallback_matches;
  char target_serial[32];
  char target_token[128];
  ctrl_values_t desired_values;
  ctrl_values_t reported_values;
  ctrl_values_t remote_values;
  lm_ctrl_machine_heat_info_t heat_info;
  uint32_t pending_mask;
  uint32_t inflight_cloud_mask;
  uint32_t loaded_mask;
  uint32_t remote_loaded_mask;
  uint32_t feature_mask;
  char status_text[LM_CTRL_MACHINE_STATUS_TEXT_LEN];
  uint32_t status_version;
  uint16_t read_handle;
  uint16_t write_handle;
  uint16_t auth_handle;
  uint16_t att_mtu;
  uint8_t read_properties;
  uint8_t write_properties;
  uint8_t auth_properties;
  bool diagnostics_logged;
  bool mtu_ready;
  bool cloud_live_updates_active;
  bool cloud_live_updates_connected;
  uint32_t sync_request_flags;
  int64_t last_ble_failure_us;
  ble_uuid_any_t read_uuid;
  ble_uuid_any_t write_uuid;
  ble_uuid_any_t auth_uuid;
  int op_status;
  uint16_t discovered_handle;
  uint16_t discovered_def_handle;
  uint8_t discovered_properties;
  bool op_followup_read;
  uint16_t op_followup_handle;
  char response_buffer[LM_CTRL_MACHINE_RESPONSE_MAX];
  size_t response_length;
  TaskHandle_t worker_task;
  SemaphoreHandle_t conn_sem;
  SemaphoreHandle_t op_sem;
  SemaphoreHandle_t mtu_sem;
} lm_ctrl_machine_link_state_t;

typedef struct {
  bool active;
  char command_id[64];
  uint32_t field_mask;
  int64_t started_us;
  ctrl_values_t sent_values;
  lm_ctrl_cloud_command_status_t last_status;
} lm_ctrl_pending_cloud_command_t;

typedef enum {
  LM_CTRL_CLOUD_SEND_FAILED = 0,
  LM_CTRL_CLOUD_SEND_APPLIED,
  LM_CTRL_CLOUD_SEND_WAITING_FOR_ACK,
} lm_ctrl_cloud_send_result_t;

extern lm_ctrl_machine_link_state_t s_link;
extern lm_ctrl_pending_cloud_command_t s_pending_cloud_commands[LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS];
extern lm_ctrl_machine_link_deps_t s_deps;
extern portMUX_TYPE s_link_lock;

#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
#define BLE_VERBOSE_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define BLE_VERBOSE_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define BLE_VERBOSE_LOGI(...) do { } while (0)
#define BLE_VERBOSE_LOGW(...) do { } while (0)
#endif

void ble_store_config_init(void);
static inline void copy_text(char *dst, size_t dst_size, const char *src) {
  size_t len;

  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  len = 0;
  while ((len + 1) < dst_size && src[len] != '\0') {
    ++len;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static inline bool approx_equal(float a, float b) {
  return fabsf(a - b) < 0.05f;
}

ctrl_steam_level_t snapshot_preferred_steam_level(void);
void set_statusf(const char *fmt, ...);
void clear_connection_state_locked(void);
void snapshot_desired_values(ctrl_values_t *values);
void snapshot_remote_values(ctrl_values_t *values, uint32_t *loaded_mask);
bool snapshot_ble_write_ready(void);
uint32_t snapshot_pending_mask(void);
void mark_field_complete(uint32_t field_mask, const ctrl_values_t *sent_values);
void clear_pending_mask(uint32_t field_mask);
void apply_values_to_reported_locked(const ctrl_values_t *values, uint32_t field_mask, bool *changed);
void mark_fields_inflight(uint32_t field_mask, const ctrl_values_t *sent_values);
int find_pending_cloud_command_slot(const char *command_id);
bool register_pending_cloud_command(const char *command_id, uint32_t field_mask, const ctrl_values_t *sent_values);
void wake_machine_worker(void);
void mark_ble_failure_now(void);
void clear_ble_failure(void);
void update_reported_values(const ctrl_values_t *values, uint32_t loaded_mask);
void update_feature_mask(uint32_t feature_mask);
void update_heat_info(const lm_ctrl_machine_heat_info_t *info);
bool should_skip_ble_attempt(void);

bool fetch_values_via_ble(const lm_ctrl_machine_binding_t *binding, ctrl_values_t *values, uint32_t *loaded_mask);
bool fetch_values_via_cloud(
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  lm_ctrl_machine_heat_info_t *heat_info
);
esp_err_t ensure_connected_and_authenticated(const lm_ctrl_machine_binding_t *binding);
const char *steam_level_cloud_code(ctrl_steam_level_t level);
esp_err_t send_power_command(bool enabled);
esp_err_t send_steam_command(ctrl_steam_level_t level);
esp_err_t send_temperature_command(float temperature_c);
lm_ctrl_cloud_send_result_t send_power_command_cloud(bool enabled);
lm_ctrl_cloud_send_result_t send_steam_command_cloud(ctrl_steam_level_t level);
lm_ctrl_cloud_send_result_t send_temperature_command_cloud(float temperature_c);
esp_err_t send_prebrewing_mode_cloud(const char *mode);
lm_ctrl_cloud_send_result_t send_prebrewing_times_cloud(float infuse_s, float pause_s);
lm_ctrl_cloud_send_result_t send_bbw_mode_cloud(ctrl_bbw_mode_t mode);
lm_ctrl_cloud_send_result_t send_bbw_doses_cloud(float dose_1_g, float dose_2_g);
bool apply_prebrewing_via_cloud(const ctrl_values_t *desired_values, uint32_t pending_mask);
bool apply_bbw_via_cloud(const ctrl_values_t *desired_values, uint32_t pending_mask);
bool apply_pending_via_cloud(const ctrl_values_t *desired_values, uint32_t pending_mask);
void expire_pending_cloud_commands(void);
void machine_link_host_task(void *param);
void machine_link_on_reset(int reason);
void machine_link_on_sync(void);
int ble_gap_event(struct ble_gap_event *event, void *arg);
