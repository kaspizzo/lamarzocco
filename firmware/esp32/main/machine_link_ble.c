/**
 * BLE transport, authentication, and direct machine command execution.
 */
#include "machine_link_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef enum {
  LM_CTRL_DISCOVERY_READ = 1,
  LM_CTRL_DISCOVERY_WRITE,
  LM_CTRL_DISCOVERY_AUTH,
} lm_ctrl_discovery_target_t;

typedef enum {
  LM_CTRL_ADV_MATCH_NONE = 0,
  LM_CTRL_ADV_MATCH_FALLBACK,
  LM_CTRL_ADV_MATCH_EXACT,
} lm_ctrl_adv_match_t;

static const char *TAG = "lm_ble";
#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
static uint8_t s_scan_log_budget = 0;
#endif

static bool addr_equal(const ble_addr_t *left, const ble_addr_t *right) {
  if (left == NULL || right == NULL) {
    return false;
  }
  return left->type == right->type && memcmp(left->val, right->val, sizeof(left->val)) == 0;
}

static bool contains_ascii_token_ignore_case(const char *text, const char *token) {
  size_t text_len;
  size_t token_len;
  size_t index;
  size_t inner;

  if (text == NULL || token == NULL) {
    return false;
  }

  text_len = strlen(text);
  token_len = strlen(token);
  if (token_len == 0 || text_len < token_len) {
    return false;
  }

  for (index = 0; index <= (text_len - token_len); ++index) {
    for (inner = 0; inner < token_len; ++inner) {
      unsigned char lhs = (unsigned char)text[index + inner];
      unsigned char rhs = (unsigned char)token[inner];

      if (toupper(lhs) != toupper(rhs)) {
        break;
      }
    }
    if (inner == token_len) {
      return true;
    }
  }

  return false;
}

#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
static const char *adv_match_label(lm_ctrl_adv_match_t match_type) {
  switch (match_type) {
    case LM_CTRL_ADV_MATCH_EXACT:
      return "exact";
    case LM_CTRL_ADV_MATCH_FALLBACK:
      return "fallback";
    case LM_CTRL_ADV_MATCH_NONE:
    default:
      return "none";
  }
}
#endif

static void log_scan_observation(const struct ble_gap_disc_desc *disc, lm_ctrl_adv_match_t match_type) {
#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
  struct ble_hs_adv_fields fields = {0};
  char local_name[40];
  size_t name_len = 0;

  if (disc == NULL || s_scan_log_budget == 0) {
    return;
  }

  local_name[0] = '\0';
  if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0 && fields.name_len > 0) {
    name_len = fields.name_len;
    if (name_len >= sizeof(local_name)) {
      name_len = sizeof(local_name) - 1;
    }
    memcpy(local_name, fields.name, name_len);
    local_name[name_len] = '\0';
  }

  if (local_name[0] == '\0' && disc->rssi < -70) {
    return;
  }

  ESP_LOGI(
    TAG,
    "BLE scan saw addr=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d name=%s match=%s",
    disc->addr.val[5],
    disc->addr.val[4],
    disc->addr.val[3],
    disc->addr.val[2],
    disc->addr.val[1],
    disc->addr.val[0],
    disc->rssi,
    local_name[0] != '\0' ? local_name : "<no-name>",
    adv_match_label(match_type)
  );
  s_scan_log_budget--;
#else
  (void)disc;
  (void)match_type;
#endif
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

  is_lm_model = contains_ascii_token_ignore_case(local_name, "MICRA") ||
                contains_ascii_token_ignore_case(local_name, "MINI") ||
                contains_ascii_token_ignore_case(local_name, "GS3");
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
  float *out_target,
  bool *out_has_target
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
    if (out_has_target != NULL) {
      *out_has_target = cJSON_IsNumber(target_item);
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
    if (parse_boiler_details(response, boiler_id, &actual_enabled, NULL, NULL) && actual_enabled == expected_enabled) {
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
    if (parse_boiler_details(response, boiler_id, NULL, &actual_target, NULL) &&
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

static esp_err_t read_boilers_via_ble(
  float *out_temperature_c,
  ctrl_steam_level_t *out_steam_level,
  ctrl_steam_level_t fallback_level
) {
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];
  bool steam_enabled = false;
  bool steam_target_available = false;
  ctrl_steam_level_t steam_level = ctrl_steam_level_enabled(fallback_level)
    ? ctrl_steam_level_normalize(fallback_level)
    : CTRL_STEAM_LEVEL_2;
  float steam_target_c = 0.0f;
  float temperature_c = 0.0f;

  if (out_temperature_c == NULL || out_steam_level == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  ESP_RETURN_ON_ERROR(
    request_machine_read_value(LM_CTRL_MACHINE_READ_BOILERS, response, sizeof(response)),
    TAG,
    "boilers read failed"
  );

  if (!parse_boiler_details(response, LM_CTRL_MACHINE_BOILER_COFFEE, NULL, &temperature_c, NULL) ||
      !parse_boiler_details(
        response,
        LM_CTRL_MACHINE_BOILER_STEAM,
        &steam_enabled,
        &steam_target_c,
        &steam_target_available
      )) {
    return ESP_FAIL;
  }

  if (!steam_enabled) {
    steam_level = CTRL_STEAM_LEVEL_OFF;
  } else if (steam_target_available) {
    ctrl_steam_level_t parsed_level = CTRL_STEAM_LEVEL_OFF;

    if (ctrl_steam_level_from_temperature(steam_target_c, &parsed_level)) {
      steam_level = parsed_level;
    }
  }

  *out_temperature_c = temperature_c;
  *out_steam_level = steam_level;
  return ESP_OK;
}

bool fetch_values_via_ble(
  const lm_ctrl_machine_binding_t *binding,
  ctrl_values_t *values,
  uint32_t *loaded_mask
) {
  uint32_t local_loaded_mask = 0;

  if (values == NULL || loaded_mask == NULL) {
    return false;
  }

  *values = (ctrl_values_t){0};
  values->steam_level = snapshot_preferred_steam_level();
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
  if (read_boilers_via_ble(&values->temperature_c, &values->steam_level, values->steam_level) == ESP_OK) {
    local_loaded_mask |= LM_CTRL_MACHINE_FIELD_TEMPERATURE | LM_CTRL_MACHINE_FIELD_STEAM;
  }

  if (local_loaded_mask != 0) {
    *loaded_mask = local_loaded_mask;
    clear_ble_failure();
    return true;
  }

  return false;
}

esp_err_t send_power_command(bool enabled) {
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
      ESP_LOGW(TAG, "BLE power verification failed, using cloud fallback.");
      return send_power_command_cloud(enabled);
    }
    set_statusf("BLE power command rejected: %s", message[0] != '\0' ? message : response);
    return ESP_FAIL;
  }

  if (verify_machine_mode_via_ble(enabled) != ESP_OK) {
    ESP_LOGW(TAG, "BLE power success response could not be verified, using cloud fallback.");
    return send_power_command_cloud(enabled);
  }

  set_statusf(
    "BLE power verified: %s",
    message[0] != '\0' ? message : expected_mode
  );
  return ESP_OK;
}

static esp_err_t steam_cloud_result_to_esp_err(lm_ctrl_cloud_send_result_t result) {
  return result == LM_CTRL_CLOUD_SEND_FAILED ? ESP_FAIL : ESP_OK;
}

const char *steam_level_cloud_code(ctrl_steam_level_t level) {
  switch (ctrl_steam_level_normalize(level)) {
    case CTRL_STEAM_LEVEL_1:
      return "Level1";
    case CTRL_STEAM_LEVEL_2:
      return "Level2";
    case CTRL_STEAM_LEVEL_3:
      return "Level3";
    case CTRL_STEAM_LEVEL_OFF:
    default:
      return NULL;
  }
}

static esp_err_t send_steam_enable_command(bool enabled, ctrl_steam_level_t desired_level) {
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
      ESP_LOGW(TAG, "BLE steam verification failed, using cloud fallback.");
      return steam_cloud_result_to_esp_err(send_steam_command_cloud(
        enabled ? desired_level : CTRL_STEAM_LEVEL_OFF
      ));
    }
    set_statusf("BLE steam command rejected: %s", message[0] != '\0' ? message : response);
    return ESP_FAIL;
  }

  set_statusf("BLE steam command applied: %s", message[0] != '\0' ? message : (enabled ? "on" : "off"));
  return ESP_OK;
}

static esp_err_t send_steam_level_command(ctrl_steam_level_t level) {
  const float target_temperature_c = ctrl_steam_level_target_temperature_c(level);
  uint16_t write_handle;
  char response[LM_CTRL_MACHINE_RESPONSE_MAX];
  char message[96];
  char payload[160];

  if (!ctrl_steam_level_enabled(level)) {
    return ESP_ERR_INVALID_ARG;
  }

  portENTER_CRITICAL(&s_link_lock);
  write_handle = s_link.write_handle;
  portEXIT_CRITICAL(&s_link_lock);

  snprintf(
    payload,
    sizeof(payload),
    "{\"name\":\"SettingBoilerTarget\",\"parameter\":{\"identifier\":\"SteamBoiler\",\"value\":%.1f}}",
    (double)target_temperature_c
  );

  ESP_RETURN_ON_ERROR(
    run_write_operation(write_handle, payload, strlen(payload) + 1U, true, write_handle, response, sizeof(response)),
    TAG,
    "Steam level command failed"
  );

  if (!response_is_success(response, message, sizeof(message))) {
    if (response_missing_or_empty(response, message)) {
      ESP_LOGW(TAG, "BLE steam target response missing, verifying via boilers.");
      if (verify_boiler_target_via_ble(LM_CTRL_MACHINE_BOILER_STEAM, target_temperature_c) == ESP_OK) {
        set_statusf("BLE steam level verified via boilers: %s", ctrl_steam_level_label(level));
        return ESP_OK;
      }
      set_statusf("BLE steam level verification failed; cloud fallback disabled.");
      return ESP_FAIL;
    }
    set_statusf("BLE steam level command rejected: %s", message[0] != '\0' ? message : response);
    return ESP_FAIL;
  }

  set_statusf("BLE steam level applied: %s", ctrl_steam_level_label(level));
  return ESP_OK;
}

esp_err_t send_steam_command(ctrl_steam_level_t level) {
  ctrl_steam_level_t normalized_level = ctrl_steam_level_normalize(level);

  if (!ctrl_steam_level_enabled(normalized_level)) {
    return send_steam_enable_command(false, CTRL_STEAM_LEVEL_OFF);
  }

  ESP_RETURN_ON_ERROR(send_steam_level_command(normalized_level), TAG, "Steam level update failed");
  return send_steam_enable_command(true, normalized_level);
}

esp_err_t send_temperature_command(float temperature_c) {
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
      ESP_LOGW(TAG, "BLE temperature verification failed, using cloud fallback.");
      return send_temperature_command_cloud(temperature_c);
    }
    set_statusf("BLE temperature command rejected: %s", message[0] != '\0' ? message : response);
    return ESP_FAIL;
  }

  set_statusf("BLE temperature applied: %.1f C", (double)temperature_c);
  return ESP_OK;
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
#if LM_CTRL_BLE_VERBOSE_DIAGNOSTICS
  s_scan_log_budget = 12;
#endif

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

  rc = ble_gap_disc(own_addr_type, LM_CTRL_MACHINE_SCAN_TIMEOUT_MS, &disc_params, ble_gap_event, NULL);
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

esp_err_t ensure_connected_and_authenticated(const lm_ctrl_machine_binding_t *binding) {
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
      wait_for_conn_sem_interruptible(
        pdMS_TO_TICKS(LM_CTRL_MACHINE_SCAN_TIMEOUT_MS + LM_CTRL_MACHINE_CONNECT_TIMEOUT_MS),
        "BLE machine discovery/connect timed out."
      ),
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


void machine_link_host_task(void *param) {
  (void)param;
  BLE_VERBOSE_LOGI("BLE host task started");
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void machine_link_on_reset(int reason) {
  portENTER_CRITICAL(&s_link_lock);
  s_link.host_ready = false;
  portEXIT_CRITICAL(&s_link_lock);
  set_statusf("BLE stack reset (reason=%d).", reason);
}

void machine_link_on_sync(void) {
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
      log_scan_observation(&event->disc, match_type);
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
