/** BLE source central + virtual scale peripheral, sharing the machine's host.
 * Mutable protocol state belongs exclusively to the NimBLE event queue.
 * Portal/machine tasks use snapshots and config revisions under s_lock. */
#include "scale_adapter.h"
#include "scale_adapter_core.h"
#include "scale_registration_policy.h"
#include "scale_link_lifecycle.h"
#include "lm_ctrl_nvs_keys.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nvs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if !CONFIG_BT_NIMBLE_ROLE_PERIPHERAL || !CONFIG_BT_NIMBLE_ROLE_BROADCASTER || CONFIG_BT_NIMBLE_MAX_CONNECTIONS < 3
#error "Scale adapter requires NimBLE peripheral/broadcaster roles and at least three connections; update existing sdkconfig from sdkconfig.defaults"
#endif

static const char *TAG = "lm_scale";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static lm_scale_adapter_info_t s_info;
static uint32_t s_revision, s_applied_revision;
static uint64_t s_registration_after;
static unsigned s_registration_failures;
static struct ble_npl_callout s_tick;
static bool s_initialized, s_synced, s_scanning, s_connecting, s_selected, s_subscribed, s_faulted;
static uint16_t s_source = BLE_HS_CONN_HANDLE_NONE, s_gateway = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_notify, s_source_notify, s_source_write, s_cccd, s_service_start, s_service_end, s_dsc_end;
static uint64_t s_deadline, s_scan_after, s_identified, s_heartbeat;
static lm_scale_link_lifecycle_t s_lifecycle;
static bool s_connect_started;
static uint64_t s_weight_log_at;
static bool s_stream_sent;
static bool s_source_write_no_rsp;
static ble_addr_t s_target;
static lm_scale_adapter_config_t s_config;
static lm_scale_adapter_core_t s_core;
static const lm_scale_driver_t *s_driver;
/* Union guarantees alignment for driver state; no heap allocation in BLE callbacks. */
static union { uint64_t alignment; uint8_t bytes[512]; } s_driver_state;
static ble_uuid_any_t s_source_service_uuid, s_source_notify_uuid, s_source_write_uuid;
static int source_gap(struct ble_gap_event *event, void *arg);
static int gateway_gap(struct ble_gap_event *event, void *arg);
static void tick(struct ble_npl_event *event);
static void stop_links(const char *reason, bool fatal);

static uint64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void status(const char *text) {
  bool changed;
  portENTER_CRITICAL(&s_lock);
  changed = strcmp(s_info.status, text) != 0;
  snprintf(s_info.status, sizeof(s_info.status), "%s", text);
  portEXIT_CRITICAL(&s_lock);
  if (changed) ESP_LOGI(TAG, "%s", text);
}
static bool parse_address(const char *text, uint8_t type, ble_addr_t *out) {
  if (strlen(text) != 17 || type > 1) return false;
  out->type = type;
  for (size_t i = 0; i < 6; ++i) {
    unsigned value = 0;
    for (size_t j = 0; j < 2; ++j) {
      unsigned char ch = text[i * 3 + j];
      if (!isxdigit(ch)) return false;
      value = value * 16 + (isdigit(ch) ? ch - '0' : toupper(ch) - 'A' + 10);
    }
    if (i < 5 && text[i * 3 + 2] != ':') return false;
    out->val[5 - i] = value;
  }
  return true;
}
static void format_address(const ble_addr_t *addr, char *text, size_t size) {
  snprintf(text, size, "%02X:%02X:%02X:%02X:%02X:%02X", addr->val[5], addr->val[4],
           addr->val[3], addr->val[2], addr->val[1], addr->val[0]);
}
static bool valid_config(const lm_scale_adapter_config_t *c) {
  if (!c || c->version != 1 || c->enabled > 1 || c->manual_mode > 1 || c->address_type > 1 ||
      !memchr(c->driver, 0, sizeof(c->driver)) || !memchr(c->address, 0, sizeof(c->address)) ||
      !memchr(c->name, 0, sizeof(c->name)) || !lm_scale_driver_find(c->driver)) return false;
  if (strncmp(c->name, "LUNAR-", 6) || strlen(c->name) < 7) return false;
  for (const char *p = c->name; *p; ++p)
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return false;
  ble_addr_t addr;
  if (c->address[0] && !parse_address(c->address, c->address_type, &addr)) return false;
  return !c->enabled || !c->address[0] || c->manual_mode;
}
void lm_scale_adapter_get_info(lm_scale_adapter_info_t *info) {
  if (!info) return;
  portENTER_CRITICAL(&s_lock);
  *info = s_info;
  portEXIT_CRITICAL(&s_lock);
}
bool lm_scale_adapter_source_active(void) {
  bool active;
  portENTER_CRITICAL(&s_lock);
  active = s_info.config.enabled && (s_info.source_connected || s_info.gateway_connected);
  portEXIT_CRITICAL(&s_lock);
  return active;
}
bool lm_scale_adapter_source_fresh(void) {
  bool fresh;
  portENTER_CRITICAL(&s_lock);
  fresh = s_info.config.enabled && s_info.source_fresh;
  portEXIT_CRITICAL(&s_lock);
  return fresh;
}
esp_err_t lm_scale_adapter_configure(const lm_scale_adapter_config_t *config) {
  if (!valid_config(config)) return ESP_ERR_INVALID_ARG;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(nvs, "scale_adapter", config, sizeof(*config));
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err != ESP_OK) return err;
  portENTER_CRITICAL(&s_lock);
  s_info.config = *config;
  ++s_revision;
  s_info.source_fresh = false;
  s_registration_after = 0;
  s_registration_failures = 0;
  portEXIT_CRITICAL(&s_lock);
  return ESP_OK;
}
bool lm_scale_adapter_registration_due(char *name, size_t size) {
  bool due = false;
  uint64_t now = now_ms();
  portENTER_CRITICAL(&s_lock);
  if (lm_scale_driver_can_bridge(lm_scale_driver_find(s_info.config.driver)) &&
      s_revision == s_applied_revision && s_info.config.enabled && s_info.source_fresh && !s_info.gateway_connected && !s_info.faulted &&
      now >= s_registration_after) {
    snprintf(name, size, "%s", s_info.config.name);
    /* Reserve this check before any blocking machine operation. */
    s_registration_after = now + lm_scale_registration_retry_ms(s_registration_failures);
    if (s_registration_failures < 3) ++s_registration_failures;
    due = true;
  }
  portEXIT_CRITICAL(&s_lock);
  return due;
}
void lm_scale_adapter_registration_result(const char *text, bool attempted) {
  bool changed;
  portENTER_CRITICAL(&s_lock);
  changed = strcmp(s_info.registration_status, text) != 0;
  snprintf(s_info.registration_status, sizeof(s_info.registration_status), "%s", text);
  if (attempted) ++s_info.registration_attempts;
  portEXIT_CRITICAL(&s_lock);
  if (changed || attempted) ESP_LOGI(TAG, "Registration: %s", text);
}
static int source_written(uint16_t conn, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg) {
  (void)attr; (void)arg;
  if (conn == s_source && lm_scale_link_allows_io(&s_lifecycle) && (!error || error->status))
    stop_links("Source scale rejected control write", true);
  return 0;
}
static bool source_write(lm_scale_frame_t frame) {
  if (!frame.size || s_source == BLE_HS_CONN_HANDLE_NONE || !s_source_write) return false;
  if (s_source_write_no_rsp)
    return ble_gattc_write_no_rsp_flat(s_source, s_source_write, frame.data, frame.size) == 0;
  return ble_gattc_write_flat(s_source, s_source_write, frame.data, frame.size, source_written, NULL) == 0;
}
static bool send_control(void *ctx, lm_scale_control_t control) {
  (void)ctx;
  if (!lm_scale_driver_can_bridge(s_driver) || !source_write(s_driver->control(control))) return false;
  if (s_driver->control_written && !s_driver->control_written(s_driver_state.bytes, control, now_ms())) return false;
  ESP_LOGI(TAG, "Source control queued: driver=%s control=%u", s_driver->id, (unsigned)control);
  return true;
}
static void stop_links(const char *reason, bool fatal) {
  if (!lm_scale_link_begin_stop(&s_lifecycle, now_ms(), s_gateway != BLE_HS_CONN_HANDLE_NONE)) return;
  if (!s_faulted || fatal) status(reason);
  ESP_LOGW(TAG, "%s", reason);
  s_faulted |= fatal;
  portENTER_CRITICAL(&s_lock);
  s_info.source_fresh = false;
  s_info.faulted = s_faulted;
  portEXIT_CRITICAL(&s_lock);
  if (ble_gap_adv_active()) (void)ble_gap_adv_stop();
  if (s_gateway != BLE_HS_CONN_HANDLE_NONE) {
    if (s_subscribed) {
      lm_scale_frame_t frame = lm_scale_packet(32, (const uint8_t[]){1}, 1);
      struct os_mbuf *om = ble_hs_mbuf_from_flat(frame.data, frame.size);
      if (om) (void)ble_gatts_notify_custom(s_gateway, s_notify, om);
    }
  }
  lm_scale_core_init(&s_core); /* Immediately invalidate all source/session data. */
  s_identified = s_heartbeat = 0;
  s_stream_sent = false;
  if (s_source != BLE_HS_CONN_HANDLE_NONE)
    (void)ble_gap_terminate(s_source, BLE_ERR_REM_USER_CONN_TERM);
  if (s_scanning) {
    /* NimBLE does not emit DISC_COMPLETE for an explicitly cancelled scan. */
    int rc = ble_gap_disc_cancel();
    if (rc == 0 || rc == BLE_HS_EALREADY) s_scanning = false;
  }
  if (s_connecting) {
    if (s_connect_started) (void)ble_gap_conn_cancel();
    else s_connecting = false;
  }
  s_scan_after = now_ms() + 1500;
}
static bool source_event(void *ctx, const lm_scale_event_t *event) {
  (void)ctx;
  uint64_t now = now_ms();
  if (!lm_scale_core_event(&s_core, event, now)) return false;
  if (event->kind == LM_SCALE_READY) {
    if (s_driver->identify) ESP_LOGI(TAG, "Source handshake confirmed; battery=%u%%", event->battery);
    else ESP_LOGI(TAG, "Source measurements validated (experimental driver); battery=%u%%", event->battery);
  }
  if (event->kind == LM_SCALE_WEIGHT && lm_scale_core_fresh(&s_core, now) &&
      (s_core.weights == 1 || now - s_weight_log_at >= 5000)) {
    s_weight_log_at = now;
    ESP_LOGI(TAG, "Source weight=%.2f g; samples=%llu", event->grams, (unsigned long long)s_core.weights);
  }
  if (event->kind == LM_SCALE_ACK)
    ESP_LOGI(TAG, "Source control completion=%u (%s)", (unsigned)event->control,
             s_driver->observe_control ? "observed state, experimental" : "protocol ACK");
  return true;
}
static int subscribed(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
  (void)attr; (void)arg;
  if (conn != s_source || !lm_scale_link_allows_io(&s_lifecycle)) return 0;
  if (!error || error->status || (s_driver->identify && !source_write(s_driver->identify()))) {
    stop_links("Scale notification subscription failed", false);
    return 0;
  }
  s_identified = now_ms();
  s_stream_sent = s_driver->subscribe == NULL;
  s_deadline = now_ms() + 5000;
  status(s_driver->identify ? "Waiting for fresh source handshake and weight" : "Waiting for source measurements (experimental driver)");
  return 0;
}
static int descriptors(uint16_t conn, const struct ble_gatt_error *error, uint16_t value,
                       const struct ble_gatt_dsc *dsc, void *arg) {
  (void)value; (void)arg;
  if (conn != s_source || !lm_scale_link_allows_io(&s_lifecycle)) return 0;
  int rc = error ? error->status : BLE_HS_EAPP;
  if (!rc && dsc && ble_uuid_u16(&dsc->uuid.u) == 0x2902) s_cccd = dsc->handle;
  if (rc == BLE_HS_EDONE && s_cccd) {
    const uint8_t p[] = {1, 0};
    if (ble_gattc_write_flat(conn, s_cccd, p, sizeof(p), subscribed, NULL) == 0) return 0;
  } else if (!rc) return 0;
  stop_links("Scale CCCD discovery failed", false);
  return 0;
}
static int characteristics(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg) {
  (void)arg;
  if (conn != s_source || !lm_scale_link_allows_io(&s_lifecycle)) return 0;
  int rc = error ? error->status : BLE_HS_EAPP;
  if (!rc && chr) {
    if (ble_uuid_cmp(&chr->uuid.u, &s_source_notify_uuid.u) == 0 && (chr->properties & BLE_GATT_CHR_F_NOTIFY)) {
      s_source_notify = chr->val_handle;
      s_dsc_end = s_service_end;
    } else if (s_source_notify && chr->def_handle > s_source_notify && chr->def_handle - 1 < s_dsc_end) {
      s_dsc_end = chr->def_handle - 1;
    }
    if (ble_uuid_cmp(&chr->uuid.u, &s_source_write_uuid.u) == 0 &&
        (chr->properties & (BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE))) {
      s_source_write = chr->val_handle;
      s_source_write_no_rsp = (chr->properties & BLE_GATT_CHR_F_WRITE_NO_RSP) != 0;
    }
    return 0;
  }
  if (rc == BLE_HS_EDONE && s_source_notify && s_source_write && s_dsc_end > s_source_notify &&
      ble_gattc_disc_all_dscs(conn, s_source_notify, s_dsc_end, descriptors, NULL) == 0) return 0;
  stop_links("Required source scale characteristics missing", false);
  return 0;
}
static int service(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg) {
  (void)arg;
  if (conn != s_source || !lm_scale_link_allows_io(&s_lifecycle)) return 0;
  int rc = error ? error->status : BLE_HS_EAPP;
  if (!rc && svc) { s_service_start = svc->start_handle; s_service_end = svc->end_handle; return 0; }
  if (rc == BLE_HS_EDONE && s_service_start &&
      ble_gattc_disc_all_chrs(conn, s_service_start, s_service_end, characteristics, NULL) == 0) return 0;
  stop_links("Required source scale service missing", false);
  return 0;
}
static void observe(const struct ble_gap_disc_desc *disc) {
  struct ble_hs_adv_fields fields = {0};
  if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) || !fields.name_len) return;
  char name[32] = {0};
  memcpy(name, fields.name, fields.name_len < sizeof(name) ? fields.name_len : sizeof(name) - 1);
  /* Discovery is advisory; connecting always requires the selected exact address. */
  if (strncmp(name, "LUNAR", 5) && strncmp(name, "ACAIA", 5) && strncmp(name, "acaia", 5) &&
      strncmp(name, "BOOKO", 5) && strncmp(name, "booko", 5)) return;
  char addr[18];
  format_address(&disc->addr, addr, sizeof(addr));
  portENTER_CRITICAL(&s_lock);
  size_t slot = 0;
  for (; slot < s_info.candidate_count; ++slot)
    if (!strcmp(addr, s_info.candidates[slot].address) && disc->addr.type == s_info.candidates[slot].address_type) break;
  if (slot < LM_SCALE_CANDIDATE_LIMIT) {
    if (slot == s_info.candidate_count) ++s_info.candidate_count;
    lm_scale_candidate_t *c = &s_info.candidates[slot];
    snprintf(c->address, sizeof(c->address), "%s", addr);
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->address_type = disc->addr.type;
    c->rssi = disc->rssi;
  }
  portEXIT_CRITICAL(&s_lock);
}
static int source_gap(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_DISC:
      if (!lm_scale_link_allows_io(&s_lifecycle)) break;
      observe(&event->disc);
      if (s_selected && !s_connecting && event->disc.addr.type == s_target.type &&
          !memcmp(event->disc.addr.val, s_target.val, 6)) {
        /* Explicit cancellation has no DISC_COMPLETE callback. Release our
         * scan state here, then connect from a separate tick event. */
        s_connecting = true;
        int rc = ble_gap_disc_cancel();
        if (rc == 0 || rc == BLE_HS_EALREADY) s_scanning = false;
      }
      break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
      s_scanning = false;
      break;
    case BLE_GAP_EVENT_CONNECT:
      s_connecting = s_connect_started = false;
      if (!event->connect.status) {
        s_source = event->connect.conn_handle;
        lm_scale_core_init(&s_core);
        s_driver->reset(s_driver_state.bytes);
        s_source_notify = s_source_write = s_cccd = s_service_start = s_service_end = s_dsc_end = 0;
        s_source_write_no_rsp = false;
        s_deadline = now_ms() + 10000;
        portENTER_CRITICAL(&s_lock);
        bool config_changed = s_revision != s_applied_revision;
        portEXIT_CRITICAL(&s_lock);
        if (s_faulted || !lm_scale_link_allows_io(&s_lifecycle) || config_changed ||
            ble_gattc_disc_svc_by_uuid(s_source, &s_source_service_uuid.u, service, NULL))
          {
            stop_links("Source scale connection cancelled", false);
            (void)ble_gap_terminate(s_source, BLE_ERR_REM_USER_CONN_TERM);
          }
        else {
          portENTER_CRITICAL(&s_lock);
          ++s_info.source_sessions;
          portEXIT_CRITICAL(&s_lock);
          status("Discovering source scale service");
        }
      } else s_scan_after = now_ms() + 1500;
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      if (event->disconnect.conn.conn_handle == s_source) {
        s_source = BLE_HS_CONN_HANDLE_NONE;
        stop_links("Source scale disconnected; waiting for its return", false);
      }
      break;
    case BLE_GAP_EVENT_NOTIFY_RX:
      if (event->notify_rx.conn_handle == s_source && event->notify_rx.attr_handle == s_source_notify && lm_scale_link_allows_io(&s_lifecycle)) {
        uint8_t data[256];
        size_t size = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (size > sizeof(data) || os_mbuf_copydata(event->notify_rx.om, 0, size, data)) {
          stop_links("Invalid source notification length", true);
        } else if (!s_driver->feed(s_driver_state.bytes, data, size, source_event, NULL) ||
                   (s_driver->observe_control && !s_driver->observe_control(s_driver_state.bytes, now_ms(), source_event, NULL))) {
          /* Source-scale protocol only: never log machine authentication data. */
          ESP_LOGW(TAG, "Rejected scale notification: driver=%s bytes=%u", s_driver->id, (unsigned)size);
          ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, size > 40 ? 40 : size, ESP_LOG_WARN);
          stop_links("Invalid source protocol or source unit is not grams", true);
        }
      }
      break;
    default: break;
  }
  return 0;
}

static int access_gateway(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg) {
  (void)attr; (void)arg;
  if (conn != s_gateway || !s_subscribed || !lm_scale_link_allows_io(&s_lifecycle)) return BLE_ATT_ERR_UNLIKELY;
  if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    lm_scale_frame_t frame;
    if (!lm_scale_core_peek(&s_core, now_ms(), &frame)) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctx->om, frame.data, frame.size) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
  }
  uint8_t data[256];
  size_t size = OS_MBUF_PKTLEN(ctx->om);
  if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR || size > sizeof(data) ||
      os_mbuf_copydata(ctx->om, 0, size, data)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  if (!lm_scale_core_command(&s_core, data, size, now_ms())) {
    const char *reason = s_core.error ? s_core.error : "Invalid gateway command";
    stop_links(reason, true);
    return BLE_ATT_ERR_UNLIKELY;
  }
  return 0;
}
static const ble_uuid128_t s_virtual_service = BLE_UUID128_INIT(
  0x55,0xe4,0x05,0xd2,0xaf,0x9f,0xa9,0x8f,0xe5,0x4a,0x7d,0xfe,0x43,0x53,0x53,0x49);
static const ble_uuid128_t s_virtual_notify = BLE_UUID128_INIT(
  0x16,0x96,0x24,0x47,0xc6,0x23,0x61,0xba,0xd9,0x4b,0x4d,0x1e,0x43,0x53,0x53,0x49);
static const ble_uuid128_t s_virtual_write = BLE_UUID128_INIT(
  0xb3,0x9b,0x72,0x34,0xbe,0xec,0xd4,0xa8,0xf4,0x43,0x41,0x88,0x43,0x53,0x53,0x49);
static const struct ble_gatt_chr_def s_virtual_chrs[] = {
  {.uuid = &s_virtual_notify.u, .access_cb = access_gateway,
   .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ, .val_handle = &s_notify},
  {.uuid = &s_virtual_write.u, .access_cb = access_gateway,
   .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP}, {0},
};
static const struct ble_gatt_svc_def s_services[] = {
  {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_virtual_service.u, .characteristics = s_virtual_chrs}, {0},
};
static int gateway_gap(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (!event->connect.status) {
        if (!lm_scale_driver_can_bridge(s_driver) || !lm_scale_link_allows_io(&s_lifecycle) ||
            s_gateway != BLE_HS_CONN_HANDLE_NONE || !lm_scale_core_fresh(&s_core, now_ms())) {
          (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
          break;
        }
        s_gateway = event->connect.conn_handle;
        lm_scale_core_end_gateway(&s_core);
        s_subscribed = false;
        s_deadline = now_ms() + 10000;
        portENTER_CRITICAL(&s_lock);
        ++s_info.gateway_sessions;
        s_registration_failures = 0;
        s_registration_after = now_ms() + 30000;
        portEXIT_CRITICAL(&s_lock);
        status("Machine connected to virtual scale");
      }
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.conn_handle == s_gateway && event->subscribe.attr_handle == s_notify) {
        s_subscribed = event->subscribe.cur_notify;
        ESP_LOGI(TAG, "Machine notifications %s", s_subscribed ? "subscribed" : "unsubscribed");
        if (!s_subscribed) {
          /* Discard late physical ACKs by starting a new physical session. */
          stop_links("Machine unsubscribed; resetting scale session", false);
        }
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      if (event->disconnect.conn.conn_handle == s_gateway) {
        bool pending = s_core.control_count != 0;
        s_gateway = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        lm_scale_core_end_gateway(&s_core);
        if (pending) stop_links("Machine left during a scale control; resetting source", false);
        else if (!s_faulted) status("Machine disconnected; virtual scale will advertise again");
      }
      break;
    default: break;
  }
  return 0;
}
static void advertise(void) {
  struct ble_hs_adv_fields fields = {0};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.name = (const uint8_t *)s_config.name;
  fields.name_len = strlen(s_config.name);
  fields.name_is_complete = 1;
  struct ble_hs_adv_fields rsp = {0};
  rsp.uuids128 = (ble_uuid128_t *)&s_virtual_service;
  rsp.num_uuids128 = 1;
  rsp.uuids128_is_complete = 1;
  struct ble_gap_adv_params params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
  uint8_t own;
  int rc = ble_hs_id_infer_auto(0, &own);
  if (!rc) rc = ble_gap_adv_set_fields(&fields);
  if (!rc) rc = ble_gap_adv_rsp_set_fields(&rsp);
  if (!rc) rc = ble_gap_adv_start(own, NULL, BLE_HS_FOREVER, &params, gateway_gap, NULL);
  if (rc) status("Could not advertise virtual scale; retrying");
  else status("Source ready; virtual scale advertising for machine");
}
static void tick(struct ble_npl_event *event) {
  (void)event;
  if (!s_synced) return;
  uint64_t now = now_ms();
  lm_scale_adapter_config_t desired;
  uint32_t revision;
  portENTER_CRITICAL(&s_lock);
  desired = s_info.config;
  revision = s_revision;
  portEXIT_CRITICAL(&s_lock);
  (void)lm_scale_link_finish_stop(&s_lifecycle, s_source != BLE_HS_CONN_HANDLE_NONE,
                                  s_gateway != BLE_HS_CONN_HANDLE_NONE, s_scanning, s_connecting);
  if (revision != s_applied_revision) {
    if (s_source != BLE_HS_CONN_HANDLE_NONE || s_gateway != BLE_HS_CONN_HANDLE_NONE || s_scanning || s_connecting) {
      stop_links("Applying scale adapter settings", false);
    } else {
      s_config = desired;
      portENTER_CRITICAL(&s_lock);
      s_applied_revision = revision;
      portEXIT_CRITICAL(&s_lock);
      s_faulted = false;
      s_driver = lm_scale_driver_find(s_config.driver);
      s_selected = s_config.address[0] && parse_address(s_config.address, s_config.address_type, &s_target);
      lm_scale_core_init(&s_core);
      if (s_driver) {
        s_driver->reset(s_driver_state.bytes);
        ble_uuid_from_str(&s_source_service_uuid, s_driver->service_uuid);
        ble_uuid_from_str(&s_source_notify_uuid, s_driver->notify_uuid);
        ble_uuid_from_str(&s_source_write_uuid, s_driver->write_uuid);
      }
      if (ble_gap_adv_active()) (void)ble_gap_adv_stop();
      status(!s_config.enabled ? "Scale adapter disabled" : s_selected ? "Waiting for selected source scale" : "Scanning; select a source scale and confirm manual mode");
    }
  }
  if (s_lifecycle.gateway_close_at && now >= s_lifecycle.gateway_close_at && s_gateway != BLE_HS_CONN_HANDLE_NONE)
    (void)ble_gap_terminate(s_gateway, BLE_ERR_REM_USER_CONN_TERM);
  if (!lm_scale_link_allows_io(&s_lifecycle)) goto publish;
  if (revision == s_applied_revision && s_config.enabled && !s_faulted) {
    if (s_source != BLE_HS_CONN_HANDLE_NONE) {
      if (s_driver->subscribe && s_identified && !s_stream_sent && now - s_identified >= 500) {
        s_stream_sent = source_write(s_driver->subscribe());
        if (!s_stream_sent) { stop_links("Source streaming command failed", false); goto publish; }
      }
      if ((!s_core.ready || !s_core.has_weight) && now >= s_deadline) {
        stop_links("Source setup timed out; retrying", false);
        goto publish;
      }
      else if (s_core.ready && s_core.has_weight) {
        if (s_driver->control_timeout_ms) s_core.control_timeout_ms = s_driver->control_timeout_ms;
        if (!lm_scale_core_tick(&s_core, now, send_control, NULL)) {
          const char *reason = s_core.error;
          stop_links(reason, s_core.control_count != 0 || s_core.reply_count != 0);
          goto publish;
        } else {
          if (s_driver->heartbeat && (!s_heartbeat || now - s_heartbeat >= 2000)) {
            s_heartbeat = now;
            if (!source_write(s_driver->heartbeat())) {
              stop_links("Source heartbeat write failed", false);
              goto publish;
            }
          }
          if (lm_scale_driver_can_bridge(s_driver) && s_gateway == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active()) advertise();
          if (s_gateway != BLE_HS_CONN_HANDLE_NONE && s_subscribed) {
            lm_scale_frame_t frame;
            if (lm_scale_core_peek(&s_core, now, &frame)) {
              struct os_mbuf *om = ble_hs_mbuf_from_flat(frame.data, frame.size);
              if (om && ble_gatts_notify_custom(s_gateway, s_notify, om) == 0) lm_scale_core_delivered(&s_core);
            }
          }
        }
      }
      if (s_gateway != BLE_HS_CONN_HANDLE_NONE && !s_subscribed && now >= s_deadline)
        stop_links("Machine did not subscribe to scale notifications", false);
    } else if (s_gateway == BLE_HS_CONN_HANDLE_NONE && s_connecting && !s_scanning && !ble_gap_conn_active()) {
      uint8_t own;
      if (ble_hs_id_infer_auto(0, &own) ||
          ble_gap_connect(own, &s_target, 10000, NULL, source_gap, NULL)) {
        s_connecting = false;
        s_scan_after = now + 1500;
      } else {
        s_connect_started = true;
      }
    } else if (!s_connecting && !s_scanning && !ble_gap_disc_active() && !ble_gap_conn_active() && now >= s_scan_after) {
      uint8_t own;
      struct ble_gap_disc_params params = {.passive = 0, .filter_duplicates = 0};
      if (!ble_hs_id_infer_auto(0, &own) && !ble_gap_disc(own, 2000, &params, source_gap, NULL)) s_scanning = true;
      s_scan_after = now + 3500;
    }
  }
publish:;
  bool advertising = ble_gap_adv_active();
  portENTER_CRITICAL(&s_lock);
  s_info.source_connected = s_source != BLE_HS_CONN_HANDLE_NONE;
  s_info.source_fresh = lm_scale_core_fresh(&s_core, now);
  s_info.gateway_connected = s_gateway != BLE_HS_CONN_HANDLE_NONE && s_subscribed && lm_scale_link_allows_io(&s_lifecycle);
  s_info.advertising = advertising;
  s_info.faulted = s_faulted;
  s_info.grams = s_core.grams;
  s_info.battery = s_core.battery;
  portEXIT_CRITICAL(&s_lock);
  ble_npl_callout_reset(&s_tick, ble_npl_time_ms_to_ticks32(25));
}
esp_err_t lm_scale_adapter_init(void) {
  if (s_initialized) return ESP_OK;
  s_info.config = (lm_scale_adapter_config_t){.version = 1, .driver = "acaia_legacy", .name = "LUNAR-C"};
  nvs_handle_t nvs;
  if (nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
    lm_scale_adapter_config_t stored;
    size_t size = sizeof(stored);
    if (nvs_get_blob(nvs, "scale_adapter", &stored, &size) == ESP_OK && size == sizeof(stored) && valid_config(&stored))
      s_info.config = stored;
    nvs_close(nvs);
  }
  s_revision = 1;
  s_driver = lm_scale_driver_find(s_info.config.driver);
  if (!s_driver || s_driver->state_size > sizeof(s_driver_state)) return ESP_ERR_NO_MEM;
  lm_scale_core_init(&s_core);
  ble_svc_gap_init();
  ble_svc_gatt_init();
  int rc = ble_gatts_count_cfg(s_services);
  if (!rc) rc = ble_gatts_add_svcs(s_services);
  if (rc) return ESP_FAIL;
  ble_npl_callout_init(&s_tick, nimble_port_get_dflt_eventq(), tick, NULL);
  s_initialized = true;
  return ESP_OK;
}
void lm_scale_adapter_on_sync(void) {
  if (!s_initialized) return;
  s_synced = true;
  ble_npl_callout_reset(&s_tick, ble_npl_time_ms_to_ticks32(25));
}
void lm_scale_adapter_on_reset(void) {
  s_synced = false;
  if (!s_initialized) return;
  ble_npl_callout_stop(&s_tick);
  s_source = s_gateway = BLE_HS_CONN_HANDLE_NONE;
  s_scanning = s_connecting = s_connect_started = s_subscribed = false;
  s_identified = s_heartbeat = 0;
  s_lifecycle = (lm_scale_link_lifecycle_t){0};
  s_stream_sent = false;
  lm_scale_core_init(&s_core);
  portENTER_CRITICAL(&s_lock);
  s_info.source_connected = s_info.source_fresh = s_info.gateway_connected = s_info.advertising = false;
  portEXIT_CRITICAL(&s_lock);
}
