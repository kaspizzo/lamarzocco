/**
 * Machine transport layer for BLE and cloud-backed controller commands.
 *
 * This worker owns machine synchronization, pending writes, BLE connection
 * management, and cloud fallback reads/writes where local BLE is not enough.
 */
#include "machine_link.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "wifi_setup.h"

static const char *TAG = "lm_ble";

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
#define LM_CTRL_MACHINE_ENABLE_CLOUD_FALLBACK 0
#define LM_CTRL_BLE_VERBOSE_DIAGNOSTICS 0
#define LM_CTRL_MACHINE_FIELD_BLE_MASK \
  (LM_CTRL_MACHINE_FIELD_TEMPERATURE | LM_CTRL_MACHINE_FIELD_STEAM | LM_CTRL_MACHINE_FIELD_STANDBY)

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

typedef enum {
  LM_CTRL_DISCOVERY_NONE = 0,
  LM_CTRL_DISCOVERY_READ,
  LM_CTRL_DISCOVERY_WRITE,
  LM_CTRL_DISCOVERY_AUTH,
} lm_ctrl_discovery_target_t;

typedef enum {
  LM_CTRL_ADV_MATCH_NONE = 0,
  LM_CTRL_ADV_MATCH_FALLBACK,
  LM_CTRL_ADV_MATCH_EXACT,
} lm_ctrl_adv_match_t;

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
  uint32_t pending_mask;
  uint32_t inflight_cloud_mask;
  uint32_t loaded_mask;
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
  uint32_t sync_request_flags;
  int64_t last_ble_failure_us;
  ble_uuid_any_t read_uuid;
  ble_uuid_any_t write_uuid;
  ble_uuid_any_t auth_uuid;
  lm_ctrl_discovery_target_t discovery_target;
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

static lm_ctrl_machine_link_state_t s_link = {
  .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};
static lm_ctrl_pending_cloud_command_t s_pending_cloud_commands[LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS];

static portMUX_TYPE s_link_lock = portMUX_INITIALIZER_UNLOCKED;

#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
#define BLE_VERBOSE_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define BLE_VERBOSE_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define BLE_VERBOSE_LOGI(...) do { } while (0)
#define BLE_VERBOSE_LOGW(...) do { } while (0)
#endif

typedef enum {
  LM_CTRL_CLOUD_SEND_FAILED = 0,
  LM_CTRL_CLOUD_SEND_APPLIED,
  LM_CTRL_CLOUD_SEND_WAITING_FOR_ACK,
} lm_ctrl_cloud_send_result_t;

void ble_store_config_init(void);
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static esp_err_t ensure_connected_and_authenticated(const lm_ctrl_machine_binding_t *binding);
static lm_ctrl_cloud_send_result_t send_power_command_cloud(bool enabled);
static lm_ctrl_cloud_send_result_t send_steam_command_cloud(bool enabled);
static lm_ctrl_cloud_send_result_t send_temperature_command_cloud(float temperature_c);
static esp_err_t send_prebrewing_mode_cloud(const char *mode);
static lm_ctrl_cloud_send_result_t send_prebrewing_times_cloud(float infuse_s, float pause_s);
static lm_ctrl_cloud_send_result_t send_bbw_mode_cloud(ctrl_bbw_mode_t mode);
static lm_ctrl_cloud_send_result_t send_bbw_doses_cloud(float dose_1_g, float dose_2_g);
static void expire_pending_cloud_commands(void);

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  snprintf(dst, dst_size, "%s", src);
}

static bool approx_equal(float a, float b) {
  return fabsf(a - b) < 0.05f;
}

static bool addr_equal(const ble_addr_t *left, const ble_addr_t *right) {
  if (left == NULL || right == NULL) {
    return false;
  }
  return left->type == right->type && memcmp(left->val, right->val, sizeof(left->val)) == 0;
}

static void set_statusf(const char *fmt, ...) {
  char buffer[LM_CTRL_MACHINE_STATUS_TEXT_LEN];
  va_list args;

  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  portENTER_CRITICAL(&s_link_lock);
  copy_text(s_link.status_text, sizeof(s_link.status_text), buffer);
  s_link.status_version++;
  portEXIT_CRITICAL(&s_link_lock);

  ESP_LOGI(TAG, "%s", buffer);
}

static void clear_connection_state_locked(void) {
  s_link.scanning = false;
  s_link.connect_after_scan = false;
  s_link.connect_in_progress = false;
  s_link.connected = false;
  s_link.handles_ready = false;
  s_link.authenticated = false;
  s_link.conn_handle = BLE_HS_CONN_HANDLE_NONE;
  s_link.read_handle = 0;
  s_link.write_handle = 0;
  s_link.auth_handle = 0;
  s_link.att_mtu = BLE_ATT_MTU_DFLT;
  s_link.read_properties = 0;
  s_link.write_properties = 0;
  s_link.auth_properties = 0;
  s_link.mtu_ready = false;
  s_link.discovered_def_handle = 0;
  s_link.discovered_properties = 0;
}

static void snapshot_desired_values(ctrl_values_t *values) {
  if (values == NULL) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  *values = s_link.desired_values;
  portEXIT_CRITICAL(&s_link_lock);
}

static uint32_t snapshot_pending_mask(void) {
  uint32_t pending_mask;

  portENTER_CRITICAL(&s_link_lock);
  pending_mask = s_link.pending_mask & ~s_link.inflight_cloud_mask;
  portEXIT_CRITICAL(&s_link_lock);

  return pending_mask;
}

static void mark_field_complete(uint32_t field_mask, const ctrl_values_t *sent_values) {
  bool needs_wakeup = false;

  portENTER_CRITICAL(&s_link_lock);
  if ((field_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      approx_equal(s_link.desired_values.temperature_c, sent_values->temperature_c)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_TEMPERATURE;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      s_link.desired_values.steam_on == sent_values->steam_on) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_STEAM;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      s_link.desired_values.standby_on == sent_values->standby_on) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_STANDBY;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      approx_equal(s_link.desired_values.infuse_s, sent_values->infuse_s)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_INFUSE;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      approx_equal(s_link.desired_values.pause_s, sent_values->pause_s)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_PAUSE;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      s_link.desired_values.bbw_mode == sent_values->bbw_mode) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_MODE;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      approx_equal(s_link.desired_values.bbw_dose_1_g, sent_values->bbw_dose_1_g)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_DOSE_1;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      approx_equal(s_link.desired_values.bbw_dose_2_g, sent_values->bbw_dose_2_g)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
  }
  needs_wakeup = s_link.pending_mask != 0;
  portEXIT_CRITICAL(&s_link_lock);

  if (needs_wakeup && s_link.worker_task != NULL) {
    xTaskNotifyGive(s_link.worker_task);
  }
}

static void clear_pending_mask(uint32_t field_mask) {
  portENTER_CRITICAL(&s_link_lock);
  s_link.pending_mask &= ~field_mask;
  portEXIT_CRITICAL(&s_link_lock);
}

static void apply_values_to_reported_locked(const ctrl_values_t *values, uint32_t field_mask, bool *changed) {
  if (values == NULL) {
    return;
  }

  if ((field_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      !approx_equal(s_link.reported_values.temperature_c, values->temperature_c)) {
    s_link.reported_values.temperature_c = values->temperature_c;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      !approx_equal(s_link.reported_values.infuse_s, values->infuse_s)) {
    s_link.reported_values.infuse_s = values->infuse_s;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      !approx_equal(s_link.reported_values.pause_s, values->pause_s)) {
    s_link.reported_values.pause_s = values->pause_s;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      s_link.reported_values.steam_on != values->steam_on) {
    s_link.reported_values.steam_on = values->steam_on;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      s_link.reported_values.standby_on != values->standby_on) {
    s_link.reported_values.standby_on = values->standby_on;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      s_link.reported_values.bbw_mode != values->bbw_mode) {
    s_link.reported_values.bbw_mode = values->bbw_mode;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      !approx_equal(s_link.reported_values.bbw_dose_1_g, values->bbw_dose_1_g)) {
    s_link.reported_values.bbw_dose_1_g = values->bbw_dose_1_g;
    *changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      !approx_equal(s_link.reported_values.bbw_dose_2_g, values->bbw_dose_2_g)) {
    s_link.reported_values.bbw_dose_2_g = values->bbw_dose_2_g;
    *changed = true;
  }
  if ((s_link.loaded_mask & field_mask) != field_mask) {
    s_link.loaded_mask |= field_mask;
    *changed = true;
  }
}

static bool should_accept_remote_float_locked(uint32_t field_mask, float incoming_value) {
  const uint32_t protected_mask = s_link.pending_mask | s_link.inflight_cloud_mask;

  if ((protected_mask & field_mask) == 0) {
    return true;
  }

  switch (field_mask) {
    case LM_CTRL_MACHINE_FIELD_TEMPERATURE:
      return approx_equal(s_link.desired_values.temperature_c, incoming_value);
    case LM_CTRL_MACHINE_FIELD_INFUSE:
      return approx_equal(s_link.desired_values.infuse_s, incoming_value);
    case LM_CTRL_MACHINE_FIELD_PAUSE:
      return approx_equal(s_link.desired_values.pause_s, incoming_value);
    case LM_CTRL_MACHINE_FIELD_BBW_DOSE_1:
      return approx_equal(s_link.desired_values.bbw_dose_1_g, incoming_value);
    case LM_CTRL_MACHINE_FIELD_BBW_DOSE_2:
      return approx_equal(s_link.desired_values.bbw_dose_2_g, incoming_value);
    default:
      return true;
  }
}

static bool should_accept_remote_bool_locked(uint32_t field_mask, bool incoming_value) {
  const uint32_t protected_mask = s_link.pending_mask | s_link.inflight_cloud_mask;

  if ((protected_mask & field_mask) == 0) {
    return true;
  }

  switch (field_mask) {
    case LM_CTRL_MACHINE_FIELD_STEAM:
      return s_link.desired_values.steam_on == incoming_value;
    case LM_CTRL_MACHINE_FIELD_STANDBY:
      return s_link.desired_values.standby_on == incoming_value;
    default:
      return true;
  }
}

static bool should_accept_remote_int_locked(uint32_t field_mask, int incoming_value) {
  const uint32_t protected_mask = s_link.pending_mask | s_link.inflight_cloud_mask;

  if ((protected_mask & field_mask) == 0) {
    return true;
  }

  if (field_mask == LM_CTRL_MACHINE_FIELD_BBW_MODE) {
    return (int)s_link.desired_values.bbw_mode == incoming_value;
  }
  return true;
}

static void mark_fields_inflight(uint32_t field_mask, const ctrl_values_t *sent_values) {
  bool changed = false;

  if (sent_values == NULL || field_mask == 0) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  if ((field_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      approx_equal(s_link.desired_values.temperature_c, sent_values->temperature_c)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_TEMPERATURE;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_TEMPERATURE;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      s_link.desired_values.steam_on == sent_values->steam_on) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_STEAM;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_STEAM;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      s_link.desired_values.standby_on == sent_values->standby_on) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_STANDBY;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_STANDBY;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      approx_equal(s_link.desired_values.infuse_s, sent_values->infuse_s)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_INFUSE;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_INFUSE;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      approx_equal(s_link.desired_values.pause_s, sent_values->pause_s)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_PAUSE;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_PAUSE;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      s_link.desired_values.bbw_mode == sent_values->bbw_mode) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_MODE;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_BBW_MODE;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      approx_equal(s_link.desired_values.bbw_dose_1_g, sent_values->bbw_dose_1_g)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_DOSE_1;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_BBW_DOSE_1;
    changed = true;
  }
  if ((field_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      (s_link.pending_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      approx_equal(s_link.desired_values.bbw_dose_2_g, sent_values->bbw_dose_2_g)) {
    s_link.pending_mask &= ~LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
    s_link.inflight_cloud_mask |= LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
    changed = true;
  }
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);
}

static int find_pending_cloud_command_slot(const char *command_id) {
  size_t index;

  if (command_id == NULL || command_id[0] == '\0') {
    return -1;
  }

  for (index = 0; index < LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS; ++index) {
    if (s_pending_cloud_commands[index].active &&
        strcmp(s_pending_cloud_commands[index].command_id, command_id) == 0) {
      return (int)index;
    }
  }
  return -1;
}

static int allocate_pending_cloud_command_slot(void) {
  size_t index;

  for (index = 0; index < LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS; ++index) {
    if (!s_pending_cloud_commands[index].active) {
      return (int)index;
    }
  }
  return -1;
}

static bool register_pending_cloud_command(const char *command_id, uint32_t field_mask, const ctrl_values_t *sent_values) {
  int slot;

  if (command_id == NULL || command_id[0] == '\0' || sent_values == NULL || field_mask == 0) {
    return false;
  }

  slot = find_pending_cloud_command_slot(command_id);
  if (slot < 0) {
    slot = allocate_pending_cloud_command_slot();
  }
  if (slot < 0) {
    set_statusf("Cloud command accepted, but no pending slot was available.");
    return false;
  }

  s_pending_cloud_commands[slot].active = true;
  copy_text(s_pending_cloud_commands[slot].command_id, sizeof(s_pending_cloud_commands[slot].command_id), command_id);
  s_pending_cloud_commands[slot].field_mask = field_mask;
  s_pending_cloud_commands[slot].started_us = esp_timer_get_time();
  s_pending_cloud_commands[slot].sent_values = *sent_values;
  s_pending_cloud_commands[slot].last_status = LM_CTRL_CLOUD_COMMAND_STATUS_PENDING;

  mark_fields_inflight(field_mask, sent_values);
  return true;
}

static void wake_machine_worker(void) {
  if (s_link.worker_task != NULL) {
    xTaskNotifyGive(s_link.worker_task);
  }
}

static void mark_ble_failure_now(void) {
  portENTER_CRITICAL(&s_link_lock);
  s_link.last_ble_failure_us = esp_timer_get_time();
  portEXIT_CRITICAL(&s_link_lock);
}

static void clear_ble_failure(void) {
  portENTER_CRITICAL(&s_link_lock);
  s_link.last_ble_failure_us = 0;
  portEXIT_CRITICAL(&s_link_lock);
}

static void update_reported_values(const ctrl_values_t *values, uint32_t loaded_mask) {
  bool changed = false;

  if (values == NULL || loaded_mask == 0) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0 &&
      should_accept_remote_float_locked(LM_CTRL_MACHINE_FIELD_TEMPERATURE, values->temperature_c) &&
      !approx_equal(s_link.reported_values.temperature_c, values->temperature_c)) {
    s_link.reported_values.temperature_c = values->temperature_c;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0 &&
      should_accept_remote_float_locked(LM_CTRL_MACHINE_FIELD_INFUSE, values->infuse_s) &&
      !approx_equal(s_link.reported_values.infuse_s, values->infuse_s)) {
    s_link.reported_values.infuse_s = values->infuse_s;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0 &&
      should_accept_remote_float_locked(LM_CTRL_MACHINE_FIELD_PAUSE, values->pause_s) &&
      !approx_equal(s_link.reported_values.pause_s, values->pause_s)) {
    s_link.reported_values.pause_s = values->pause_s;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0 &&
      should_accept_remote_bool_locked(LM_CTRL_MACHINE_FIELD_STEAM, values->steam_on) &&
      s_link.reported_values.steam_on != values->steam_on) {
    s_link.reported_values.steam_on = values->steam_on;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0 &&
      should_accept_remote_bool_locked(LM_CTRL_MACHINE_FIELD_STANDBY, values->standby_on) &&
      s_link.reported_values.standby_on != values->standby_on) {
    s_link.reported_values.standby_on = values->standby_on;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0 &&
      should_accept_remote_int_locked(LM_CTRL_MACHINE_FIELD_BBW_MODE, (int)values->bbw_mode) &&
      s_link.reported_values.bbw_mode != values->bbw_mode) {
    s_link.reported_values.bbw_mode = values->bbw_mode;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 &&
      should_accept_remote_float_locked(LM_CTRL_MACHINE_FIELD_BBW_DOSE_1, values->bbw_dose_1_g) &&
      !approx_equal(s_link.reported_values.bbw_dose_1_g, values->bbw_dose_1_g)) {
    s_link.reported_values.bbw_dose_1_g = values->bbw_dose_1_g;
    changed = true;
  }
  if ((loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 &&
      should_accept_remote_float_locked(LM_CTRL_MACHINE_FIELD_BBW_DOSE_2, values->bbw_dose_2_g) &&
      !approx_equal(s_link.reported_values.bbw_dose_2_g, values->bbw_dose_2_g)) {
    s_link.reported_values.bbw_dose_2_g = values->bbw_dose_2_g;
    changed = true;
  }
  if ((s_link.loaded_mask & loaded_mask) != loaded_mask) {
    s_link.loaded_mask |= loaded_mask;
    changed = true;
  }
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);
}

static void update_feature_mask(uint32_t feature_mask) {
  bool changed = false;

  portENTER_CRITICAL(&s_link_lock);
  if (s_link.feature_mask != feature_mask) {
    s_link.feature_mask = feature_mask;
    changed = true;
  }
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);
}

static bool should_skip_ble_attempt(void) {
  int64_t last_failure_us;

  portENTER_CRITICAL(&s_link_lock);
  last_failure_us = s_link.last_ble_failure_us;
  portEXIT_CRITICAL(&s_link_lock);

  return last_failure_us > 0 && (esp_timer_get_time() - last_failure_us) < LM_CTRL_MACHINE_BLE_BACKOFF_US;
}

static lm_ctrl_adv_match_t advertisement_match_type(
  const struct ble_gap_disc_desc *disc,
  char *matched_name,
  size_t matched_name_size
) {
  struct ble_hs_adv_fields fields = {0};
  char local_name[40];
  char target_serial[sizeof(s_link.target_serial)];
  size_t name_len;
  bool is_lm_model = false;
  bool has_serial = false;

  if (disc == NULL || matched_name == NULL || matched_name_size == 0) {
    return LM_CTRL_ADV_MATCH_NONE;
  }

  portENTER_CRITICAL(&s_link_lock);
  copy_text(target_serial, sizeof(target_serial), s_link.target_serial);
  portEXIT_CRITICAL(&s_link_lock);
  if (target_serial[0] == '\0') {
    return LM_CTRL_ADV_MATCH_NONE;
  }

  if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0 || fields.name_len == 0) {
    return LM_CTRL_ADV_MATCH_NONE;
  }

  name_len = fields.name_len;
  if (name_len >= sizeof(local_name)) {
    name_len = sizeof(local_name) - 1;
  }
  memcpy(local_name, fields.name, name_len);
  local_name[name_len] = '\0';

  is_lm_model = strncmp(local_name, "MICRA", 5) == 0 ||
                strncmp(local_name, "MINI", 4) == 0 ||
                strncmp(local_name, "GS3", 3) == 0;
  has_serial = strstr(local_name, target_serial) != NULL;

  if (!is_lm_model && !has_serial) {
    return LM_CTRL_ADV_MATCH_NONE;
  }

  copy_text(matched_name, matched_name_size, local_name);
  return has_serial ? LM_CTRL_ADV_MATCH_EXACT : LM_CTRL_ADV_MATCH_FALLBACK;
}

static void finish_op(int status) {
  portENTER_CRITICAL(&s_link_lock);
  s_link.op_status = status;
  portEXIT_CRITICAL(&s_link_lock);

  if (s_link.op_sem != NULL) {
    xSemaphoreGive(s_link.op_sem);
  }
}

static int gatt_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
  size_t chunk_len = 0;
  char chunk[LM_CTRL_MACHINE_RESPONSE_MAX] = {0};
  int status = error != NULL ? error->status : BLE_HS_EAPP;
  (void)conn_handle;
  (void)arg;

  if (status == 0) {
    if (attr != NULL && attr->om != NULL) {
      size_t existing_len;
      size_t copy_len;

      chunk_len = OS_MBUF_PKTLEN(attr->om);
      if (chunk_len >= sizeof(chunk)) {
        chunk_len = sizeof(chunk) - 1;
      }
      if (os_mbuf_copydata(attr->om, 0, chunk_len, chunk) != 0) {
        finish_op(BLE_HS_EAPP);
        return 0;
      }

      chunk[chunk_len] = '\0';

      portENTER_CRITICAL(&s_link_lock);
      existing_len = s_link.response_length;
      if (existing_len >= sizeof(s_link.response_buffer)) {
        existing_len = sizeof(s_link.response_buffer) - 1;
      }
      copy_len = chunk_len;
      if (existing_len + copy_len >= sizeof(s_link.response_buffer)) {
        copy_len = sizeof(s_link.response_buffer) - 1 - existing_len;
      }
      memcpy(s_link.response_buffer + existing_len, chunk, copy_len);
      s_link.response_length = existing_len + copy_len;
      s_link.response_buffer[s_link.response_length] = '\0';
      portEXIT_CRITICAL(&s_link_lock);

      BLE_VERBOSE_LOGI(
        "BLE read callback status=%d handle=0x%04x len=%u",
        status,
        attr->handle,
        (unsigned)chunk_len
      );
      if (chunk_len > 0) {
        if (LM_CTRL_BLE_VERBOSE_DIAGNOSTICS) {
          ESP_LOG_BUFFER_HEX_LEVEL(TAG, chunk, chunk_len, ESP_LOG_INFO);
        }
        if (chunk[0] != '\0') {
          BLE_VERBOSE_LOGI("BLE read text: %s", chunk);
        }
      }
    }
    return 0;
  }

  if (status == BLE_HS_EDONE) {
#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
    size_t response_len;

    portENTER_CRITICAL(&s_link_lock);
    response_len = s_link.response_length;
    portEXIT_CRITICAL(&s_link_lock);

    BLE_VERBOSE_LOGI("BLE read complete handle=0x%04x len=%u", attr != NULL ? attr->handle : 0, (unsigned)response_len);
#endif
    finish_op(0);
    return 0;
  }

  finish_op(status);
  return 0;
}

static int gatt_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
  bool followup_read = false;
  uint16_t followup_handle = 0;
  int status = error != NULL ? error->status : BLE_HS_EAPP;
  (void)attr;
  (void)arg;

  if (status != 0) {
    finish_op(status);
    return 0;
  }

  portENTER_CRITICAL(&s_link_lock);
  followup_read = s_link.op_followup_read;
  followup_handle = s_link.op_followup_handle;
  portEXIT_CRITICAL(&s_link_lock);

  if (!followup_read) {
    finish_op(0);
    return 0;
  }

  status = ble_gattc_read_long(conn_handle, followup_handle, 0, gatt_read_cb, NULL);
  if (status != 0) {
    finish_op(status);
  }
  return 0;
}

static int gatt_mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg) {
  int status = error != NULL ? error->status : BLE_HS_EAPP;
  (void)conn_handle;
  (void)arg;

  portENTER_CRITICAL(&s_link_lock);
  s_link.op_status = status;
  if (status == 0 && mtu >= BLE_ATT_MTU_DFLT) {
    s_link.att_mtu = mtu;
    s_link.mtu_ready = true;
  }
  portEXIT_CRITICAL(&s_link_lock);

  if (s_link.mtu_sem != NULL) {
    xSemaphoreGive(s_link.mtu_sem);
  }
  return 0;
}

static int gatt_discovery_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg) {
  int status = error != NULL ? error->status : BLE_HS_EAPP;
  (void)conn_handle;
  (void)arg;

  if (status == 0 && chr != NULL) {
    portENTER_CRITICAL(&s_link_lock);
    s_link.discovered_handle = chr->val_handle;
    s_link.discovered_def_handle = chr->def_handle;
    s_link.discovered_properties = chr->properties;
    portEXIT_CRITICAL(&s_link_lock);
    return 0;
  }

  if (status == BLE_HS_EDONE) {
    uint16_t discovered_handle;

    portENTER_CRITICAL(&s_link_lock);
    discovered_handle = s_link.discovered_handle;
#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
    uint16_t discovered_def_handle = s_link.discovered_def_handle;
    uint8_t discovered_properties = s_link.discovered_properties;
#endif
    portEXIT_CRITICAL(&s_link_lock);

    finish_op(discovered_handle != 0 ? 0 : BLE_HS_ENOENT);
#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
    if (discovered_handle != 0) {
      BLE_VERBOSE_LOGI(
        "Resolved BLE characteristic handle=0x%04x def=0x%04x props=0x%02x",
        discovered_handle,
        discovered_def_handle,
        discovered_properties
      );
    }
#endif
    return 0;
  }

  finish_op(status);
  return 0;
}

static esp_err_t wait_for_sem(SemaphoreHandle_t sem, TickType_t ticks_to_wait, const char *timeout_message) {
  if (xSemaphoreTake(sem, ticks_to_wait) == pdTRUE) {
    return ESP_OK;
  }

  set_statusf("%s", timeout_message);
  return ESP_ERR_TIMEOUT;
}

static bool has_cloud_priority_pending_work(void) {
  bool has_pending = false;

  portENTER_CRITICAL(&s_link_lock);
  has_pending = (s_link.pending_mask & (LM_CTRL_MACHINE_FIELD_PREBREWING | LM_CTRL_MACHINE_FIELD_BBW)) != 0;
  portEXIT_CRITICAL(&s_link_lock);

  return has_pending;
}

static esp_err_t wait_for_conn_sem_interruptible(TickType_t ticks_to_wait, const char *timeout_message) {
  const TickType_t poll_ticks = pdMS_TO_TICKS(250);
  TickType_t waited_ticks = 0;

  while (waited_ticks < ticks_to_wait) {
    TickType_t remaining_ticks = ticks_to_wait - waited_ticks;
    TickType_t step_ticks = remaining_ticks < poll_ticks ? remaining_ticks : poll_ticks;

    if (xSemaphoreTake(s_link.conn_sem, step_ticks) == pdTRUE) {
      return ESP_OK;
    }

    waited_ticks += step_ticks;

    if (ulTaskNotifyTake(pdTRUE, 0) > 0 && has_cloud_priority_pending_work()) {
      bool scanning = false;
      bool connect_in_progress = false;

      portENTER_CRITICAL(&s_link_lock);
      scanning = s_link.scanning;
      connect_in_progress = s_link.connect_in_progress;
      portEXIT_CRITICAL(&s_link_lock);

      if (scanning) {
        (void)ble_gap_disc_cancel();
      } else if (connect_in_progress) {
        (void)ble_gap_conn_cancel();
      }

      set_statusf("Prioritizing cloud updates before BLE reconnect.");
      return ESP_ERR_NOT_FINISHED;
    }
  }

  set_statusf("%s", timeout_message);
  return ESP_ERR_TIMEOUT;
}

static esp_err_t run_read_operation(uint16_t handle, char *response, size_t response_size, const char *timeout_message) {
  uint16_t conn_handle;
  int rc;
  int op_status;

  while (xSemaphoreTake(s_link.op_sem, 0) == pdTRUE) {
  }

  portENTER_CRITICAL(&s_link_lock);
  conn_handle = s_link.conn_handle;
  s_link.op_followup_read = false;
  s_link.op_followup_handle = 0;
  s_link.op_status = BLE_HS_EAPP;
  s_link.response_buffer[0] = '\0';
  s_link.response_length = 0;
  portEXIT_CRITICAL(&s_link_lock);

  rc = ble_gattc_read_long(conn_handle, handle, 0, gatt_read_cb, NULL);
  if (rc != 0) {
    set_statusf("BLE read start failed (rc=%d).", rc);
    return ESP_FAIL;
  }

  ESP_RETURN_ON_ERROR(
    wait_for_sem(s_link.op_sem, pdMS_TO_TICKS(LM_CTRL_MACHINE_CMD_TIMEOUT_MS), timeout_message),
    TAG,
    "BLE read wait failed"
  );

  portENTER_CRITICAL(&s_link_lock);
  op_status = s_link.op_status;
  if (response != NULL && response_size > 0) {
    copy_text(response, response_size, s_link.response_buffer);
  }
  portEXIT_CRITICAL(&s_link_lock);

  if (op_status != 0) {
    set_statusf("BLE read failed (status=%d).", op_status);
    return ESP_FAIL;
  }

  return ESP_OK;
}

static esp_err_t ensure_ble_mtu(void) {
  uint16_t conn_handle;
  uint16_t att_mtu;
  bool mtu_ready;
  int rc;
  int op_status;

  portENTER_CRITICAL(&s_link_lock);
  conn_handle = s_link.conn_handle;
  att_mtu = s_link.att_mtu;
  mtu_ready = s_link.mtu_ready;
  portEXIT_CRITICAL(&s_link_lock);

  if (mtu_ready && att_mtu > BLE_ATT_MTU_DFLT) {
    return ESP_OK;
  }

  while (xSemaphoreTake(s_link.mtu_sem, 0) == pdTRUE) {
  }

  portENTER_CRITICAL(&s_link_lock);
  s_link.op_status = BLE_HS_EAPP;
  portEXIT_CRITICAL(&s_link_lock);

  rc = ble_gattc_exchange_mtu(conn_handle, gatt_mtu_cb, NULL);
  if (rc != 0) {
    set_statusf("BLE MTU exchange start failed (rc=%d).", rc);
    return ESP_FAIL;
  }

  ESP_RETURN_ON_ERROR(
    wait_for_sem(s_link.mtu_sem, pdMS_TO_TICKS(LM_CTRL_MACHINE_MTU_TIMEOUT_MS), "BLE MTU exchange timed out."),
    TAG,
    "BLE MTU wait failed"
  );

  portENTER_CRITICAL(&s_link_lock);
  op_status = s_link.op_status;
  att_mtu = s_link.att_mtu;
  portEXIT_CRITICAL(&s_link_lock);

  if (op_status != 0) {
    set_statusf("BLE MTU exchange failed (status=%d).", op_status);
    return ESP_FAIL;
  }

  set_statusf("BLE MTU ready: %u", (unsigned)att_mtu);
  return ESP_OK;
}

static uint8_t get_handle_properties_locked(uint16_t handle) {
  if (handle == s_link.read_handle) {
    return s_link.read_properties;
  }
  if (handle == s_link.write_handle) {
    return s_link.write_properties;
  }
  if (handle == s_link.auth_handle) {
    return s_link.auth_properties;
  }
  return 0;
}

static esp_err_t read_followup_response_with_retries(uint16_t response_handle, char *response, size_t response_size) {
  if (response == NULL || response_size == 0 || response_handle == 0) {
    return ESP_OK;
  }

  for (int attempt = 0; attempt <= LM_CTRL_MACHINE_RESPONSE_RETRY_COUNT; ++attempt) {
    if (attempt > 0) {
      vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_RESPONSE_RETRY_DELAY_MS));
    }
    if (run_read_operation(response_handle, response, response_size, "BLE response read timed out.") != ESP_OK) {
      return ESP_FAIL;
    }
    if (response[0] != '\0') {
      if (attempt > 0) {
        BLE_VERBOSE_LOGI("Received deferred BLE command response on retry %d", attempt);
      }
      break;
    }
  }

  return ESP_OK;
}

static esp_err_t run_write_operation(uint16_t handle, const void *data, size_t data_len, bool followup_read, uint16_t response_handle, char *response, size_t response_size) {
  uint16_t conn_handle;
  uint8_t properties;
  bool use_write_no_rsp;
  bool use_write_long;
  uint16_t att_mtu;
  uint16_t payload_limit;
  int rc;
  int op_status;

  while (xSemaphoreTake(s_link.op_sem, 0) == pdTRUE) {
  }

  portENTER_CRITICAL(&s_link_lock);
  conn_handle = s_link.conn_handle;
  properties = get_handle_properties_locked(handle);
  s_link.op_followup_read = followup_read;
  s_link.op_followup_handle = response_handle;
  s_link.op_status = BLE_HS_EAPP;
  s_link.response_buffer[0] = '\0';
  s_link.response_length = 0;
  portEXIT_CRITICAL(&s_link_lock);

  att_mtu = ble_att_mtu(conn_handle);
  if (att_mtu < BLE_ATT_MTU_DFLT) {
    att_mtu = BLE_ATT_MTU_DFLT;
  }
  payload_limit = att_mtu > 3 ? att_mtu - 3 : 20;
  use_write_no_rsp = (properties & BLE_GATT_CHR_F_WRITE_NO_RSP) != 0 &&
                     (properties & BLE_GATT_CHR_F_WRITE) == 0;
  use_write_long = !use_write_no_rsp && data_len > payload_limit;
  if (use_write_no_rsp) {
    BLE_VERBOSE_LOGI("Using BLE write without response for handle 0x%04x", handle);
    rc = ble_gattc_write_no_rsp_flat(conn_handle, handle, data, data_len);
    if (rc != 0) {
      set_statusf("BLE write(no-rsp) start failed (rc=%d).", rc);
      return ESP_FAIL;
    }

    if (!followup_read) {
      return ESP_OK;
    }

    vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_RESPONSE_RETRY_DELAY_MS));
    return read_followup_response_with_retries(response_handle, response, response_size);
  }

  if (use_write_long) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, data_len);

    if (om == NULL) {
      set_statusf("BLE write(long) alloc failed.");
      return ESP_ERR_NO_MEM;
    }

    BLE_VERBOSE_LOGI(
      "Using BLE long write for handle 0x%04x len=%u mtu=%u",
      handle,
      (unsigned)data_len,
      (unsigned)att_mtu
    );
    rc = ble_gattc_write_long(conn_handle, handle, 0, om, gatt_write_cb, NULL);
  } else {
    rc = ble_gattc_write_flat(conn_handle, handle, data, data_len, gatt_write_cb, NULL);
  }
  if (rc != 0) {
    set_statusf("BLE write start failed (rc=%d).", rc);
    return ESP_FAIL;
  }

  ESP_RETURN_ON_ERROR(
    wait_for_sem(s_link.op_sem, pdMS_TO_TICKS(LM_CTRL_MACHINE_CMD_TIMEOUT_MS), "BLE command timed out."),
    TAG,
    "BLE write wait failed"
  );

  portENTER_CRITICAL(&s_link_lock);
  op_status = s_link.op_status;
  if (response != NULL && response_size > 0) {
    copy_text(response, response_size, s_link.response_buffer);
  }
  portEXIT_CRITICAL(&s_link_lock);

  if (op_status != 0) {
    set_statusf("BLE command failed (status=%d).", op_status);
    return ESP_FAIL;
  }

  if (followup_read && response != NULL && response_size > 0 && response[0] == '\0') {
    ESP_RETURN_ON_ERROR(
      read_followup_response_with_retries(response_handle, response, response_size),
      TAG,
      "BLE deferred response read failed"
    );
  }

  return ESP_OK;
}

static esp_err_t discover_characteristic_handle(
  const ble_uuid_t *uuid,
  lm_ctrl_discovery_target_t target,
  uint16_t *out_handle,
  uint8_t *out_properties
) {
  uint16_t conn_handle;
  int rc;
  int op_status;
  uint16_t discovered_handle;
  uint8_t discovered_properties;

  if (out_handle == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  while (xSemaphoreTake(s_link.op_sem, 0) == pdTRUE) {
  }

  portENTER_CRITICAL(&s_link_lock);
  conn_handle = s_link.conn_handle;
  s_link.discovery_target = target;
  s_link.discovered_handle = 0;
  s_link.discovered_def_handle = 0;
  s_link.discovered_properties = 0;
  s_link.op_status = BLE_HS_EAPP;
  portEXIT_CRITICAL(&s_link_lock);

  rc = ble_gattc_disc_chrs_by_uuid(conn_handle, 1, 0xffff, uuid, gatt_discovery_cb, NULL);
  if (rc != 0) {
    set_statusf("BLE discovery start failed (rc=%d).", rc);
    return ESP_FAIL;
  }

  ESP_RETURN_ON_ERROR(
    wait_for_sem(s_link.op_sem, pdMS_TO_TICKS(LM_CTRL_MACHINE_DISCOVERY_TIMEOUT_MS), "BLE discovery timed out."),
    TAG,
    "BLE discovery wait failed"
  );

  portENTER_CRITICAL(&s_link_lock);
  op_status = s_link.op_status;
  discovered_handle = s_link.discovered_handle;
  discovered_properties = s_link.discovered_properties;
  portEXIT_CRITICAL(&s_link_lock);

  if (op_status != 0 || discovered_handle == 0) {
    set_statusf("BLE characteristic discovery failed (status=%d).", op_status);
    return ESP_FAIL;
  }

  *out_handle = discovered_handle;
  if (out_properties != NULL) {
    *out_properties = discovered_properties;
  }
  return ESP_OK;
}

static bool response_is_success(const char *response, char *message, size_t message_size) {
  cJSON *root = NULL;
  cJSON *status_item = NULL;
  cJSON *message_item = NULL;
  bool success = false;

  if (message != NULL && message_size > 0) {
    message[0] = '\0';
  }

  if (response == NULL || response[0] == '\0') {
    copy_text(message, message_size, "Empty BLE response.");
    return false;
  }

  root = cJSON_Parse(response);
  if (root == NULL) {
    copy_text(message, message_size, "Invalid BLE JSON response.");
    return false;
  }

  status_item = cJSON_GetObjectItemCaseSensitive(root, "status");
  message_item = cJSON_GetObjectItemCaseSensitive(root, "message");
  if (cJSON_IsString(message_item) && message_item->valuestring != NULL) {
    copy_text(message, message_size, message_item->valuestring);
  } else if (cJSON_IsString(status_item) && status_item->valuestring != NULL) {
    copy_text(message, message_size, status_item->valuestring);
  }

  success = cJSON_IsString(status_item) &&
            status_item->valuestring != NULL &&
            strcasecmp(status_item->valuestring, "success") == 0;

  cJSON_Delete(root);
  return success;
}

static bool response_missing_or_empty(const char *response, const char *message) {
  return (response == NULL || response[0] == '\0') ||
         (message != NULL && strcmp(message, "Empty BLE response.") == 0);
}

static esp_err_t request_machine_read_value(const char *setting, char *response, size_t response_size) {
  uint16_t read_handle;

  if (setting == NULL || setting[0] == '\0' || response == NULL || response_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  portENTER_CRITICAL(&s_link_lock);
  read_handle = s_link.read_handle;
  portEXIT_CRITICAL(&s_link_lock);

  if (read_handle == 0) {
    set_statusf("BLE read handle missing.");
    return ESP_ERR_INVALID_STATE;
  }

  return run_write_operation(
    read_handle,
    setting,
    strlen(setting) + 1U,
    true,
    read_handle,
    response,
    response_size
  );
}

static size_t get_last_response_length(void) {
  size_t response_length;

  portENTER_CRITICAL(&s_link_lock);
  response_length = s_link.response_length;
  portEXIT_CRITICAL(&s_link_lock);

  return response_length;
}

static void run_ble_connection_diagnostics(void) {
  static const struct {
    const char *label;
    const char *setting;
  } k_diagnostics[] = {
    {"machineCapabilities", LM_CTRL_MACHINE_READ_CAPABILITIES},
    {"machineMode", LM_CTRL_MACHINE_READ_MACHINE_MODE},
    {"tankStatus", LM_CTRL_MACHINE_READ_TANK_STATUS},
    {"boilers", LM_CTRL_MACHINE_READ_BOILERS},
    {"smartStandBy", LM_CTRL_MACHINE_READ_SMART_STAND_BY},
  };
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];

  if (!LM_CTRL_BLE_VERBOSE_DIAGNOSTICS) {
    return;
  }

  ESP_LOGI(TAG, "Starting BLE read diagnostics.");
  for (size_t i = 0; i < sizeof(k_diagnostics) / sizeof(k_diagnostics[0]); ++i) {
    esp_err_t err = request_machine_read_value(k_diagnostics[i].setting, response, sizeof(response));
    size_t response_length = get_last_response_length();

    if (err != ESP_OK) {
      BLE_VERBOSE_LOGW("BLE diagnostic %s failed err=%s", k_diagnostics[i].label, esp_err_to_name(err));
      continue;
    }

    ESP_LOGI(
      TAG,
      "BLE diagnostic %s len=%u text=%s",
      k_diagnostics[i].label,
      (unsigned)response_length,
      response[0] != '\0' ? response : "<empty>"
    );
  }
}

static bool response_matches_machine_mode(const char *response, const char *expected_mode) {
  cJSON *root = NULL;
  bool matches = false;

  if (response == NULL || expected_mode == NULL) {
    return false;
  }

  root = cJSON_Parse(response);
  if (root == NULL) {
    return false;
  }

  matches = cJSON_IsString(root) &&
            root->valuestring != NULL &&
            strcmp(root->valuestring, expected_mode) == 0;

  cJSON_Delete(root);
  return matches;
}

static bool parse_boiler_details(
  const char *response,
  const char *boiler_id,
  bool *out_enabled,
  float *out_target
) {
  cJSON *root = NULL;
  cJSON *entry = NULL;
  bool found = false;

  if (response == NULL || boiler_id == NULL) {
    return false;
  }

  root = cJSON_Parse(response);
  if (root == NULL || !cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return false;
  }

  cJSON_ArrayForEach(entry, root) {
    cJSON *id_item = NULL;
    cJSON *enabled_item = NULL;
    cJSON *target_item = NULL;

    if (!cJSON_IsObject(entry)) {
      continue;
    }

    id_item = cJSON_GetObjectItemCaseSensitive(entry, "id");
    if (!cJSON_IsString(id_item) || id_item->valuestring == NULL || strcmp(id_item->valuestring, boiler_id) != 0) {
      continue;
    }

    enabled_item = cJSON_GetObjectItemCaseSensitive(entry, "isEnabled");
    target_item = cJSON_GetObjectItemCaseSensitive(entry, "target");
    if (out_enabled != NULL && cJSON_IsBool(enabled_item)) {
      *out_enabled = cJSON_IsTrue(enabled_item);
    }
    if (out_target != NULL && cJSON_IsNumber(target_item)) {
      *out_target = (float)target_item->valuedouble;
    }
    found = true;
    break;
  }

  cJSON_Delete(root);
  return found;
}

static esp_err_t verify_machine_mode_via_ble(bool enabled) {
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];
  const char *expected_mode = enabled ? LM_CTRL_MACHINE_MODE_BREWING : LM_CTRL_MACHINE_MODE_STANDBY;

  for (int attempt = 0; attempt < LM_CTRL_MACHINE_VERIFY_RETRY_COUNT; ++attempt) {
    if (attempt > 0) {
      vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_VERIFY_RETRY_DELAY_MS));
    }

    if (request_machine_read_value(LM_CTRL_MACHINE_READ_MACHINE_MODE, response, sizeof(response)) != ESP_OK) {
      continue;
    }

    BLE_VERBOSE_LOGI("BLE machineMode response: %s", response);
    if (response_matches_machine_mode(response, expected_mode)) {
      return ESP_OK;
    }
  }

  set_statusf("BLE mode verification failed for %s.", expected_mode);
  return ESP_FAIL;
}

static esp_err_t verify_boiler_enabled_via_ble(const char *boiler_id, bool expected_enabled) {
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];

  for (int attempt = 0; attempt < LM_CTRL_MACHINE_VERIFY_RETRY_COUNT; ++attempt) {
    bool actual_enabled = false;

    if (attempt > 0) {
      vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_VERIFY_RETRY_DELAY_MS));
    }

    if (request_machine_read_value(LM_CTRL_MACHINE_READ_BOILERS, response, sizeof(response)) != ESP_OK) {
      continue;
    }

    BLE_VERBOSE_LOGI("BLE boilers response: %s", response);
    if (parse_boiler_details(response, boiler_id, &actual_enabled, NULL) && actual_enabled == expected_enabled) {
      return ESP_OK;
    }
  }

  set_statusf("BLE boiler verification failed for %s.", boiler_id);
  return ESP_FAIL;
}

static esp_err_t verify_boiler_target_via_ble(const char *boiler_id, float expected_target) {
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];

  for (int attempt = 0; attempt < LM_CTRL_MACHINE_VERIFY_RETRY_COUNT; ++attempt) {
    float actual_target = 0.0f;

    if (attempt > 0) {
      vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_VERIFY_RETRY_DELAY_MS));
    }

    if (request_machine_read_value(LM_CTRL_MACHINE_READ_BOILERS, response, sizeof(response)) != ESP_OK) {
      continue;
    }

    BLE_VERBOSE_LOGI("BLE boilers response: %s", response);
    if (parse_boiler_details(response, boiler_id, NULL, &actual_target) &&
        fabsf(actual_target - expected_target) < 0.15f) {
      return ESP_OK;
    }
  }

  set_statusf("BLE target verification failed for %s.", boiler_id);
  return ESP_FAIL;
}

static esp_err_t read_machine_mode_via_ble(bool *out_standby_on) {
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];
  cJSON *root = NULL;
  esp_err_t ret = ESP_FAIL;

  if (out_standby_on == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  ESP_RETURN_ON_ERROR(
    request_machine_read_value(LM_CTRL_MACHINE_READ_MACHINE_MODE, response, sizeof(response)),
    TAG,
    "machineMode read failed"
  );

  root = cJSON_Parse(response);
  if (cJSON_IsString(root) && root->valuestring != NULL) {
    *out_standby_on = strcmp(root->valuestring, LM_CTRL_MACHINE_MODE_STANDBY) == 0;
    ret = ESP_OK;
  }
  cJSON_Delete(root);
  return ret;
}

static esp_err_t read_boilers_via_ble(float *out_temperature_c, bool *out_steam_on) {
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];
  bool steam_on = false;
  float temperature_c = 0.0f;

  if (out_temperature_c == NULL || out_steam_on == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  ESP_RETURN_ON_ERROR(
    request_machine_read_value(LM_CTRL_MACHINE_READ_BOILERS, response, sizeof(response)),
    TAG,
    "boilers read failed"
  );

  if (!parse_boiler_details(response, LM_CTRL_MACHINE_BOILER_COFFEE, NULL, &temperature_c) ||
      !parse_boiler_details(response, LM_CTRL_MACHINE_BOILER_STEAM, &steam_on, NULL)) {
    return ESP_FAIL;
  }

  *out_temperature_c = temperature_c;
  *out_steam_on = steam_on;
  return ESP_OK;
}

static bool fetch_values_via_ble(
  const lm_ctrl_machine_binding_t *binding,
  ctrl_values_t *values,
  uint32_t *loaded_mask
) {
  uint32_t local_loaded_mask = 0;

  if (values == NULL || loaded_mask == NULL) {
    return false;
  }

  *values = (ctrl_values_t){0};
  *loaded_mask = 0;

  if (binding == NULL || binding->communication_key[0] == '\0') {
    return false;
  }
  if (should_skip_ble_attempt()) {
    return false;
  }
  if (ensure_connected_and_authenticated(binding) != ESP_OK) {
    mark_ble_failure_now();
    return false;
  }

  if (read_machine_mode_via_ble(&values->standby_on) == ESP_OK) {
    local_loaded_mask |= LM_CTRL_MACHINE_FIELD_STANDBY;
  }
  if (read_boilers_via_ble(&values->temperature_c, &values->steam_on) == ESP_OK) {
    local_loaded_mask |= LM_CTRL_MACHINE_FIELD_TEMPERATURE | LM_CTRL_MACHINE_FIELD_STEAM;
  }

  if (local_loaded_mask != 0) {
    *loaded_mask = local_loaded_mask;
    clear_ble_failure();
    return true;
  }

  return false;
}

static bool fetch_values_via_cloud(
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask
) {
  if (values == NULL || loaded_mask == NULL || feature_mask == NULL) {
    return false;
  }

  if (lm_ctrl_wifi_live_updates_active()) {
    return false;
  }

  *values = (ctrl_values_t){0};
  *loaded_mask = 0;
  *feature_mask = 0;

  if (lm_ctrl_wifi_fetch_dashboard_values(values, loaded_mask, feature_mask) != ESP_OK) {
    return false;
  }

  return *loaded_mask != 0 || *feature_mask != 0;
}

static esp_err_t send_power_command(bool enabled) {
  uint16_t write_handle;
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];
  char message[96];
  char payload[128];
  const char *expected_mode = enabled ? LM_CTRL_MACHINE_MODE_BREWING : LM_CTRL_MACHINE_MODE_STANDBY;

  portENTER_CRITICAL(&s_link_lock);
  write_handle = s_link.write_handle;
  portEXIT_CRITICAL(&s_link_lock);

  snprintf(
    payload,
    sizeof(payload),
    "{\"name\":\"MachineChangeMode\",\"parameter\":{\"mode\":\"%s\"}}",
    enabled ? "BrewingMode" : "StandBy"
  );

  ESP_RETURN_ON_ERROR(
    run_write_operation(write_handle, payload, strlen(payload) + 1U, true, write_handle, response, sizeof(response)),
    TAG,
    "Power command failed"
  );

  if (!response_is_success(response, message, sizeof(message))) {
    if (response_missing_or_empty(response, message)) {
      ESP_LOGW(TAG, "BLE power response missing, verifying via machineMode.");
      if (verify_machine_mode_via_ble(enabled) == ESP_OK) {
        set_statusf("BLE power verified via machineMode: %s", enabled ? LM_CTRL_MACHINE_MODE_BREWING : LM_CTRL_MACHINE_MODE_STANDBY);
        return ESP_OK;
      }
      if (LM_CTRL_MACHINE_ENABLE_CLOUD_FALLBACK) {
        ESP_LOGW(TAG, "BLE power verification failed, using cloud fallback.");
        return send_power_command_cloud(enabled);
      }
      set_statusf("BLE power verification failed; cloud fallback disabled.");
      return ESP_FAIL;
    }
    set_statusf("BLE power command rejected: %s", message[0] != '\0' ? message : response);
    return ESP_FAIL;
  }

  if (verify_machine_mode_via_ble(enabled) != ESP_OK) {
    if (LM_CTRL_MACHINE_ENABLE_CLOUD_FALLBACK) {
      ESP_LOGW(TAG, "BLE power success response could not be verified, using cloud fallback.");
      return send_power_command_cloud(enabled);
    }
    set_statusf("BLE power response ok, but mode verify failed for %s.", expected_mode);
    return ESP_FAIL;
  }

  set_statusf(
    "BLE power verified: %s",
    message[0] != '\0' ? message : expected_mode
  );
  return ESP_OK;
}

static esp_err_t send_steam_command(bool enabled) {
  uint16_t write_handle;
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];
  char message[96];
  char payload[144];

  portENTER_CRITICAL(&s_link_lock);
  write_handle = s_link.write_handle;
  portEXIT_CRITICAL(&s_link_lock);

  snprintf(
    payload,
    sizeof(payload),
    "{\"name\":\"SettingBoilerEnable\",\"parameter\":{\"identifier\":\"SteamBoiler\",\"state\":%s}}",
    enabled ? "true" : "false"
  );

  ESP_RETURN_ON_ERROR(
    run_write_operation(write_handle, payload, strlen(payload) + 1U, true, write_handle, response, sizeof(response)),
    TAG,
    "Steam command failed"
  );

  if (!response_is_success(response, message, sizeof(message))) {
    if (response_missing_or_empty(response, message)) {
      ESP_LOGW(TAG, "BLE steam response missing, verifying via boilers.");
      if (verify_boiler_enabled_via_ble(LM_CTRL_MACHINE_BOILER_STEAM, enabled) == ESP_OK) {
        set_statusf("BLE steam verified via boilers: %s", enabled ? "on" : "off");
        return ESP_OK;
      }
      if (LM_CTRL_MACHINE_ENABLE_CLOUD_FALLBACK) {
        ESP_LOGW(TAG, "BLE steam verification failed, using cloud fallback.");
        return send_steam_command_cloud(enabled);
      }
      set_statusf("BLE steam verification failed; cloud fallback disabled.");
      return ESP_FAIL;
    }
    set_statusf("BLE steam command rejected: %s", message[0] != '\0' ? message : response);
    return ESP_FAIL;
  }

  set_statusf("BLE steam command applied: %s", message[0] != '\0' ? message : (enabled ? "on" : "off"));
  return ESP_OK;
}

static esp_err_t send_temperature_command(float temperature_c) {
  uint16_t write_handle;
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];
  char message[96];
  char payload[160];

  portENTER_CRITICAL(&s_link_lock);
  write_handle = s_link.write_handle;
  portEXIT_CRITICAL(&s_link_lock);

  snprintf(
    payload,
    sizeof(payload),
    "{\"name\":\"SettingBoilerTarget\",\"parameter\":{\"identifier\":\"CoffeeBoiler1\",\"value\":%.1f}}",
    (double)temperature_c
  );

  ESP_RETURN_ON_ERROR(
    run_write_operation(write_handle, payload, strlen(payload) + 1U, true, write_handle, response, sizeof(response)),
    TAG,
    "Temperature command failed"
  );

  if (!response_is_success(response, message, sizeof(message))) {
    if (response_missing_or_empty(response, message)) {
      ESP_LOGW(TAG, "BLE temperature response missing, verifying via boilers.");
      if (verify_boiler_target_via_ble(LM_CTRL_MACHINE_BOILER_COFFEE, temperature_c) == ESP_OK) {
        set_statusf("BLE temperature verified via boilers: %.1f C", (double)temperature_c);
        return ESP_OK;
      }
      if (LM_CTRL_MACHINE_ENABLE_CLOUD_FALLBACK) {
        ESP_LOGW(TAG, "BLE temperature verification failed, using cloud fallback.");
        return send_temperature_command_cloud(temperature_c);
      }
      set_statusf("BLE temperature verification failed; cloud fallback disabled.");
      return ESP_FAIL;
    }
    set_statusf("BLE temperature command rejected: %s", message[0] != '\0' ? message : response);
    return ESP_FAIL;
  }

  set_statusf("BLE temperature applied: %.1f C", (double)temperature_c);
  return ESP_OK;
}

static lm_ctrl_cloud_send_result_t send_cloud_command(
  const char *command,
  const char *payload,
  uint32_t field_mask,
  const ctrl_values_t *sent_values,
  const char *success_message,
  const char *pending_message,
  char *failure_status_text,
  size_t failure_status_text_size
) {
  char status_text[96];
  lm_ctrl_cloud_command_result_t result = {0};
  const bool websocket_connected = lm_ctrl_wifi_live_updates_connected();

  if (lm_ctrl_wifi_execute_machine_command(command, payload, &result, status_text, sizeof(status_text)) != ESP_OK) {
    if (failure_status_text != NULL && failure_status_text_size > 0) {
      copy_text(failure_status_text, failure_status_text_size, status_text);
    }
    set_statusf("Cloud fallback failed: %s", status_text[0] != '\0' ? status_text : "request error");
    return LM_CTRL_CLOUD_SEND_FAILED;
  }

  if (websocket_connected && result.command_id[0] != '\0' && register_pending_cloud_command(result.command_id, field_mask, sent_values)) {
    set_statusf("%s", pending_message != NULL ? pending_message : "Cloud command accepted, waiting for dashboard confirmation.");
    return LM_CTRL_CLOUD_SEND_WAITING_FOR_ACK;
  }

  set_statusf("%s", success_message);
  return LM_CTRL_CLOUD_SEND_APPLIED;
}

static lm_ctrl_cloud_send_result_t send_power_command_cloud(bool enabled) {
  char payload[96];
  ctrl_values_t sent_values = {0};

  snprintf(
    payload,
    sizeof(payload),
    "{\"mode\":\"%s\"}",
    enabled ? "BrewingMode" : "StandBy"
  );
  sent_values.standby_on = !enabled;

  return send_cloud_command(
    "CoffeeMachineChangeMode",
    payload,
    LM_CTRL_MACHINE_FIELD_STANDBY,
    &sent_values,
    enabled ? "Cloud power command applied: BrewingMode" : "Cloud power command applied: StandBy",
    enabled ? "Cloud power command accepted, waiting for dashboard confirmation." : "Cloud standby command accepted, waiting for dashboard confirmation."
    ,
    NULL,
    0
  );
}

static lm_ctrl_cloud_send_result_t send_steam_command_cloud(bool enabled) {
  char payload[80];
  ctrl_values_t sent_values = {0};

  snprintf(
    payload,
    sizeof(payload),
    "{\"boilerIndex\":1,\"enabled\":%s}",
    enabled ? "true" : "false"
  );
  sent_values.steam_on = enabled;

  return send_cloud_command(
    "CoffeeMachineSettingSteamBoilerEnabled",
    payload,
    LM_CTRL_MACHINE_FIELD_STEAM,
    &sent_values,
    enabled ? "Cloud steam command applied: on" : "Cloud steam command applied: off",
    enabled ? "Cloud steam command accepted, waiting for dashboard confirmation." : "Cloud steam-off command accepted, waiting for dashboard confirmation."
    ,
    NULL,
    0
  );
}

static lm_ctrl_cloud_send_result_t send_temperature_command_cloud(float temperature_c) {
  char payload[96];
  char success_message[64];
  char pending_message[96];
  ctrl_values_t sent_values = {0};

  snprintf(
    payload,
    sizeof(payload),
    "{\"boilerIndex\":1,\"targetTemperature\":%.1f}",
    (double)temperature_c
  );
  snprintf(success_message, sizeof(success_message), "Cloud temperature applied: %.1f C", (double)temperature_c);
  snprintf(
    pending_message,
    sizeof(pending_message),
    "Cloud temperature accepted, waiting for %.1f C confirmation.",
    (double)temperature_c
  );
  sent_values.temperature_c = temperature_c;

  return send_cloud_command(
    "CoffeeMachineSettingCoffeeBoilerTargetTemperature",
    payload,
    LM_CTRL_MACHINE_FIELD_TEMPERATURE,
    &sent_values,
    success_message,
    pending_message,
    NULL,
    0
  );
}

static esp_err_t send_prebrewing_mode_cloud(const char *mode) {
  char payload[80];
  char status_text[96];

  if (mode == NULL || mode[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }

  snprintf(payload, sizeof(payload), "{\"mode\":\"%s\"}", mode);

  status_text[0] = '\0';
  if (lm_ctrl_wifi_execute_machine_command(
        "CoffeeMachinePreBrewingChangeMode",
        payload,
        NULL,
        status_text,
        sizeof(status_text)
      ) == ESP_OK) {
    set_statusf("Cloud prebrewing mode applied: %s", mode);
    return ESP_OK;
  }

  if (strstr(status_text, "status 412") != NULL) {
    ESP_LOGW(TAG, "Cloud prebrewing mode change returned 412, trying times update anyway.");
    return ESP_ERR_NOT_SUPPORTED;
  }

  set_statusf("Cloud fallback failed: %s", status_text[0] != '\0' ? status_text : "request error");
  return ESP_FAIL;
}

static lm_ctrl_cloud_send_result_t send_prebrewing_times_cloud(float infuse_s, float pause_s) {
  char payload[128];
  char success_message[80];
  char pending_message[96];
  char status_text[192];
  ctrl_values_t sent_values = {0};
  lm_ctrl_cloud_send_result_t result;

  snprintf(
    payload,
    sizeof(payload),
    "{\"times\":{\"In\":%.1f,\"Out\":%.1f},\"groupIndex\":1,\"doseIndex\":\"ByGroup\"}",
    (double)infuse_s,
    (double)pause_s
  );
  snprintf(success_message, sizeof(success_message), "Cloud prebrewing applied: %.1f s / %.1f s", (double)infuse_s, (double)pause_s);
  snprintf(
    pending_message,
    sizeof(pending_message),
    "Cloud prebrewing accepted, waiting for %.1f s / %.1f s.",
    (double)infuse_s,
    (double)pause_s
  );
  sent_values.infuse_s = infuse_s;
  sent_values.pause_s = pause_s;

  status_text[0] = '\0';
  result = send_cloud_command(
        "CoffeeMachinePreBrewingSettingTimes",
        payload,
        LM_CTRL_MACHINE_FIELD_PREBREWING,
        &sent_values,
        success_message,
        pending_message,
        status_text,
        sizeof(status_text)
      );
  if (result != LM_CTRL_CLOUD_SEND_FAILED) {
    return result;
  }

  if (strstr(status_text, "status 412") != NULL) {
    char dashboard_status[192];

    dashboard_status[0] = '\0';
    if (lm_ctrl_wifi_log_prebrew_dashboard_state(dashboard_status, sizeof(dashboard_status)) == ESP_OK) {
      set_statusf("Cloud prebrewing unavailable: %s", dashboard_status);
    } else {
      set_statusf("Cloud prebrewing unavailable (412).");
    }
    clear_pending_mask(LM_CTRL_MACHINE_FIELD_PREBREWING);
    return LM_CTRL_CLOUD_SEND_FAILED;
  }

  set_statusf("Cloud fallback failed: %s", status_text[0] != '\0' ? status_text : "request error");
  return LM_CTRL_CLOUD_SEND_FAILED;
}

static lm_ctrl_cloud_send_result_t send_bbw_mode_cloud(ctrl_bbw_mode_t mode) {
  char payload[80];
  char success_message[96];
  char pending_message[120];
  ctrl_values_t sent_values = {0};

  snprintf(payload, sizeof(payload), "{\"mode\":\"%s\"}", ctrl_bbw_mode_cloud_code(mode));
  snprintf(success_message, sizeof(success_message), "Cloud brew by weight mode applied: %s", ctrl_bbw_mode_cloud_code(mode));
  snprintf(
    pending_message,
    sizeof(pending_message),
    "Cloud brew by weight mode accepted, waiting for %s.",
    ctrl_bbw_mode_cloud_code(mode)
  );
  sent_values.bbw_mode = mode;

  return send_cloud_command(
    "CoffeeMachineBrewByWeightChangeMode",
    payload,
    LM_CTRL_MACHINE_FIELD_BBW_MODE,
    &sent_values,
    success_message,
    pending_message,
    NULL,
    0
  );
}

static lm_ctrl_cloud_send_result_t send_bbw_doses_cloud(float dose_1_g, float dose_2_g) {
  char payload[128];
  char success_message[96];
  char pending_message[128];
  ctrl_values_t sent_values = {0};

  snprintf(
    payload,
    sizeof(payload),
    "{\"doses\":{\"Dose1\":%.1f,\"Dose2\":%.1f}}",
    (double)dose_1_g,
    (double)dose_2_g
  );
  snprintf(success_message, sizeof(success_message), "Cloud brew by weight doses applied: %.1f g / %.1f g", (double)dose_1_g, (double)dose_2_g);
  snprintf(
    pending_message,
    sizeof(pending_message),
    "Cloud brew by weight doses accepted, waiting for %.1f g / %.1f g.",
    (double)dose_1_g,
    (double)dose_2_g
  );
  sent_values.bbw_dose_1_g = dose_1_g;
  sent_values.bbw_dose_2_g = dose_2_g;

  return send_cloud_command(
    "CoffeeMachineBrewByWeightSettingDoses",
    payload,
    LM_CTRL_MACHINE_FIELD_BBW_DOSE_1 | LM_CTRL_MACHINE_FIELD_BBW_DOSE_2,
    &sent_values,
    success_message,
    pending_message,
    NULL,
    0
  );
}

static esp_err_t ensure_ble_handles(void) {
  uint16_t read_handle = 0;
  uint16_t write_handle = 0;
  uint16_t auth_handle = 0;
  uint8_t read_properties = 0;
  uint8_t write_properties = 0;
  uint8_t auth_properties = 0;
  bool ready;

  portENTER_CRITICAL(&s_link_lock);
  ready = s_link.handles_ready;
  portEXIT_CRITICAL(&s_link_lock);
  if (ready) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(
    discover_characteristic_handle(&s_link.read_uuid.u, LM_CTRL_DISCOVERY_READ, &read_handle, &read_properties),
    TAG,
    "Read characteristic discovery failed"
  );
  ESP_RETURN_ON_ERROR(
    discover_characteristic_handle(&s_link.write_uuid.u, LM_CTRL_DISCOVERY_WRITE, &write_handle, &write_properties),
    TAG,
    "Write characteristic discovery failed"
  );
  ESP_RETURN_ON_ERROR(
    discover_characteristic_handle(&s_link.auth_uuid.u, LM_CTRL_DISCOVERY_AUTH, &auth_handle, &auth_properties),
    TAG,
    "Auth characteristic discovery failed"
  );

  portENTER_CRITICAL(&s_link_lock);
  s_link.read_handle = read_handle;
  s_link.write_handle = write_handle;
  s_link.auth_handle = auth_handle;
  s_link.read_properties = read_properties;
  s_link.write_properties = write_properties;
  s_link.auth_properties = auth_properties;
  s_link.handles_ready = true;
  portEXIT_CRITICAL(&s_link_lock);

  set_statusf("BLE characteristics resolved.");
  return ESP_OK;
}

static esp_err_t authenticate_with_machine(const char *token) {
  uint16_t auth_handle;
  bool authenticated;
  bool diagnostics_logged;

  if (token == NULL || token[0] == '\0') {
    set_statusf("BLE auth token missing.");
    return ESP_ERR_INVALID_STATE;
  }

  portENTER_CRITICAL(&s_link_lock);
  authenticated = s_link.authenticated;
  auth_handle = s_link.auth_handle;
  diagnostics_logged = s_link.diagnostics_logged;
  portEXIT_CRITICAL(&s_link_lock);
  if (authenticated) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(
    run_write_operation(auth_handle, token, strlen(token), false, 0, NULL, 0),
    TAG,
    "BLE auth write failed"
  );

  portENTER_CRITICAL(&s_link_lock);
  s_link.authenticated = true;
  if (!diagnostics_logged) {
    s_link.diagnostics_logged = true;
  }
  portEXIT_CRITICAL(&s_link_lock);

  set_statusf("BLE authenticated with machine.");
  if (!diagnostics_logged) {
    run_ble_connection_diagnostics();
  }
  return ESP_OK;
}

static esp_err_t start_scan(void) {
  struct ble_gap_disc_params disc_params = {0};
  uint8_t own_addr_type;
  char target_serial[sizeof(s_link.target_serial)];
  int rc;

  portENTER_CRITICAL(&s_link_lock);
  if (s_link.scanning || s_link.connect_in_progress || s_link.connected) {
    portEXIT_CRITICAL(&s_link_lock);
    return ESP_OK;
  }
  copy_text(target_serial, sizeof(target_serial), s_link.target_serial);
  s_link.scanning = true;
  s_link.connect_after_scan = false;
  s_link.fallback_matches = 0;
  s_link.fallback_addr_valid = false;
  portEXIT_CRITICAL(&s_link_lock);

  rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc != 0) {
    portENTER_CRITICAL(&s_link_lock);
    s_link.scanning = false;
    portEXIT_CRITICAL(&s_link_lock);
    set_statusf("BLE address inference failed (rc=%d).", rc);
    return ESP_FAIL;
  }

  disc_params.filter_duplicates = 1;
  disc_params.passive = 0;

  rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, ble_gap_event, NULL);
  if (rc != 0) {
    portENTER_CRITICAL(&s_link_lock);
    s_link.scanning = false;
    portEXIT_CRITICAL(&s_link_lock);
    set_statusf("BLE scan start failed (rc=%d).", rc);
    return ESP_FAIL;
  }

  set_statusf("Scanning BLE for %s...", target_serial);
  return ESP_OK;
}

static esp_err_t start_connect_to_addr(const ble_addr_t *addr) {
  uint8_t own_addr_type;
  int rc;

  if (addr == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  rc = ble_gap_disc_cancel();
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    set_statusf("BLE scan cancel failed (rc=%d).", rc);
    return ESP_FAIL;
  }

  rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc != 0) {
    set_statusf("BLE connect address inference failed (rc=%d).", rc);
    return ESP_FAIL;
  }

  portENTER_CRITICAL(&s_link_lock);
  s_link.connect_in_progress = true;
  s_link.connect_after_scan = false;
  s_link.candidate_addr = *addr;
  portEXIT_CRITICAL(&s_link_lock);

  rc = ble_gap_connect(own_addr_type, addr, LM_CTRL_MACHINE_CONNECT_TIMEOUT_MS, NULL, ble_gap_event, NULL);
  if (rc != 0) {
    portENTER_CRITICAL(&s_link_lock);
    s_link.connect_in_progress = false;
    portEXIT_CRITICAL(&s_link_lock);
    set_statusf("BLE connect start failed (rc=%d).", rc);
    return ESP_FAIL;
  }

  set_statusf("Connecting to BLE machine...");
  return ESP_OK;
}

static bool refresh_binding(const lm_ctrl_machine_binding_t *binding) {
  bool same_binding = false;
  bool needs_reset = false;
  bool was_scanning = false;
  bool was_connected = false;
  uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;

  if (binding == NULL) {
    return false;
  }

  portENTER_CRITICAL(&s_link_lock);
  same_binding = strcmp(s_link.target_serial, binding->serial) == 0 &&
                 strcmp(s_link.target_token, binding->communication_key) == 0;
  if (!same_binding) {
    copy_text(s_link.target_serial, sizeof(s_link.target_serial), binding->serial);
    copy_text(s_link.target_token, sizeof(s_link.target_token), binding->communication_key);
    needs_reset = s_link.scanning || s_link.connect_in_progress || s_link.connected;
    was_scanning = s_link.scanning;
    was_connected = s_link.connected;
    conn_handle = s_link.conn_handle;
    s_link.handles_ready = false;
    s_link.authenticated = false;
    s_link.read_handle = 0;
    s_link.write_handle = 0;
    s_link.auth_handle = 0;
    s_link.read_properties = 0;
    s_link.write_properties = 0;
    s_link.auth_properties = 0;
    s_link.diagnostics_logged = false;
  }
  portEXIT_CRITICAL(&s_link_lock);

  if (!needs_reset) {
    return true;
  }

  if (was_scanning) {
    (void)ble_gap_disc_cancel();
  }
  if (was_connected && conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
  }

  set_statusf("Switching BLE target to %s.", binding->serial);
  return false;
}

static esp_err_t ensure_connected_and_authenticated(const lm_ctrl_machine_binding_t *binding) {
  bool host_ready;
  bool connected;

  if (!refresh_binding(binding)) {
    return ESP_ERR_INVALID_STATE;
  }

  portENTER_CRITICAL(&s_link_lock);
  host_ready = s_link.host_ready;
  connected = s_link.connected;
  portEXIT_CRITICAL(&s_link_lock);

  if (!host_ready) {
    set_statusf("BLE host not ready yet.");
    return ESP_ERR_INVALID_STATE;
  }

  if (!connected) {
  while (xSemaphoreTake(s_link.conn_sem, 0) == pdTRUE) {
  }

  ESP_RETURN_ON_ERROR(start_scan(), TAG, "BLE scan start failed");

  ESP_RETURN_ON_ERROR(
      wait_for_conn_sem_interruptible(pdMS_TO_TICKS(LM_CTRL_MACHINE_SCAN_TIMEOUT_MS), "BLE machine scan timed out."),
      TAG,
      "BLE connect wait failed"
    );

    portENTER_CRITICAL(&s_link_lock);
    connected = s_link.connected;
    portEXIT_CRITICAL(&s_link_lock);
    if (!connected) {
      set_statusf("BLE machine %s not found.", binding->serial);
      return ESP_FAIL;
    }
  }

  ESP_RETURN_ON_ERROR(ensure_ble_mtu(), TAG, "BLE MTU exchange failed");
  ESP_RETURN_ON_ERROR(ensure_ble_handles(), TAG, "BLE handle discovery failed");
  ESP_RETURN_ON_ERROR(authenticate_with_machine(binding->communication_key), TAG, "BLE auth failed");
  clear_ble_failure();
  return ESP_OK;
}

static bool has_sendable_work(const ctrl_values_t *values, uint32_t pending_mask) {
  if ((pending_mask & LM_CTRL_MACHINE_FIELD_PREBREWING) != 0) {
    return values != NULL;
  }
  if ((pending_mask & LM_CTRL_MACHINE_FIELD_BBW) != 0) {
    return values != NULL;
  }
  if ((pending_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0) {
    return true;
  }
  if (values == NULL) {
    return false;
  }
  if ((pending_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0) {
    return true;
  }
  if (values->standby_on) {
    return false;
  }
  return (pending_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0;
}

static bool apply_prebrewing_via_cloud(const ctrl_values_t *desired_values, uint32_t pending_mask) {
  ctrl_values_t current_values;
  lm_ctrl_cloud_send_result_t times_ret;

  if (desired_values == NULL || (pending_mask & LM_CTRL_MACHINE_FIELD_PREBREWING) == 0) {
    return false;
  }

  current_values = *desired_values;
  snapshot_desired_values(&current_values);

  esp_err_t mode_ret = send_prebrewing_mode_cloud("PreBrewing");
  if (mode_ret != ESP_OK && mode_ret != ESP_ERR_NOT_SUPPORTED) {
    return false;
  }
  snapshot_desired_values(&current_values);
  times_ret = send_prebrewing_times_cloud(current_values.infuse_s, current_values.pause_s);
  if (times_ret == LM_CTRL_CLOUD_SEND_FAILED && mode_ret == ESP_ERR_NOT_SUPPORTED) {
    clear_pending_mask(LM_CTRL_MACHINE_FIELD_PREBREWING);
    return false;
  }
  if (times_ret == LM_CTRL_CLOUD_SEND_FAILED) {
    return false;
  }
  if (times_ret == LM_CTRL_CLOUD_SEND_APPLIED) {
    mark_field_complete(LM_CTRL_MACHINE_FIELD_PREBREWING, &current_values);
  }
  return true;
}

static bool apply_bbw_via_cloud(const ctrl_values_t *desired_values, uint32_t pending_mask) {
  ctrl_values_t current_values;
  uint32_t feature_mask;

  if (desired_values == NULL || (pending_mask & LM_CTRL_MACHINE_FIELD_BBW) == 0) {
    return false;
  }

  current_values = *desired_values;
  snapshot_desired_values(&current_values);

  portENTER_CRITICAL(&s_link_lock);
  feature_mask = s_link.feature_mask;
  portEXIT_CRITICAL(&s_link_lock);

  if ((feature_mask & LM_CTRL_MACHINE_FEATURE_BBW) == 0) {
    clear_pending_mask(LM_CTRL_MACHINE_FIELD_BBW);
    set_statusf("Brew by weight is not available for the selected machine.");
    return false;
  }

  if ((pending_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0) {
    lm_ctrl_cloud_send_result_t result = send_bbw_mode_cloud(current_values.bbw_mode);
    if (result == LM_CTRL_CLOUD_SEND_FAILED) {
      return false;
    }
    if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
      mark_field_complete(LM_CTRL_MACHINE_FIELD_BBW_MODE, &current_values);
    }
  }

  if ((pending_mask & (LM_CTRL_MACHINE_FIELD_BBW_DOSE_1 | LM_CTRL_MACHINE_FIELD_BBW_DOSE_2)) != 0) {
    snapshot_desired_values(&current_values);
    lm_ctrl_cloud_send_result_t result = send_bbw_doses_cloud(current_values.bbw_dose_1_g, current_values.bbw_dose_2_g);
    if (result == LM_CTRL_CLOUD_SEND_FAILED) {
      return false;
    }
    if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
      mark_field_complete(LM_CTRL_MACHINE_FIELD_BBW_DOSE_1 | LM_CTRL_MACHINE_FIELD_BBW_DOSE_2, &current_values);
    }
  }

  return true;
}

static bool apply_pending_via_cloud(const ctrl_values_t *desired_values, uint32_t pending_mask) {
  ctrl_values_t current_values;
  bool progress = false;

  if (desired_values == NULL) {
    return false;
  }

  if (!LM_CTRL_MACHINE_ENABLE_CLOUD_FALLBACK) {
    return false;
  }

  current_values = *desired_values;
  snapshot_desired_values(&current_values);

  if ((pending_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0) {
    lm_ctrl_cloud_send_result_t result = send_power_command_cloud(!current_values.standby_on);
    if (result != LM_CTRL_CLOUD_SEND_FAILED) {
      if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
        mark_field_complete(LM_CTRL_MACHINE_FIELD_STANDBY, &current_values);
      }
      progress = true;
    }
  }

  if ((pending_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0) {
    snapshot_desired_values(&current_values);
    lm_ctrl_cloud_send_result_t result = send_temperature_command_cloud(current_values.temperature_c);
    if (result != LM_CTRL_CLOUD_SEND_FAILED) {
      if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
        mark_field_complete(LM_CTRL_MACHINE_FIELD_TEMPERATURE, &current_values);
      }
      progress = true;
    }
  }

  if (!current_values.standby_on && (pending_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0) {
    snapshot_desired_values(&current_values);
    lm_ctrl_cloud_send_result_t result = send_steam_command_cloud(current_values.steam_on);
    if (result != LM_CTRL_CLOUD_SEND_FAILED) {
      if (result == LM_CTRL_CLOUD_SEND_APPLIED) {
        mark_field_complete(LM_CTRL_MACHINE_FIELD_STEAM, &current_values);
      }
      progress = true;
    }
  }

  return progress;
}

static void machine_link_worker(void *arg) {
  (void)arg;

  while (1) {
    ctrl_values_t desired_values;
    uint32_t pending_mask;
    lm_ctrl_machine_binding_t binding = {0};
    bool progress = false;
    uint32_t sync_request_flags = LM_CTRL_MACHINE_SYNC_NONE;

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LM_CTRL_MACHINE_WORKER_IDLE_WAIT_MS)) > 0) {
      while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LM_CTRL_MACHINE_DEBOUNCE_MS)) > 0) {
      }
    }

    expire_pending_cloud_commands();

    while (1) {
      portENTER_CRITICAL(&s_link_lock);
      desired_values = s_link.desired_values;
      pending_mask = s_link.pending_mask & ~s_link.inflight_cloud_mask;
      sync_request_flags = s_link.sync_request_flags;
      portEXIT_CRITICAL(&s_link_lock);

      if (pending_mask == 0 && sync_request_flags == LM_CTRL_MACHINE_SYNC_NONE) {
        break;
      }

      if (!lm_ctrl_wifi_get_machine_binding(&binding) || !binding.configured || binding.serial[0] == '\0') {
        clear_pending_mask(pending_mask);
        portENTER_CRITICAL(&s_link_lock);
        s_link.sync_request_flags = LM_CTRL_MACHINE_SYNC_NONE;
        portEXIT_CRITICAL(&s_link_lock);
        set_statusf("No cloud machine selected.");
        break;
      }

      if (sync_request_flags != LM_CTRL_MACHINE_SYNC_NONE) {
        ctrl_values_t merged_values = {0};
        ctrl_values_t cloud_values = {0};
        ctrl_values_t ble_values = {0};
        uint32_t merged_mask = 0;
        uint32_t cloud_loaded_mask = 0;
        uint32_t ble_loaded_mask = 0;
        uint32_t feature_mask = 0;
        bool synced = false;

        if ((sync_request_flags & LM_CTRL_MACHINE_SYNC_CLOUD) != 0) {
          synced |= fetch_values_via_cloud(&cloud_values, &cloud_loaded_mask, &feature_mask);
          update_feature_mask(feature_mask);
          if (cloud_loaded_mask != 0) {
            merged_values = cloud_values;
            merged_mask = cloud_loaded_mask;
            synced = true;
          }
        }

        if ((sync_request_flags & LM_CTRL_MACHINE_SYNC_BLE) != 0 &&
            fetch_values_via_ble(&binding, &ble_values, &ble_loaded_mask)) {
          if ((ble_loaded_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0) {
            merged_values.standby_on = ble_values.standby_on;
          }
          if ((ble_loaded_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0) {
            merged_values.temperature_c = ble_values.temperature_c;
          }
          if ((ble_loaded_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0) {
            merged_values.steam_on = ble_values.steam_on;
          }
          merged_mask |= ble_loaded_mask;
          synced = true;
        }

        if (merged_mask != 0) {
          update_reported_values(&merged_values, merged_mask);
        }

        portENTER_CRITICAL(&s_link_lock);
        s_link.sync_request_flags = LM_CTRL_MACHINE_SYNC_NONE;
        portEXIT_CRITICAL(&s_link_lock);

        if (synced) {
          set_statusf("Machine values synchronized.");
        }
      }

      if (!has_sendable_work(&desired_values, pending_mask)) {
        if (sync_request_flags == LM_CTRL_MACHINE_SYNC_NONE) {
          break;
        }
        continue;
      }

      progress = apply_prebrewing_via_cloud(&desired_values, pending_mask);

      portENTER_CRITICAL(&s_link_lock);
      pending_mask = s_link.pending_mask;
      portEXIT_CRITICAL(&s_link_lock);

      if (pending_mask != 0) {
        progress |= apply_bbw_via_cloud(&desired_values, pending_mask);
        snapshot_desired_values(&desired_values);
        pending_mask = snapshot_pending_mask();
      }

      if (pending_mask == 0) {
        break;
      }

      if ((pending_mask & LM_CTRL_MACHINE_FIELD_BLE_MASK) == 0) {
        if (!progress) {
          vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_RETRY_DELAY_MS));
        }
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
        }
        continue;
      }

      if (binding.communication_key[0] == '\0') {
        clear_pending_mask(pending_mask & LM_CTRL_MACHINE_FIELD_BLE_MASK);
        set_statusf("No BLE machine token configured.");
        break;
      }

      if (should_skip_ble_attempt()) {
        progress = apply_pending_via_cloud(&desired_values, pending_mask);
        if (!progress) {
          vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_RETRY_DELAY_MS));
        }
        continue;
      }

      {
        esp_err_t ble_ret = ensure_connected_and_authenticated(&binding);

        if (ble_ret == ESP_ERR_NOT_FINISHED) {
          continue;
        }
        if (ble_ret != ESP_OK) {
        mark_ble_failure_now();
        progress = apply_pending_via_cloud(&desired_values, pending_mask);
        if (!progress) {
          vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_RETRY_DELAY_MS));
        }
        continue;
      }
      }

      progress = false;
      snapshot_desired_values(&desired_values);
      pending_mask = snapshot_pending_mask();

      if ((pending_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0) {
        if (send_power_command(!desired_values.standby_on) == ESP_OK) {
          mark_field_complete(LM_CTRL_MACHINE_FIELD_STANDBY, &desired_values);
          progress = true;
        }
      }

      if ((pending_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0) {
        if (send_temperature_command(desired_values.temperature_c) == ESP_OK) {
          mark_field_complete(LM_CTRL_MACHINE_FIELD_TEMPERATURE, &desired_values);
          progress = true;
        }
      }

      if (!desired_values.standby_on && (pending_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0) {
        if (send_steam_command(desired_values.steam_on) == ESP_OK) {
          mark_field_complete(LM_CTRL_MACHINE_FIELD_STEAM, &desired_values);
          progress = true;
        }
      }

      if (!progress) {
        vTaskDelay(pdMS_TO_TICKS(LM_CTRL_MACHINE_RETRY_DELAY_MS));
      }

      while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
      }
    }
  }
}

static void expire_pending_cloud_commands(void) {
  const int64_t now_us = esp_timer_get_time();
  size_t index;
  bool changed = false;
  bool should_sync = false;
  bool should_wake = false;

  portENTER_CRITICAL(&s_link_lock);
  for (index = 0; index < LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS; ++index) {
    lm_ctrl_pending_cloud_command_t *pending = &s_pending_cloud_commands[index];

    if (!pending->active || pending->started_us == 0 ||
        (now_us - pending->started_us) < LM_CTRL_MACHINE_CLOUD_ACK_TIMEOUT_US) {
      continue;
    }

    s_link.inflight_cloud_mask &= ~pending->field_mask;
    pending->active = false;
    pending->started_us = 0;
    pending->field_mask = 0;
    pending->last_status = LM_CTRL_CLOUD_COMMAND_STATUS_TIMEOUT;
    pending->command_id[0] = '\0';
    changed = true;
    should_sync = true;
  }

  if (should_sync) {
    s_link.sync_request_flags |= LM_CTRL_MACHINE_SYNC_ALL;
    s_link.status_version++;
    should_wake = s_link.pending_mask != 0 || s_link.sync_request_flags != LM_CTRL_MACHINE_SYNC_NONE;
  }
  portEXIT_CRITICAL(&s_link_lock);

  if (changed) {
    set_statusf("Cloud dashboard confirmation timed out.");
  }
  if (should_wake) {
    wake_machine_worker();
  }
}

static void machine_link_host_task(void *param) {
  (void)param;
  BLE_VERBOSE_LOGI("BLE host task started");
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void machine_link_on_reset(int reason) {
  portENTER_CRITICAL(&s_link_lock);
  s_link.host_ready = false;
  portEXIT_CRITICAL(&s_link_lock);
  set_statusf("BLE stack reset (reason=%d).", reason);
}

static void machine_link_on_sync(void) {
  int rc;

  rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    set_statusf("BLE identity setup failed (rc=%d).", rc);
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  s_link.host_ready = true;
  portEXIT_CRITICAL(&s_link_lock);

  set_statusf("BLE host ready.");

  if (s_link.worker_task != NULL) {
    xTaskNotifyGive(s_link.worker_task);
  }
}

int ble_gap_event(struct ble_gap_event *event, void *arg) {
  char matched_name[40];
  lm_ctrl_adv_match_t match_type;
  (void)arg;

  switch (event->type) {
    case BLE_GAP_EVENT_DISC:
      match_type = advertisement_match_type(&event->disc, matched_name, sizeof(matched_name));
      if (match_type == LM_CTRL_ADV_MATCH_EXACT) {
        ESP_LOGI(
          TAG,
          "Found matching machine %s at %02x:%02x:%02x:%02x:%02x:%02x",
          matched_name,
          event->disc.addr.val[5],
          event->disc.addr.val[4],
          event->disc.addr.val[3],
          event->disc.addr.val[2],
          event->disc.addr.val[1],
          event->disc.addr.val[0]
        );
        (void)start_connect_to_addr(&event->disc.addr);
      } else if (match_type == LM_CTRL_ADV_MATCH_FALLBACK) {
        bool should_log = false;

        portENTER_CRITICAL(&s_link_lock);
        if (!s_link.fallback_addr_valid) {
          s_link.fallback_addr = event->disc.addr;
          s_link.fallback_addr_valid = true;
          s_link.fallback_matches = 1;
          should_log = true;
        } else if (!addr_equal(&s_link.fallback_addr, &event->disc.addr) && s_link.fallback_matches < 2) {
          s_link.fallback_matches++;
          should_log = true;
        }
#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
        uint8_t fallback_matches = s_link.fallback_matches;
#endif
        portEXIT_CRITICAL(&s_link_lock);

#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
        if (should_log) {
          BLE_VERBOSE_LOGI(
            "Found fallback BLE machine %s at %02x:%02x:%02x:%02x:%02x:%02x (matches=%u)",
            matched_name,
            event->disc.addr.val[5],
            event->disc.addr.val[4],
            event->disc.addr.val[3],
            event->disc.addr.val[2],
            event->disc.addr.val[1],
            event->disc.addr.val[0],
            (unsigned)fallback_matches
          );
        }
#else
        (void)should_log;
#endif
      }
      return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE: {
      bool should_connect = false;
      ble_addr_t candidate_addr;
      ble_addr_t fallback_addr;
      bool fallback_addr_valid = false;
      uint8_t fallback_matches = 0;
      uint8_t own_addr_type;
      int rc;

      portENTER_CRITICAL(&s_link_lock);
      s_link.scanning = false;
      should_connect = s_link.connect_after_scan;
      candidate_addr = s_link.candidate_addr;
      fallback_addr = s_link.fallback_addr;
      fallback_addr_valid = s_link.fallback_addr_valid;
      fallback_matches = s_link.fallback_matches;
      if (!should_connect && fallback_addr_valid && fallback_matches == 1) {
        candidate_addr = fallback_addr;
        should_connect = true;
        s_link.connect_after_scan = true;
      }
      portEXIT_CRITICAL(&s_link_lock);

      if (!should_connect) {
        if (fallback_matches > 1) {
          set_statusf("Multiple BLE machines found; no unique match for %s.", s_link.target_serial);
        }
        if (s_link.conn_sem != NULL) {
          xSemaphoreGive(s_link.conn_sem);
        }
        return 0;
      }

      rc = ble_hs_id_infer_auto(0, &own_addr_type);
      if (rc != 0) {
        set_statusf("BLE connect address inference failed (rc=%d).", rc);
        if (s_link.conn_sem != NULL) {
          xSemaphoreGive(s_link.conn_sem);
        }
        return 0;
      }

      portENTER_CRITICAL(&s_link_lock);
      s_link.connect_in_progress = true;
      portEXIT_CRITICAL(&s_link_lock);

      rc = ble_gap_connect(own_addr_type, &candidate_addr, LM_CTRL_MACHINE_CONNECT_TIMEOUT_MS, NULL, ble_gap_event, NULL);
      if (rc != 0) {
        portENTER_CRITICAL(&s_link_lock);
        s_link.connect_after_scan = false;
        s_link.connect_in_progress = false;
        portEXIT_CRITICAL(&s_link_lock);
        set_statusf("BLE connect start failed (rc=%d).", rc);
        if (s_link.conn_sem != NULL) {
          xSemaphoreGive(s_link.conn_sem);
        }
        return 0;
      }

      set_statusf("Connecting to BLE machine...");
      return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        portENTER_CRITICAL(&s_link_lock);
        s_link.connected = true;
        s_link.connect_in_progress = false;
        s_link.connect_after_scan = false;
        s_link.conn_handle = event->connect.conn_handle;
        portEXIT_CRITICAL(&s_link_lock);
        set_statusf("BLE machine connected.");
      } else {
        portENTER_CRITICAL(&s_link_lock);
        s_link.connect_in_progress = false;
        s_link.connect_after_scan = false;
        portEXIT_CRITICAL(&s_link_lock);
        set_statusf("BLE connect failed (status=%d).", event->connect.status);
      }
      if (s_link.conn_sem != NULL) {
        xSemaphoreGive(s_link.conn_sem);
      }
      return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
      bool has_pending = false;

      portENTER_CRITICAL(&s_link_lock);
      has_pending = s_link.pending_mask != 0;
      clear_connection_state_locked();
      portEXIT_CRITICAL(&s_link_lock);

      set_statusf("BLE disconnected (reason=%d).", event->disconnect.reason);

      if (s_link.conn_sem != NULL) {
        xSemaphoreGive(s_link.conn_sem);
      }
      if (s_link.op_sem != NULL) {
        xSemaphoreGive(s_link.op_sem);
      }
      if (has_pending && s_link.worker_task != NULL) {
        xTaskNotifyGive(s_link.worker_task);
      }
      return 0;
    }

    case BLE_GAP_EVENT_MTU:
      BLE_VERBOSE_LOGI("BLE MTU updated to %d", event->mtu.value);
      return 0;

    default:
      return 0;
  }
}

esp_err_t lm_ctrl_machine_link_init(void) {
  int rc;

  if (s_link.initialized) {
    return ESP_OK;
  }

  s_link.conn_sem = xSemaphoreCreateBinary();
  s_link.op_sem = xSemaphoreCreateBinary();
  s_link.mtu_sem = xSemaphoreCreateBinary();
  if (s_link.conn_sem == NULL || s_link.op_sem == NULL || s_link.mtu_sem == NULL) {
    return ESP_ERR_NO_MEM;
  }

  if (ble_uuid_from_str(&s_link.read_uuid, LM_CTRL_MACHINE_UUID_READ) != 0 ||
      ble_uuid_from_str(&s_link.write_uuid, LM_CTRL_MACHINE_UUID_WRITE) != 0 ||
      ble_uuid_from_str(&s_link.auth_uuid, LM_CTRL_MACHINE_UUID_AUTH) != 0) {
    return ESP_FAIL;
  }

  rc = nimble_port_init();
  if (rc != ESP_OK) {
    return rc;
  }

  ble_hs_cfg.reset_cb = machine_link_on_reset;
  ble_hs_cfg.sync_cb = machine_link_on_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_store_config_init();

  xTaskCreate(machine_link_worker, "lm_ble_worker", LM_CTRL_MACHINE_WORKER_STACK_SIZE, NULL, 5, &s_link.worker_task);
  nimble_port_freertos_init(machine_link_host_task);

  portENTER_CRITICAL(&s_link_lock);
  s_link.initialized = true;
  portEXIT_CRITICAL(&s_link_lock);

  set_statusf("BLE transport initialized.");
  return ESP_OK;
}

esp_err_t lm_ctrl_machine_link_queue_values(const ctrl_values_t *values, uint32_t field_mask) {
  bool changed = false;

  if (!s_link.initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (values == NULL || field_mask == LM_CTRL_MACHINE_FIELD_NONE) {
    return ESP_ERR_INVALID_ARG;
  }

  portENTER_CRITICAL(&s_link_lock);
  s_link.desired_values = *values;
  s_link.pending_mask |= field_mask;
  apply_values_to_reported_locked(values, field_mask, &changed);
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);

  ESP_LOGI(
    TAG,
    "Queued machine fields=0x%02x temp=%.1f inf=%.1f pause=%.1f steam=%d standby=%d bbw_mode=%s dose1=%.1f dose2=%.1f",
    (unsigned)field_mask,
    values->temperature_c,
    values->infuse_s,
    values->pause_s,
    values->steam_on,
    values->standby_on,
    ctrl_bbw_mode_cloud_code(values->bbw_mode),
    values->bbw_dose_1_g,
    values->bbw_dose_2_g
  );

  if (s_link.worker_task != NULL) {
    xTaskNotifyGive(s_link.worker_task);
  }
  return ESP_OK;
}

esp_err_t lm_ctrl_machine_link_request_sync(void) {
  return lm_ctrl_machine_link_request_sync_mode(LM_CTRL_MACHINE_SYNC_ALL);
}

esp_err_t lm_ctrl_machine_link_request_sync_mode(uint32_t sync_flags) {
  if (!s_link.initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (sync_flags == LM_CTRL_MACHINE_SYNC_NONE) {
    return ESP_ERR_INVALID_ARG;
  }

  portENTER_CRITICAL(&s_link_lock);
  s_link.sync_request_flags |= sync_flags;
  portEXIT_CRITICAL(&s_link_lock);

  if (s_link.worker_task != NULL) {
    xTaskNotifyGive(s_link.worker_task);
  }
  return ESP_OK;
}

bool lm_ctrl_machine_link_get_values(ctrl_values_t *values, uint32_t *loaded_mask, uint32_t *feature_mask) {
  if (values == NULL || loaded_mask == NULL || feature_mask == NULL) {
    return false;
  }

  portENTER_CRITICAL(&s_link_lock);
  *values = s_link.reported_values;
  *loaded_mask = s_link.loaded_mask;
  *feature_mask = s_link.feature_mask;
  portEXIT_CRITICAL(&s_link_lock);

  return *loaded_mask != 0 || *feature_mask != 0;
}

void lm_ctrl_machine_link_apply_cloud_dashboard_values(
  const ctrl_values_t *values,
  uint32_t loaded_mask,
  uint32_t feature_mask
) {
  if (!s_link.initialized) {
    return;
  }

  update_feature_mask(feature_mask);
  update_reported_values(values, loaded_mask);
}

void lm_ctrl_machine_link_apply_cloud_command_updates(const lm_ctrl_cloud_command_update_t *updates, size_t update_count) {
  size_t index;
  bool should_wake = false;

  if (!s_link.initialized || updates == NULL || update_count == 0) {
    return;
  }

  for (index = 0; index < update_count; ++index) {
    int slot = find_pending_cloud_command_slot(updates[index].command_id);
    bool changed = false;
    uint32_t field_mask = 0;
    ctrl_values_t sent_values = {0};

    if (slot < 0) {
      continue;
    }

    portENTER_CRITICAL(&s_link_lock);
    field_mask = s_pending_cloud_commands[slot].field_mask;
    sent_values = s_pending_cloud_commands[slot].sent_values;
    s_pending_cloud_commands[slot].last_status = updates[index].status;

    if (updates[index].status == LM_CTRL_CLOUD_COMMAND_STATUS_SUCCESS) {
      s_link.inflight_cloud_mask &= ~field_mask;
      if ((s_link.pending_mask & field_mask) == 0) {
        apply_values_to_reported_locked(&sent_values, field_mask, &changed);
      } else {
        should_wake = true;
      }
      s_pending_cloud_commands[slot].active = false;
      s_pending_cloud_commands[slot].started_us = 0;
      s_pending_cloud_commands[slot].field_mask = 0;
      s_pending_cloud_commands[slot].command_id[0] = '\0';
      changed = true;
    } else if (updates[index].status == LM_CTRL_CLOUD_COMMAND_STATUS_ERROR ||
               updates[index].status == LM_CTRL_CLOUD_COMMAND_STATUS_TIMEOUT) {
      s_link.inflight_cloud_mask &= ~field_mask;
      s_pending_cloud_commands[slot].active = false;
      s_pending_cloud_commands[slot].started_us = 0;
      s_pending_cloud_commands[slot].field_mask = 0;
      s_pending_cloud_commands[slot].command_id[0] = '\0';
      s_link.sync_request_flags |= LM_CTRL_MACHINE_SYNC_ALL;
      changed = true;
      should_wake = true;
    }

    if (changed) {
      s_link.status_version++;
    }
    portEXIT_CRITICAL(&s_link_lock);

    switch (updates[index].status) {
      case LM_CTRL_CLOUD_COMMAND_STATUS_SUCCESS:
        set_statusf("Cloud command confirmed via dashboard.");
        break;
      case LM_CTRL_CLOUD_COMMAND_STATUS_ERROR:
        if (updates[index].error_code[0] != '\0') {
          set_statusf("Cloud command failed via dashboard (%s).", updates[index].error_code);
        } else {
          set_statusf("Cloud command failed via dashboard.");
        }
        break;
      case LM_CTRL_CLOUD_COMMAND_STATUS_TIMEOUT:
        set_statusf("Cloud command timed out via dashboard.");
        break;
      default:
        break;
    }
  }

  if (should_wake) {
    wake_machine_worker();
  }
}

void lm_ctrl_machine_link_handle_cloud_websocket_disconnect(void) {
  bool changed = false;
  size_t index;

  if (!s_link.initialized) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  if (s_link.inflight_cloud_mask != 0) {
    s_link.inflight_cloud_mask = 0;
    s_link.sync_request_flags |= LM_CTRL_MACHINE_SYNC_ALL;
    changed = true;
  }
  for (index = 0; index < LM_CTRL_MACHINE_MAX_PENDING_CLOUD_COMMANDS; ++index) {
    s_pending_cloud_commands[index].active = false;
    s_pending_cloud_commands[index].started_us = 0;
    s_pending_cloud_commands[index].field_mask = 0;
    s_pending_cloud_commands[index].last_status = LM_CTRL_CLOUD_COMMAND_STATUS_UNKNOWN;
    s_pending_cloud_commands[index].command_id[0] = '\0';
  }
  if (changed) {
    s_link.status_version++;
  }
  portEXIT_CRITICAL(&s_link_lock);

  if (changed) {
    set_statusf("Cloud websocket disconnected; falling back to sync.");
    wake_machine_worker();
  }
}

void lm_ctrl_machine_link_get_status(char *buffer, size_t buffer_size) {
  char local_buffer[LM_CTRL_MACHINE_STATUS_TEXT_LEN];

  if (buffer == NULL || buffer_size == 0) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  copy_text(local_buffer, sizeof(local_buffer), s_link.status_text);
  portEXIT_CRITICAL(&s_link_lock);

  copy_text(buffer, buffer_size, local_buffer);
}

void lm_ctrl_machine_link_get_info(lm_ctrl_machine_link_info_t *info) {
  if (info == NULL) {
    return;
  }

  portENTER_CRITICAL(&s_link_lock);
  info->connected = s_link.connected;
  info->authenticated = s_link.authenticated;
  info->pending_work = (s_link.pending_mask | s_link.inflight_cloud_mask) != 0;
  info->sync_pending = s_link.sync_request_flags != LM_CTRL_MACHINE_SYNC_NONE;
  info->pending_mask = s_link.pending_mask | s_link.inflight_cloud_mask;
  info->sync_flags = s_link.sync_request_flags;
  info->loaded_mask = s_link.loaded_mask;
  info->feature_mask = s_link.feature_mask;
  portEXIT_CRITICAL(&s_link_lock);
}

uint32_t lm_ctrl_machine_link_status_version(void) {
  uint32_t version;

  portENTER_CRITICAL(&s_link_lock);
  version = s_link.status_version;
  portEXIT_CRITICAL(&s_link_lock);

  return version;
}
