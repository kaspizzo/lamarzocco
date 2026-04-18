/**
 * Wi-Fi setup, captive portal, and cloud account integration for the controller.
 *
 * This module owns the setup AP, local HTTP portal, NVS-backed configuration,
 * machine selection, and the signed La Marzocco cloud requests used by the
 * controller firmware.
 */
#include "wifi_setup.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "cloud_api.h"
#include "machine_link.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/private/ctr_drbg.h"
#include "mbedtls/private/entropy.h"
#include "mbedtls/private/pk_private.h"
#include "mbedtls/private/sha256.h"
#include "mdns.h"
#include "nvs.h"
#include "extra/others/snapshot/lv_snapshot.h"

static const char *TAG = "lm_wifi";
#define LM_CTRL_ENABLE_CLOUD_LIVE_UPDATES 0

#define LM_CTRL_WIFI_NAMESPACE "wifi"
#define LM_CTRL_WIFI_KEY_SSID "ssid"
#define LM_CTRL_WIFI_KEY_PASS "pass"
#define LM_CTRL_WIFI_KEY_HOST "host"
#define LM_CTRL_WIFI_KEY_LANG "lang"
#define LM_CTRL_WIFI_KEY_LOGO_VERSION "logo_v"
#define LM_CTRL_WIFI_KEY_LOGO_BLOB "logo_blob"
#define LM_CTRL_WIFI_KEY_CLOUD_USER "cloud_user"
#define LM_CTRL_WIFI_KEY_CLOUD_PASS "cloud_pass"
#define LM_CTRL_WIFI_KEY_MACHINE_SERIAL "machine_serial"
#define LM_CTRL_WIFI_KEY_MACHINE_NAME "machine_name"
#define LM_CTRL_WIFI_KEY_MACHINE_MODEL "machine_model"
#define LM_CTRL_WIFI_KEY_MACHINE_KEY "machine_key"
#define LM_CTRL_WIFI_KEY_INSTALL_ID "install_id"
#define LM_CTRL_WIFI_KEY_INSTALL_KEY "install_key"
#define LM_CTRL_WIFI_KEY_INSTALL_REG "install_reg"
#define LM_CTRL_WIFI_DEFAULT_HOSTNAME "lm-controller"
#define LM_CTRL_CLOUD_HOST "lion.lamarzocco.io"
#define LM_CTRL_CLOUD_PORT 443
#define LM_CTRL_CLOUD_AUTH_INIT_PATH "/api/customer-app/auth/init"
#define LM_CTRL_CLOUD_AUTH_SIGNIN_PATH "/api/customer-app/auth/signin"
#define LM_CTRL_CLOUD_THINGS_PATH "/api/customer-app/things"
#define LM_CTRL_CLOUD_WS_URI "wss://lion.lamarzocco.io/ws/connect"
#define LM_CTRL_CLOUD_WS_DEST_PREFIX "/ws/sn/"
#define LM_CTRL_CLOUD_WS_DEST_SUFFIX "/dashboard"
#define LM_CTRL_CLOUD_MAX_FLEET 8
#define LM_CTRL_CLOUD_PROBE_STACK_SIZE (24 * 1024)
#define LM_CTRL_CLOUD_WS_STACK_SIZE (16 * 1024)
#define LM_CTRL_INSTALLATION_ID_LEN 37
#define LM_CTRL_PRIVATE_KEY_DER_MAX 256
#define LM_CTRL_CLOUD_SECRET_LEN 32
#define LM_CTRL_PORTAL_IP "192.168.4.1"
#define LM_CTRL_PORTAL_DNS_PORT 53
#define LM_CTRL_PORTAL_PASSWORD_FALLBACK "LMCTRL-SETUP"
#define LM_CTRL_CLOUD_WS_HEADER_BUFFER_LEN 768
#define LM_CTRL_CLOUD_WS_TOKEN_LEN 1024
#define LM_CTRL_CLOUD_WS_FRAME_MAX 8192
#define LM_CTRL_CLOUD_WS_SUBSCRIPTION_ID "lm-dashboard"
#define LM_CTRL_CLOUD_COMMAND_UPDATE_MAX 8
#define LM_CTRL_CLOUD_ACCESS_TOKEN_CACHE_FALLBACK_US (30LL * 1000LL * 1000LL)
#define LM_CTRL_CLOUD_ACCESS_TOKEN_EXP_SAFETY_MS (60LL * 1000LL)
#define LM_CTRL_STATIC_INSTALLATION_ID "28af7c9e-36cf-4f82-b4c6-0181adc3f59f"
#define LM_CTRL_STATIC_INSTALLATION_SECRET_B64 "75PrFs4iu69ClXC8RcOxevWDRm7d0TnCVVfmabNZ7Uo="
#define LM_CTRL_STATIC_PRIVATE_KEY_B64 "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg1P+Hrk0k9ttOzXVM5IDEekoX3oNCGatYkjXH/iaaMVOhRANCAASRshQpWbAJKJIVAiDpiuq3LZLVQGx9QzxZSYslckgeOPOt3ZdvMFMiv6K2AKOOjfart3sxAK229pLYTi3aGSKK"

typedef struct {
  char status[256];
  char status_html[512];
  char machine_status[160];
  char debug_status[512];
  char debug_status_html[768];
  char banner_html[256];
  char ssid_html[96];
  char hostname_html[96];
  char cloud_user_html[192];
  char local_url[96];
  char local_url_html[160];
  char selected_machine_text[160];
  char selected_machine_html[224];
  ctrl_values_t dashboard_values;
  uint32_t dashboard_loaded_mask;
  uint32_t dashboard_feature_mask;
  ctrl_preset_t presets[CTRL_PRESET_COUNT];
  lm_ctrl_wifi_info_t info;
  lm_ctrl_machine_link_info_t machine_info;
  lm_ctrl_cloud_machine_t fleet[LM_CTRL_CLOUD_MAX_FLEET];
  size_t fleet_count;
} lm_ctrl_setup_page_ctx_t;

typedef struct {
  bool initialized;
  bool wifi_started;
  bool has_credentials;
  bool has_cloud_credentials;
  bool cloud_connected;
  bool has_machine_selection;
  bool has_custom_logo;
  bool portal_running;
  bool sta_connecting;
  bool sta_connected;
  ctrl_language_t language;
  char sta_ssid[33];
  char sta_password[65];
  char hostname[33];
  uint8_t custom_logo_schema_version;
  uint8_t custom_logo_blob[LM_CTRL_CUSTOM_LOGO_BLOB_SIZE];
  char portal_ssid[33];
  char portal_password[65];
  char sta_ip[16];
  char cloud_username[96];
  char cloud_password[128];
  lm_ctrl_cloud_machine_t selected_machine;
  lm_ctrl_cloud_machine_t fleet[LM_CTRL_CLOUD_MAX_FLEET];
  size_t fleet_count;
  bool cloud_installation_ready;
  bool cloud_installation_registered;
  char cloud_installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  uint8_t cloud_secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t cloud_private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t cloud_private_key_der_len;
  uint32_t status_version;
  bool mdns_started;
  httpd_handle_t http_server;
  TaskHandle_t dns_task;
  int dns_socket;
  TaskHandle_t cloud_probe_task;
  TaskHandle_t cloud_ws_task;
  esp_websocket_client_handle_t cloud_ws_client;
  bool cloud_ws_stop_requested;
  bool cloud_ws_transport_connected;
  bool cloud_ws_connected;
  bool brew_active;
  int64_t brew_start_epoch_ms;
  int64_t brew_start_local_us;
  char cloud_access_token[LM_CTRL_CLOUD_WS_TOKEN_LEN];
  int64_t cloud_access_token_valid_until_us;
  char cloud_ws_access_token[LM_CTRL_CLOUD_WS_TOKEN_LEN];
  char cloud_ws_frame[LM_CTRL_CLOUD_WS_FRAME_MAX];
  size_t cloud_ws_frame_len;
  uint8_t cloud_http_requests_in_flight;
  esp_netif_t *sta_netif;
  esp_netif_t *ap_netif;
  SemaphoreHandle_t lock;
  SemaphoreHandle_t cloud_auth_lock;
} lm_ctrl_wifi_state_t;

static lm_ctrl_wifi_state_t s_state = {
  .hostname = LM_CTRL_WIFI_DEFAULT_HOSTNAME,
  .language = CTRL_LANGUAGE_EN,
  .dns_socket = -1,
};
static lv_img_dsc_t s_custom_logo_dsc = {
  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = LM_CTRL_CUSTOM_LOGO_WIDTH,
  .header.h = LM_CTRL_CUSTOM_LOGO_HEIGHT,
  .data_size = LM_CTRL_CUSTOM_LOGO_BLOB_SIZE,
};

static void copy_text(char *dst, size_t dst_size, const char *src);
static bool should_run_cloud_websocket_locked(void);
static esp_err_t ensure_cloud_websocket_task(void);

static void lock_state(void) {
  if (s_state.lock != NULL) {
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
  }
}

static void unlock_state(void) {
  if (s_state.lock != NULL) {
    xSemaphoreGive(s_state.lock);
  }
}

static void mark_status_dirty_locked(void) {
  s_state.status_version++;
}

static void set_cloud_connected(bool connected) {
  lock_state();
  if (s_state.cloud_connected != connected) {
    s_state.cloud_connected = connected;
    mark_status_dirty_locked();
  }
  unlock_state();
}

static int64_t current_epoch_ms(void) {
  struct timeval tv = {0};

  if (gettimeofday(&tv, NULL) != 0) {
    return 0;
  }
  if (tv.tv_sec < 1700000000) {
    return 0;
  }
  return ((int64_t)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}

static int64_t compute_cloud_access_token_cache_until_us(const char *access_token);

static void clear_cached_cloud_access_token_locked(void) {
  s_state.cloud_access_token[0] = '\0';
  s_state.cloud_access_token_valid_until_us = 0;
}

static void clear_cached_cloud_access_token(void) {
  lock_state();
  clear_cached_cloud_access_token_locked();
  unlock_state();
}

static void store_cached_cloud_access_token(const char *access_token) {
  int64_t valid_until_us;

  if (access_token == NULL || access_token[0] == '\0') {
    return;
  }

  valid_until_us = compute_cloud_access_token_cache_until_us(access_token);
  lock_state();
  copy_text(s_state.cloud_access_token, sizeof(s_state.cloud_access_token), access_token);
  s_state.cloud_access_token_valid_until_us = valid_until_us;
  unlock_state();
}

static bool copy_cached_cloud_access_token(char *buffer, size_t buffer_size) {
  bool valid = false;
  const int64_t now_us = esp_timer_get_time();

  if (buffer == NULL || buffer_size == 0) {
    return false;
  }

  lock_state();
  valid = s_state.cloud_access_token[0] != '\0' &&
          s_state.cloud_access_token_valid_until_us != 0 &&
          now_us < s_state.cloud_access_token_valid_until_us;
  if (valid) {
    copy_text(buffer, buffer_size, s_state.cloud_access_token);
  }
  unlock_state();

  return valid;
}

static void set_cloud_websocket_connected(bool transport_connected, bool stomp_connected) {
  lock_state();
  if (s_state.cloud_ws_transport_connected != transport_connected ||
      s_state.cloud_ws_connected != stomp_connected) {
    s_state.cloud_ws_transport_connected = transport_connected;
    s_state.cloud_ws_connected = stomp_connected;
    mark_status_dirty_locked();
  } else {
    s_state.cloud_ws_transport_connected = transport_connected;
    s_state.cloud_ws_connected = stomp_connected;
  }
  unlock_state();
}

static void set_brew_timer_state(bool brew_active, int64_t brew_start_epoch_ms) {
  const int64_t now_us = esp_timer_get_time();
  const int64_t now_epoch_ms = current_epoch_ms();
  bool changed = false;

  lock_state();
  if (s_state.brew_active != brew_active) {
    changed = true;
  }

  if (!brew_active) {
    if (s_state.brew_start_epoch_ms != 0 || s_state.brew_start_local_us != 0) {
      changed = true;
    }
    s_state.brew_active = false;
    s_state.brew_start_epoch_ms = 0;
    s_state.brew_start_local_us = 0;
  } else {
    if (!s_state.brew_active) {
      s_state.brew_start_local_us = now_us;
      changed = true;
    }
    s_state.brew_active = true;
    if (brew_start_epoch_ms > 0) {
      if (s_state.brew_start_epoch_ms != brew_start_epoch_ms) {
        changed = true;
      }
      s_state.brew_start_epoch_ms = brew_start_epoch_ms;
      if (now_epoch_ms > brew_start_epoch_ms) {
        int64_t elapsed_us = (now_epoch_ms - brew_start_epoch_ms) * 1000LL;
        if (elapsed_us < 0) {
          elapsed_us = 0;
        }
        s_state.brew_start_local_us = now_us - elapsed_us;
      }
    } else if (s_state.brew_start_local_us == 0) {
      s_state.brew_start_local_us = now_us;
    }
  }

  if (changed) {
    mark_status_dirty_locked();
  }
  unlock_state();
}

static void mark_cloud_http_request_started(void) {
  lock_state();
  if (s_state.cloud_http_requests_in_flight < UINT8_MAX) {
    s_state.cloud_http_requests_in_flight++;
  }
  unlock_state();
}

static void mark_cloud_http_request_finished(void) {
  bool should_retry_websocket = false;

  lock_state();
  if (s_state.cloud_http_requests_in_flight > 0) {
    s_state.cloud_http_requests_in_flight--;
  }
  should_retry_websocket =
    s_state.cloud_http_requests_in_flight == 0 &&
    s_state.cloud_probe_task == NULL &&
    s_state.cloud_ws_task == NULL &&
    should_run_cloud_websocket_locked();
  unlock_state();

  if (should_retry_websocket) {
    (void)ensure_cloud_websocket_task();
  }
}

static void clear_fleet_locked(void) {
  memset(s_state.fleet, 0, sizeof(s_state.fleet));
  s_state.fleet_count = 0;
}

static void clear_selected_machine_locked(void) {
  memset(&s_state.selected_machine, 0, sizeof(s_state.selected_machine));
  s_state.has_machine_selection = false;
}

static void clear_custom_logo_locked(void) {
  s_state.has_custom_logo = false;
  s_state.custom_logo_schema_version = 0;
  memset(s_state.custom_logo_blob, 0, sizeof(s_state.custom_logo_blob));
}

static esp_err_t erase_key_if_present(nvs_handle_t handle, const char *key) {
  esp_err_t ret = nvs_erase_key(handle, key);

  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  return ret;
}

static void delayed_restart_task(void *arg) {
  const int delay_ms = (int)(intptr_t)arg;

  vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 250));
  esp_restart();
}

static esp_err_t update_mdns_hostname(void);
static esp_err_t start_dns_server(void);
static void stop_dns_server(void);
static void cloud_probe_task(void *arg);
static void cloud_websocket_task(void *arg);
static esp_err_t ensure_cloud_websocket_task(void);
static void stop_cloud_websocket(bool wait_for_stop);
static esp_err_t disable_setup_ap(void);

static void copy_text(char *dst, size_t dst_size, const char *src) {
  size_t len;

  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  len = strnlen(src, dst_size - 1);
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static void write_u16_le(uint8_t *dst, uint16_t value) {
  dst[0] = (uint8_t)(value & 0xffU);
  dst[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void write_u32_le(uint8_t *dst, uint32_t value) {
  dst[0] = (uint8_t)(value & 0xffU);
  dst[1] = (uint8_t)((value >> 8) & 0xffU);
  dst[2] = (uint8_t)((value >> 16) & 0xffU);
  dst[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void write_i32_le(uint8_t *dst, int32_t value) {
  write_u32_le(dst, (uint32_t)value);
}

static void fill_portal_ssid_locked(void) {
  uint8_t mac[6] = {0};

  if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
    snprintf(s_state.portal_ssid, sizeof(s_state.portal_ssid), "LM-CTRL-SETUP");
    return;
  }

  snprintf(s_state.portal_ssid, sizeof(s_state.portal_ssid), "LM-CTRL-%02X%02X", mac[4], mac[5]);
}

static void fill_portal_password_locked(void) {
  uint8_t mac[6] = {0};

  if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
    copy_text(s_state.portal_password, sizeof(s_state.portal_password), LM_CTRL_PORTAL_PASSWORD_FALLBACK);
    return;
  }

  snprintf(
    s_state.portal_password,
    sizeof(s_state.portal_password),
    "LMCTRL-%02X%02X%02X%02X",
    mac[2],
    mac[3],
    mac[4],
    mac[5]
  );
}

static esp_err_t base64_decode_bytes(const char *input, uint8_t *output, size_t output_size, size_t *output_len) {
  int ret;

  if (input == NULL || output == NULL || output_len == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *output_len = 0;
  ret = mbedtls_base64_decode(output, output_size, output_len, (const unsigned char *)input, strlen(input));
  return ret == 0 ? ESP_OK : ESP_FAIL;
}

static bool parse_cloud_access_token_jwt_times(
  const char *access_token,
  int64_t *expiry_epoch_ms,
  int64_t *issued_epoch_ms,
  int64_t *not_before_epoch_ms
) {
  const char *payload_start;
  const char *payload_end;
  size_t payload_len;
  size_t normalized_len;
  size_t decoded_len = 0;
  char normalized[768];
  unsigned char decoded[512];
  cJSON *root = NULL;
  cJSON *exp_item;
  cJSON *iat_item;
  cJSON *nbf_item;
  bool parsed = false;

  if (access_token == NULL || expiry_epoch_ms == NULL || issued_epoch_ms == NULL || not_before_epoch_ms == NULL) {
    return false;
  }

  *expiry_epoch_ms = 0;
  *issued_epoch_ms = 0;
  *not_before_epoch_ms = 0;

  payload_start = strchr(access_token, '.');
  if (payload_start == NULL) {
    return false;
  }
  payload_start++;
  payload_end = strchr(payload_start, '.');
  if (payload_end == NULL || payload_end <= payload_start) {
    return false;
  }

  payload_len = (size_t)(payload_end - payload_start);
  if (payload_len == 0 || payload_len > sizeof(normalized) - 5) {
    return false;
  }

  for (size_t i = 0; i < payload_len; ++i) {
    char ch = payload_start[i];
    if (ch == '-') {
      normalized[i] = '+';
    } else if (ch == '_') {
      normalized[i] = '/';
    } else {
      normalized[i] = ch;
    }
  }
  normalized_len = payload_len;
  while ((normalized_len % 4U) != 0U) {
    normalized[normalized_len++] = '=';
  }
  normalized[normalized_len] = '\0';

  if (base64_decode_bytes(normalized, decoded, sizeof(decoded) - 1, &decoded_len) != ESP_OK || decoded_len == 0) {
    return false;
  }
  decoded[decoded_len] = '\0';

  root = cJSON_Parse((const char *)decoded);
  if (root == NULL) {
    return false;
  }

  exp_item = cJSON_GetObjectItemCaseSensitive(root, "exp");
  iat_item = cJSON_GetObjectItemCaseSensitive(root, "iat");
  nbf_item = cJSON_GetObjectItemCaseSensitive(root, "nbf");
  if (cJSON_IsNumber(exp_item)) {
    *expiry_epoch_ms = ((int64_t)exp_item->valuedouble) * 1000LL;
    parsed = true;
  }
  if (cJSON_IsNumber(iat_item)) {
    *issued_epoch_ms = ((int64_t)iat_item->valuedouble) * 1000LL;
  }
  if (cJSON_IsNumber(nbf_item)) {
    *not_before_epoch_ms = ((int64_t)nbf_item->valuedouble) * 1000LL;
  }

  cJSON_Delete(root);
  return parsed;
}

static int64_t compute_cloud_access_token_cache_until_us(const char *access_token) {
  const int64_t now_us = esp_timer_get_time();
  int64_t valid_until_us = now_us + LM_CTRL_CLOUD_ACCESS_TOKEN_CACHE_FALLBACK_US;
  int64_t expiry_epoch_ms = 0;
  int64_t issued_epoch_ms = 0;
  int64_t not_before_epoch_ms = 0;
  int64_t now_epoch_ms = 0;
  int64_t remaining_ms = 0;

  if (!parse_cloud_access_token_jwt_times(access_token, &expiry_epoch_ms, &issued_epoch_ms, &not_before_epoch_ms)) {
    ESP_LOGI(TAG, "Cloud access token cache fallback: 30s");
    return valid_until_us;
  }

  now_epoch_ms = current_epoch_ms();
  if (now_epoch_ms != 0) {
    remaining_ms = expiry_epoch_ms - now_epoch_ms - LM_CTRL_CLOUD_ACCESS_TOKEN_EXP_SAFETY_MS;
  } else if (issued_epoch_ms != 0 && expiry_epoch_ms > issued_epoch_ms) {
    remaining_ms = expiry_epoch_ms - issued_epoch_ms - LM_CTRL_CLOUD_ACCESS_TOKEN_EXP_SAFETY_MS;
    ESP_LOGI(TAG, "Cloud access token wall clock unavailable; using JWT lifetime from iat/exp");
  } else if (not_before_epoch_ms != 0 && expiry_epoch_ms > not_before_epoch_ms) {
    remaining_ms = expiry_epoch_ms - not_before_epoch_ms - LM_CTRL_CLOUD_ACCESS_TOKEN_EXP_SAFETY_MS;
    ESP_LOGI(TAG, "Cloud access token wall clock unavailable; using JWT lifetime from nbf/exp");
  } else {
    ESP_LOGI(TAG, "Cloud access token JWT exp found, but no usable lifetime claims; using 30s fallback");
    return valid_until_us;
  }

  if (remaining_ms <= 0) {
    ESP_LOGW(TAG, "Cloud access token JWT exp is too close; using 30s fallback");
    return valid_until_us;
  }

  valid_until_us = now_us + (remaining_ms * 1000LL);
  ESP_LOGI(TAG, "Cloud access token cached for %llds from JWT exp", (long long)(remaining_ms / 1000LL));
  return valid_until_us;
}

static esp_err_t derive_installation_material(
  const char *installation_id,
  const uint8_t *private_key_der,
  size_t private_key_der_len,
  char *public_key_b64,
  size_t public_key_b64_size,
  char *base_string,
  size_t base_string_size
) {
  return lm_ctrl_cloud_derive_installation_material(
    installation_id,
    private_key_der,
    private_key_der_len,
    public_key_b64,
    public_key_b64_size,
    base_string,
    base_string_size
  );
}

static esp_err_t generate_request_proof_text(
  const char *base_string,
  const uint8_t secret[32],
  char *proof,
  size_t proof_size
) {
  return lm_ctrl_cloud_generate_request_proof_text(base_string, secret, proof, proof_size);
}

static esp_err_t set_cloud_installation_registration(bool registered) {
  lock_state();
  s_state.cloud_installation_registered = registered;
  unlock_state();
  return ESP_OK;
}

static esp_err_t ensure_cloud_installation(void) {
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t secret_len = 0;
  size_t key_der_len = 0;
  esp_err_t ret = ESP_OK;

  lock_state();
  if (s_state.cloud_installation_ready) {
    unlock_state();
    return ESP_OK;
  }
  unlock_state();

  ret = base64_decode_bytes(LM_CTRL_STATIC_INSTALLATION_SECRET_B64, secret, sizeof(secret), &secret_len);
  if (ret != ESP_OK || secret_len != LM_CTRL_CLOUD_SECRET_LEN) {
    ESP_LOGE(TAG, "Failed to decode static cloud secret");
    return ESP_FAIL;
  }

  ret = base64_decode_bytes(LM_CTRL_STATIC_PRIVATE_KEY_B64, key_der, sizeof(key_der), &key_der_len);
  if (ret != ESP_OK || key_der_len == 0) {
    ESP_LOGE(TAG, "Failed to decode static cloud private key");
    return ret;
  }

  lock_state();
  copy_text(s_state.cloud_installation_id, sizeof(s_state.cloud_installation_id), LM_CTRL_STATIC_INSTALLATION_ID);
  memcpy(s_state.cloud_secret, secret, sizeof(secret));
  memcpy(s_state.cloud_private_key_der, key_der, key_der_len);
  s_state.cloud_private_key_der_len = key_der_len;
  s_state.cloud_installation_registered = false;
  s_state.cloud_installation_ready = true;
  unlock_state();
  return ESP_OK;
}

static esp_err_t load_credentials_locked(void) {
  nvs_handle_t handle;
  char language_code[8] = {0};
  uint8_t logo_schema_version = 0;
  bool has_logo_version = false;
  bool has_logo_blob = false;
  size_t size;
  esp_err_t ret;

  copy_text(s_state.hostname, sizeof(s_state.hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  s_state.language = CTRL_LANGUAGE_EN;
  s_state.sta_ssid[0] = '\0';
  s_state.sta_password[0] = '\0';
  s_state.cloud_username[0] = '\0';
  s_state.cloud_password[0] = '\0';
  clear_cached_cloud_access_token_locked();
  s_state.cloud_installation_ready = false;
  s_state.cloud_installation_registered = false;
  s_state.cloud_installation_id[0] = '\0';
  memset(s_state.cloud_secret, 0, sizeof(s_state.cloud_secret));
  s_state.cloud_private_key_der_len = 0;
  clear_selected_machine_locked();
  clear_custom_logo_locked();
  clear_fleet_locked();
  s_state.has_credentials = false;
  s_state.has_cloud_credentials = false;

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READONLY, &handle);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  if (ret != ESP_OK) {
    return ret;
  }

  size = sizeof(s_state.sta_ssid);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_SSID, s_state.sta_ssid, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }

  size = sizeof(s_state.sta_password);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_PASS, s_state.sta_password, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }

  size = sizeof(s_state.hostname);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_HOST, s_state.hostname, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }

  size = sizeof(language_code);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_LANG, language_code, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }
  if (ret == ESP_OK) {
    s_state.language = ctrl_language_from_code(language_code);
  }

  size = sizeof(s_state.cloud_username);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_CLOUD_USER, s_state.cloud_username, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }

  size = sizeof(s_state.cloud_password);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_CLOUD_PASS, s_state.cloud_password, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }

  size = sizeof(s_state.selected_machine.serial);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_MACHINE_SERIAL, s_state.selected_machine.serial, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }

  size = sizeof(s_state.selected_machine.name);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_MACHINE_NAME, s_state.selected_machine.name, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }

  size = sizeof(s_state.selected_machine.model);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_MACHINE_MODEL, s_state.selected_machine.model, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }

  size = sizeof(s_state.selected_machine.communication_key);
  ret = nvs_get_str(handle, LM_CTRL_WIFI_KEY_MACHINE_KEY, s_state.selected_machine.communication_key, &size);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }

  ret = nvs_get_u8(handle, LM_CTRL_WIFI_KEY_LOGO_VERSION, &logo_schema_version);
  if (ret == ESP_OK) {
    has_logo_version = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ret;
  }

  size = sizeof(s_state.custom_logo_blob);
  ret = nvs_get_blob(handle, LM_CTRL_WIFI_KEY_LOGO_BLOB, s_state.custom_logo_blob, &size);
  if (ret == ESP_OK && size == sizeof(s_state.custom_logo_blob)) {
    has_logo_blob = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_ERR_NVS_INVALID_LENGTH) {
    nvs_close(handle);
    return ret;
  }

  if (has_logo_version && has_logo_blob && logo_schema_version == LM_CTRL_CUSTOM_LOGO_SCHEMA_VERSION) {
    s_state.has_custom_logo = true;
    s_state.custom_logo_schema_version = logo_schema_version;
  } else {
    clear_custom_logo_locked();
  }

  s_state.has_credentials = s_state.sta_ssid[0] != '\0';
  s_state.has_cloud_credentials = s_state.cloud_username[0] != '\0' && s_state.cloud_password[0] != '\0';
  s_state.has_machine_selection = s_state.selected_machine.serial[0] != '\0';
  nvs_close(handle);
  return ESP_OK;
}

static esp_err_t save_credentials(const char *ssid, const char *password, const char *hostname, ctrl_language_t language) {
  nvs_handle_t handle;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_SSID, ssid), exit, TAG, "Failed to store SSID");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_PASS, password), exit, TAG, "Failed to store password");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_HOST, hostname), exit, TAG, "Failed to store hostname");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_LANG, ctrl_language_code(language)), exit, TAG, "Failed to store controller language");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit Wi-Fi settings");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    copy_text(s_state.sta_ssid, sizeof(s_state.sta_ssid), ssid);
    copy_text(s_state.sta_password, sizeof(s_state.sta_password), password);
    copy_text(s_state.hostname, sizeof(s_state.hostname), hostname);
    s_state.language = language;
    s_state.has_credentials = ssid[0] != '\0';
    s_state.sta_connected = false;
    s_state.sta_connecting = s_state.has_credentials;
    s_state.sta_ip[0] = '\0';
    mark_status_dirty_locked();
    unlock_state();
    if (s_state.sta_netif != NULL) {
      esp_netif_set_hostname(s_state.sta_netif, hostname[0] != '\0' ? hostname : LM_CTRL_WIFI_DEFAULT_HOSTNAME);
    }
    update_mdns_hostname();
  }

  return ret;
}

esp_err_t lm_ctrl_wifi_save_controller_preferences(const char *hostname, ctrl_language_t language) {
  nvs_handle_t handle;
  char effective_hostname[33];
  esp_err_t ret;

  copy_text(effective_hostname, sizeof(effective_hostname), hostname);
  if (effective_hostname[0] == '\0') {
    copy_text(effective_hostname, sizeof(effective_hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_HOST, effective_hostname), exit, TAG, "Failed to store hostname");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_LANG, ctrl_language_code(language)), exit, TAG, "Failed to store controller language");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit controller settings");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    copy_text(s_state.hostname, sizeof(s_state.hostname), effective_hostname);
    s_state.language = language;
    mark_status_dirty_locked();
    unlock_state();
    if (s_state.sta_netif != NULL) {
      esp_netif_set_hostname(s_state.sta_netif, effective_hostname);
    }
    update_mdns_hostname();
  }

  return ret;
}

esp_err_t lm_ctrl_wifi_save_controller_logo(uint8_t schema_version, const uint8_t *logo_data, size_t logo_size) {
  nvs_handle_t handle;
  esp_err_t ret;

  if (schema_version != LM_CTRL_CUSTOM_LOGO_SCHEMA_VERSION ||
      logo_data == NULL ||
      logo_size != LM_CTRL_CUSTOM_LOGO_BLOB_SIZE) {
    return ESP_ERR_INVALID_ARG;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_u8(handle, LM_CTRL_WIFI_KEY_LOGO_VERSION, schema_version), exit, TAG, "Failed to store logo schema");
  ESP_GOTO_ON_ERROR(nvs_set_blob(handle, LM_CTRL_WIFI_KEY_LOGO_BLOB, logo_data, logo_size), exit, TAG, "Failed to store logo data");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit controller logo");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
      lock_state();
      memcpy(s_state.custom_logo_blob, logo_data, logo_size);
      s_state.custom_logo_schema_version = schema_version;
      s_state.has_custom_logo = true;
      mark_status_dirty_locked();
      unlock_state();
      lv_img_cache_invalidate_src(&s_custom_logo_dsc);
      esp_lv_adapter_unlock();
    } else {
      lock_state();
      memcpy(s_state.custom_logo_blob, logo_data, logo_size);
      s_state.custom_logo_schema_version = schema_version;
      s_state.has_custom_logo = true;
      mark_status_dirty_locked();
      unlock_state();
    }
  }

  return ret;
}

esp_err_t lm_ctrl_wifi_clear_controller_logo(void) {
  nvs_handle_t handle = 0;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    ret = ESP_OK;
  } else if (ret == ESP_OK) {
    ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_LOGO_VERSION), exit, TAG, "Failed to erase logo schema");
    ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_LOGO_BLOB), exit, TAG, "Failed to erase logo data");
    ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit controller logo removal");
  } else {
    return ret;
  }

exit:
  if (ret != ESP_ERR_NVS_NOT_FOUND && handle != 0) {
    nvs_close(handle);
  }
  if (ret == ESP_OK) {
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
      lock_state();
      clear_custom_logo_locked();
      mark_status_dirty_locked();
      unlock_state();
      lv_img_cache_invalidate_src(&s_custom_logo_dsc);
      esp_lv_adapter_unlock();
    } else {
      lock_state();
      clear_custom_logo_locked();
      mark_status_dirty_locked();
      unlock_state();
    }
  }

  return ret;
}

static esp_err_t save_cloud_credentials(const char *username, const char *password) {
  nvs_handle_t handle;
  bool credentials_changed = false;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  lock_state();
  credentials_changed =
    strcmp(s_state.cloud_username, username) != 0 ||
    strcmp(s_state.cloud_password, password) != 0;
  unlock_state();

  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_CLOUD_USER, username), exit, TAG, "Failed to store cloud username");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_CLOUD_PASS, password), exit, TAG, "Failed to store cloud password");
  if (credentials_changed) {
    nvs_erase_key(handle, LM_CTRL_WIFI_KEY_MACHINE_SERIAL);
    nvs_erase_key(handle, LM_CTRL_WIFI_KEY_MACHINE_NAME);
    nvs_erase_key(handle, LM_CTRL_WIFI_KEY_MACHINE_MODEL);
    nvs_erase_key(handle, LM_CTRL_WIFI_KEY_MACHINE_KEY);
  }
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit cloud settings");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    copy_text(s_state.cloud_username, sizeof(s_state.cloud_username), username);
    copy_text(s_state.cloud_password, sizeof(s_state.cloud_password), password);
    s_state.has_cloud_credentials = username[0] != '\0' && password[0] != '\0';
    clear_cached_cloud_access_token_locked();
    if (credentials_changed) {
      clear_selected_machine_locked();
      clear_fleet_locked();
    }
    mark_status_dirty_locked();
    unlock_state();
    if (credentials_changed) {
      stop_cloud_websocket(true);
    } else {
      (void)ensure_cloud_websocket_task();
    }
  }

  return ret;
}

static esp_err_t save_machine_selection(const lm_ctrl_cloud_machine_t *machine) {
  nvs_handle_t handle;
  esp_err_t ret;

  if (machine == NULL || machine->serial[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }

  ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_SERIAL, machine->serial), exit, TAG, "Failed to store machine serial");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_NAME, machine->name), exit, TAG, "Failed to store machine name");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_MODEL, machine->model), exit, TAG, "Failed to store machine model");
  ESP_GOTO_ON_ERROR(nvs_set_str(handle, LM_CTRL_WIFI_KEY_MACHINE_KEY, machine->communication_key), exit, TAG, "Failed to store machine key");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit machine selection");

exit:
  nvs_close(handle);
  if (ret == ESP_OK) {
    lock_state();
    s_state.selected_machine = *machine;
    s_state.has_machine_selection = true;
    mark_status_dirty_locked();
    unlock_state();
    stop_cloud_websocket(true);
    (void)ensure_cloud_websocket_task();
  }

  return ret;
}

esp_err_t lm_ctrl_wifi_schedule_reboot(void) {
  if (xTaskCreate(delayed_restart_task, "lm_reboot", 3072, (void *)(intptr_t)1200, 5, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_reset_network(void) {
  nvs_handle_t handle;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret != ESP_OK) {
    return ret;
  }

  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_SSID), exit, TAG, "Failed to erase SSID");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_PASS), exit, TAG, "Failed to erase Wi-Fi password");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_CLOUD_USER), exit, TAG, "Failed to erase cloud username");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_CLOUD_PASS), exit, TAG, "Failed to erase cloud password");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_SERIAL), exit, TAG, "Failed to erase machine serial");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_NAME), exit, TAG, "Failed to erase machine name");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_MODEL), exit, TAG, "Failed to erase machine model");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_MACHINE_KEY), exit, TAG, "Failed to erase machine key");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_INSTALL_ID), exit, TAG, "Failed to erase installation id");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_INSTALL_KEY), exit, TAG, "Failed to erase installation key");
  ESP_GOTO_ON_ERROR(erase_key_if_present(handle, LM_CTRL_WIFI_KEY_INSTALL_REG), exit, TAG, "Failed to erase installation registration");
  ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "Failed to commit network reset");

exit:
  nvs_close(handle);
  if (ret != ESP_OK) {
    return ret;
  }

  stop_cloud_websocket(false);
  lock_state();
  s_state.sta_ssid[0] = '\0';
  s_state.sta_password[0] = '\0';
  s_state.cloud_username[0] = '\0';
  s_state.cloud_password[0] = '\0';
  s_state.has_credentials = false;
  s_state.has_cloud_credentials = false;
  s_state.sta_connected = false;
  s_state.sta_connecting = false;
  s_state.sta_ip[0] = '\0';
  s_state.cloud_installation_ready = false;
  s_state.cloud_installation_registered = false;
  s_state.cloud_installation_id[0] = '\0';
  memset(s_state.cloud_secret, 0, sizeof(s_state.cloud_secret));
  memset(s_state.cloud_private_key_der, 0, sizeof(s_state.cloud_private_key_der));
  s_state.cloud_private_key_der_len = 0;
  clear_cached_cloud_access_token_locked();
  clear_selected_machine_locked();
  clear_fleet_locked();
  mark_status_dirty_locked();
  unlock_state();

  return lm_ctrl_wifi_schedule_reboot();
}

esp_err_t lm_ctrl_wifi_factory_reset(void) {
  nvs_handle_t handle;
  esp_err_t ret = nvs_open(LM_CTRL_WIFI_NAMESPACE, NVS_READWRITE, &handle);

  if (ret == ESP_OK) {
    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
      ret = nvs_commit(handle);
    }
    nvs_close(handle);
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    return ret;
  }

  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    ret = ESP_OK;
  }
  if (ret != ESP_OK) {
    return ret;
  }

  ret = ctrl_state_reset_persisted();
  if (ret != ESP_OK) {
    return ret;
  }

  stop_cloud_websocket(false);
  lock_state();
  copy_text(s_state.hostname, sizeof(s_state.hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  s_state.language = CTRL_LANGUAGE_EN;
  s_state.sta_ssid[0] = '\0';
  s_state.sta_password[0] = '\0';
  s_state.cloud_username[0] = '\0';
  s_state.cloud_password[0] = '\0';
  s_state.has_credentials = false;
  s_state.has_cloud_credentials = false;
  s_state.sta_connected = false;
  s_state.sta_connecting = false;
  s_state.sta_ip[0] = '\0';
  s_state.cloud_installation_ready = false;
  s_state.cloud_installation_registered = false;
  s_state.cloud_installation_id[0] = '\0';
  memset(s_state.cloud_secret, 0, sizeof(s_state.cloud_secret));
  memset(s_state.cloud_private_key_der, 0, sizeof(s_state.cloud_private_key_der));
  s_state.cloud_private_key_der_len = 0;
  clear_cached_cloud_access_token_locked();
  clear_selected_machine_locked();
  clear_custom_logo_locked();
  clear_fleet_locked();
  mark_status_dirty_locked();
  unlock_state();
  update_mdns_hostname();

  return lm_ctrl_wifi_schedule_reboot();
}

static esp_err_t configure_ap(void) {
  wifi_config_t ap_config = {0};
  char portal_ssid[33];
  char portal_password[65];

  lock_state();
  copy_text(portal_ssid, sizeof(portal_ssid), s_state.portal_ssid);
  copy_text(portal_password, sizeof(portal_password), s_state.portal_password);
  unlock_state();

  copy_text((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), portal_ssid);
  copy_text((char *)ap_config.ap.password, sizeof(ap_config.ap.password), portal_password);
  ap_config.ap.ssid_len = strlen(portal_ssid);
  ap_config.ap.channel = 1;
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_config.ap.pmf_cfg.required = false;

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Failed to set Wi-Fi mode for AP");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "Failed to configure setup AP");
  return ESP_OK;
}

static esp_err_t apply_station_credentials(void) {
  wifi_config_t sta_config = {0};
  wifi_mode_t mode;
  char ssid[33];
  char password[65];
  char hostname[33];

  lock_state();
  if (!s_state.has_credentials) {
    unlock_state();
    return ESP_ERR_INVALID_STATE;
  }
  copy_text(ssid, sizeof(ssid), s_state.sta_ssid);
  copy_text(password, sizeof(password), s_state.sta_password);
  copy_text(hostname, sizeof(hostname), s_state.hostname);
  mode = s_state.portal_running ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  unlock_state();

  copy_text((char *)sta_config.sta.ssid, sizeof(sta_config.sta.ssid), ssid);
  copy_text((char *)sta_config.sta.password, sizeof(sta_config.sta.password), password);
  sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
  sta_config.sta.pmf_cfg.capable = true;
  sta_config.sta.pmf_cfg.required = false;

  if (s_state.sta_netif != NULL) {
    esp_netif_set_hostname(s_state.sta_netif, hostname[0] != '\0' ? hostname : LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "Failed to set Wi-Fi mode for STA");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "Failed to configure station");
  ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "Failed to start station connect");
  return ESP_OK;
}

static esp_err_t disable_setup_ap(void) {
  bool should_disable = false;

  lock_state();
  should_disable = s_state.portal_running;
  unlock_state();
  if (!should_disable) {
    return ESP_OK;
  }

  stop_dns_server();
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to disable setup AP");

  lock_state();
  if (s_state.portal_running) {
    s_state.portal_running = false;
    mark_status_dirty_locked();
  }
  unlock_state();
  ESP_LOGI(TAG, "Setup AP disabled after STA connection");
  return ESP_OK;
}

static esp_err_t update_mdns_hostname(void) {
  char hostname[33];
  esp_err_t ret;

  lock_state();
  copy_text(hostname, sizeof(hostname), s_state.hostname);
  unlock_state();

  if (hostname[0] == '\0') {
    copy_text(hostname, sizeof(hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  }

  if (!s_state.mdns_started) {
    ret = mdns_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
      return ret;
    }
    s_state.mdns_started = true;
  }

  ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "Failed to set mDNS hostname");
  ESP_RETURN_ON_ERROR(mdns_instance_name_set("La Marzocco Controller"), TAG, "Failed to set mDNS instance");
  if (s_state.mdns_started) {
    ret = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
      return ret;
    }
  }
  return ESP_OK;
}

static void format_portal_summary(const lm_ctrl_wifi_info_t *info, char *buffer, size_t buffer_size) {
  if (info == NULL || buffer == NULL || buffer_size == 0) {
    return;
  }

  if (info->sta_connected) {
    snprintf(
      buffer,
      buffer_size,
      "Home Wi-Fi: %.32s\nState: connected\nCurrent IP: %.15s\nStable URL: http://%.32s.local/\nCloud: %.47s\nHeader logo: %s",
      info->sta_ssid[0] != '\0' ? info->sta_ssid : "not set",
      info->sta_ip[0] != '\0' ? info->sta_ip : "--",
      info->hostname[0] != '\0' ? info->hostname : LM_CTRL_WIFI_DEFAULT_HOSTNAME,
      info->has_cloud_credentials ? info->cloud_username : "not configured",
      info->has_custom_logo ? "custom" : "default text"
    );
  } else if (info->sta_connecting) {
    snprintf(
      buffer,
      buffer_size,
      "Home Wi-Fi: %.32s\nState: connecting...\nStable URL: http://%.32s.local/\nCloud: %.47s\nHeader logo: %s",
      info->sta_ssid[0] != '\0' ? info->sta_ssid : "not set",
      info->hostname[0] != '\0' ? info->hostname : LM_CTRL_WIFI_DEFAULT_HOSTNAME,
      info->has_cloud_credentials ? info->cloud_username : "not configured",
      info->has_custom_logo ? "custom" : "default text"
    );
  } else if (info->has_credentials) {
    snprintf(
      buffer,
      buffer_size,
      "Home Wi-Fi: %.32s\nState: saved\nStable URL: http://%.32s.local/\nCloud: %.47s\nHeader logo: %s",
      info->sta_ssid[0] != '\0' ? info->sta_ssid : "not set",
      info->hostname[0] != '\0' ? info->hostname : LM_CTRL_WIFI_DEFAULT_HOSTNAME,
      info->has_cloud_credentials ? info->cloud_username : "not configured",
      info->has_custom_logo ? "custom" : "default text"
    );
  } else {
    snprintf(
      buffer,
      buffer_size,
      "Home Wi-Fi: not configured\nStable URL: http://%.32s.local/\nCloud: %.47s\nHeader logo: %s",
      info->hostname[0] != '\0' ? info->hostname : LM_CTRL_WIFI_DEFAULT_HOSTNAME,
      info->has_cloud_credentials ? info->cloud_username : "not configured",
      info->has_custom_logo ? "custom" : "default text"
    );
  }
}

static void format_debug_summary(
  const lm_ctrl_wifi_info_t *wifi_info,
  const lm_ctrl_machine_link_info_t *machine_info,
  const char *machine_status,
  char *buffer,
  size_t buffer_size
) {
  if (wifi_info == NULL || machine_info == NULL || buffer == NULL || buffer_size == 0) {
    return;
  }

  snprintf(
    buffer,
    buffer_size,
    "Portal: %s\n"
    "Station: %s\n"
    "IP: %.15s\n"
    "Cloud account: %.47s\n"
    "Machine selected: %.31s\n"
    "Header logo: %s\n"
    "BLE connected: %s\n"
    "BLE authenticated: %s\n"
    "Pending mask: 0x%02x\n"
    "Sync flags: 0x%02x\n"
    "Loaded mask: 0x%02x\n"
    "Feature mask: 0x%02x\n"
    "Link status: %.63s",
    wifi_info->portal_running ? "running" : "off",
    wifi_info->sta_connected ? "connected" : (wifi_info->sta_connecting ? "connecting" : "idle"),
    wifi_info->sta_ip[0] != '\0' ? wifi_info->sta_ip : "--",
    wifi_info->cloud_connected ? "connected" : (wifi_info->has_cloud_credentials ? "configured" : "not configured"),
    wifi_info->has_machine_selection ? wifi_info->machine_serial : "no",
    wifi_info->has_custom_logo ? "custom" : "default text",
    machine_info->connected ? "yes" : "no",
    machine_info->authenticated ? "yes" : "no",
    (unsigned)machine_info->pending_mask,
    (unsigned)machine_info->sync_flags,
    (unsigned)machine_info->loaded_mask,
    (unsigned)machine_info->feature_mask,
    machine_status != NULL && machine_status[0] != '\0' ? machine_status : "idle"
  );
}

static void format_controller_status_en(const lm_ctrl_wifi_info_t *info, char *buffer, size_t buffer_size) {
  const char *portal_ip;

  if (info == NULL || buffer == NULL || buffer_size == 0) {
    return;
  }

  portal_ip = (info->sta_connected && info->sta_ip[0] != '\0') ? info->sta_ip : LM_CTRL_PORTAL_IP;
  if (info->portal_running) {
    if (info->portal_password[0] != '\0') {
      snprintf(
        buffer,
        buffer_size,
        "AP: %s\nPassword: %s\nIP: %s",
        info->portal_ssid,
        info->portal_password,
        portal_ip
      );
    } else {
      snprintf(buffer, buffer_size, "AP: %s\nIP: %s", info->portal_ssid, portal_ip);
    }
  } else if (info->sta_connected) {
    snprintf(buffer, buffer_size, "IP: %s", info->sta_ip[0] != '\0' ? info->sta_ip : "n/a");
  } else if (info->sta_connecting) {
    snprintf(
      buffer,
      buffer_size,
      "Wi-Fi: connecting...\nIP: pending"
    );
  } else {
    snprintf(buffer, buffer_size, "IP: unavailable");
  }
}

static void format_controller_status_de(const lm_ctrl_wifi_info_t *info, char *buffer, size_t buffer_size) {
  const char *portal_ip;

  if (info == NULL || buffer == NULL || buffer_size == 0) {
    return;
  }

  portal_ip = (info->sta_connected && info->sta_ip[0] != '\0') ? info->sta_ip : LM_CTRL_PORTAL_IP;
  if (info->portal_running) {
    if (info->portal_password[0] != '\0') {
      snprintf(
        buffer,
        buffer_size,
        "AP: %s\nPasswort: %s\nIP: %s",
        info->portal_ssid,
        info->portal_password,
        portal_ip
      );
    } else {
      snprintf(buffer, buffer_size, "AP: %s\nIP: %s", info->portal_ssid, portal_ip);
    }
  } else if (info->sta_connected) {
    snprintf(buffer, buffer_size, "IP: %s", info->sta_ip[0] != '\0' ? info->sta_ip : "n/a");
  } else if (info->sta_connecting) {
    snprintf(
      buffer,
      buffer_size,
      "WLAN: verbindet...\nIP: ausstehend"
    );
  } else {
    snprintf(buffer, buffer_size, "IP: nicht verfügbar");
  }
}

static void ensure_station_dns(void) {
  esp_netif_dns_info_t dns_main = {0};
  esp_netif_dns_info_t dns_backup = {0};
  esp_netif_dns_info_t dhcp_dns = {0};

  if (s_state.sta_netif == NULL) {
    return;
  }

  if (esp_netif_get_dns_info(s_state.sta_netif, ESP_NETIF_DNS_MAIN, &dhcp_dns) == ESP_OK &&
      dhcp_dns.ip.type == ESP_IPADDR_TYPE_V4 &&
      dhcp_dns.ip.u_addr.ip4.addr != 0) {
    ESP_LOGI(TAG, "STA DNS from DHCP: " IPSTR, IP2STR(&dhcp_dns.ip.u_addr.ip4));
  }

  dns_main.ip.u_addr.ip4.addr = ipaddr_addr("1.1.1.1");
  dns_main.ip.type = ESP_IPADDR_TYPE_V4;
  if (esp_netif_set_dns_info(s_state.sta_netif, ESP_NETIF_DNS_MAIN, &dns_main) == ESP_OK) {
    ESP_LOGI(TAG, "STA DNS main forced to 1.1.1.1");
  }

  dns_backup.ip.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
  dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
  if (esp_netif_set_dns_info(s_state.sta_netif, ESP_NETIF_DNS_BACKUP, &dns_backup) == ESP_OK) {
    ESP_LOGI(TAG, "STA DNS backup forced to 8.8.8.8");
  }
}

static esp_err_t wait_for_dns_task_exit(int timeout_ms) {
  const int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);

  while (esp_timer_get_time() < deadline_us) {
    TaskHandle_t dns_task = NULL;

    lock_state();
    dns_task = s_state.dns_task;
    unlock_state();
    if (dns_task == NULL) {
      return ESP_OK;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  return ESP_ERR_TIMEOUT;
}

static void dns_server_task(void *arg) {
  char buffer[512];
  const uint32_t portal_addr = ipaddr_addr(LM_CTRL_PORTAL_IP);
  const int socket_fd = (int)(intptr_t)arg;
  TaskHandle_t current_task;

  current_task = xTaskGetCurrentTaskHandle();

  while (true) {
    struct sockaddr_in client_addr = {0};
    socklen_t client_addr_len = sizeof(client_addr);
    int len;

    len = recvfrom(socket_fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);
    if (len < 0) {
      if (errno == EBADF || errno == ENOTSOCK) {
        break;
      }
      continue;
    }
    if (len < 12 || (buffer[2] & 0x80) != 0 || (len + 16) > (int)sizeof(buffer)) {
      continue;
    }

    buffer[2] |= 0x80;
    buffer[3] |= 0x80;
    buffer[7] = 1;

    memcpy(&buffer[len], "\xc0\x0c", 2);
    len += 2;
    memcpy(&buffer[len], "\x00\x01\x00\x01\x00\x00\x00\x1c\x00\x04", 10);
    len += 10;
    memcpy(&buffer[len], &portal_addr, 4);
    len += 4;

    (void)sendto(socket_fd, buffer, len, 0, (struct sockaddr *)&client_addr, client_addr_len);
  }

  lock_state();
  if (s_state.dns_task == current_task) {
    s_state.dns_task = NULL;
  }
  unlock_state();
  vTaskDelete(NULL);
}

static esp_err_t start_dns_server(void) {
  struct sockaddr_in server_addr = {0};
  TaskHandle_t dns_task = NULL;
  int existing_socket = -1;
  int socket_fd;

  lock_state();
  dns_task = s_state.dns_task;
  existing_socket = s_state.dns_socket;
  unlock_state();
  if (dns_task != NULL) {
    if (existing_socket >= 0) {
      return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(wait_for_dns_task_exit(500), TAG, "Timed out waiting for DNS task shutdown");
  }

  socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0) {
    return ESP_FAIL;
  }

  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(LM_CTRL_PORTAL_DNS_PORT);

  if (bind(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    close(socket_fd);
    return ESP_FAIL;
  }

  lock_state();
  s_state.dns_socket = socket_fd;
  unlock_state();

  if (xTaskCreate(dns_server_task, "lm_portal_dns", 4096, (void *)(intptr_t)socket_fd, 5, &s_state.dns_task) != pdPASS) {
    lock_state();
    s_state.dns_socket = -1;
    unlock_state();
    close(socket_fd);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Captive DNS started on %s:%d", LM_CTRL_PORTAL_IP, LM_CTRL_PORTAL_DNS_PORT);
  return ESP_OK;
}

static void stop_dns_server(void) {
  int socket_fd = -1;

  lock_state();
  socket_fd = s_state.dns_socket;
  s_state.dns_socket = -1;
  unlock_state();

  if (socket_fd >= 0) {
    shutdown(socket_fd, SHUT_RDWR);
    close(socket_fd);
  }

  if (wait_for_dns_task_exit(500) != ESP_OK) {
    ESP_LOGW(TAG, "DNS task did not exit within timeout");
  }
}

static esp_err_t form_url_decode_segment(const char *src, size_t src_len, char *dst, size_t dst_size) {
  size_t out = 0;

  if (src == NULL || dst == NULL || dst_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  for (size_t i = 0; i < src_len && out + 1 < dst_size; ++i) {
    if (src[i] == '+') {
      dst[out++] = ' ';
      continue;
    }

    if (
      src[i] == '%' &&
      i + 2 < src_len &&
      isxdigit((unsigned char)src[i + 1]) &&
      isxdigit((unsigned char)src[i + 2])
    ) {
      char hex[3] = {src[i + 1], src[i + 2], '\0'};
      dst[out++] = (char)strtol(hex, NULL, 16);
      i += 2;
      continue;
    }

    dst[out++] = src[i];
  }

  dst[out] = '\0';
  return ESP_OK;
}

static bool parse_form_value(const char *body, const char *key, char *dst, size_t dst_size) {
  const size_t key_len = strlen(key);
  const char *cursor = body;

  if (body == NULL || key == NULL || dst == NULL || dst_size == 0) {
    return false;
  }

  while (cursor != NULL && *cursor != '\0') {
    const char *next = strchr(cursor, '&');
    const char *equals = strchr(cursor, '=');
    size_t segment_len = next != NULL ? (size_t)(next - cursor) : strlen(cursor);

    if (equals != NULL && (size_t)(equals - cursor) == key_len && strncmp(cursor, key, key_len) == 0) {
      const char *value = equals + 1;
      size_t value_len = segment_len - key_len - 1;

      if (form_url_decode_segment(value, value_len, dst, dst_size) != ESP_OK) {
        return false;
      }
      return true;
    }

    cursor = next != NULL ? next + 1 : NULL;
  }

  dst[0] = '\0';
  return false;
}

static void html_escape_text(const char *src, char *dst, size_t dst_size) {
  size_t out = 0;

  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  for (size_t i = 0; src[i] != '\0' && out + 1 < dst_size; ++i) {
    const char *replacement = NULL;
    switch (src[i]) {
      case '&':
        replacement = "&amp;";
        break;
      case '<':
        replacement = "&lt;";
        break;
      case '>':
        replacement = "&gt;";
        break;
      case '"':
        replacement = "&quot;";
        break;
      default:
        break;
    }

    if (replacement != NULL) {
      size_t repl_len = strlen(replacement);
      if (out + repl_len >= dst_size) {
        break;
      }
      memcpy(dst + out, replacement, repl_len);
      out += repl_len;
    } else {
      dst[out++] = src[i];
    }
  }

  dst[out] = '\0';
}

static void json_escape_text(const char *src, char *dst, size_t dst_size) {
  size_t out = 0;

  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  for (size_t i = 0; src[i] != '\0' && out + 1 < dst_size; ++i) {
    const char *replacement = NULL;

    switch (src[i]) {
      case '\\':
        replacement = "\\\\";
        break;
      case '"':
        replacement = "\\\"";
        break;
      case '\n':
        replacement = "\\n";
        break;
      case '\r':
        replacement = "\\r";
        break;
      case '\t':
        replacement = "\\t";
        break;
      default:
        break;
    }

    if (replacement != NULL) {
      size_t repl_len = strlen(replacement);
      if (out + repl_len >= dst_size) {
        break;
      }
      memcpy(dst + out, replacement, repl_len);
      out += repl_len;
    } else {
      dst[out++] = src[i];
    }
  }

  dst[out] = '\0';
}

static esp_err_t http_request_capture(
  const char *host,
  const char *path,
  int port,
  esp_http_client_method_t method,
  const lm_ctrl_cloud_http_header_t *headers,
  size_t header_count,
  const char *body,
  int timeout_ms,
  char **response_body,
  int *status_code
) {
  ESP_LOGI(
    TAG,
    "HTTP request: %s%s heap=%u internal=%u largest_internal=%u",
    host,
    path,
    (unsigned)esp_get_free_heap_size(),
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
  );
  mark_cloud_http_request_started();
  esp_err_t ret = lm_ctrl_cloud_http_request(
    host,
    path,
    port,
    method,
    headers,
    header_count,
    body,
    timeout_ms,
    response_body,
    status_code
  );
  mark_cloud_http_request_finished();
  if (ret != ESP_OK) {
    ESP_LOGE(
      TAG,
      "HTTP request failed for %s%s heap=%u internal=%u largest_internal=%u: %s",
      host,
      path,
      (unsigned)esp_get_free_heap_size(),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      esp_err_to_name(ret)
    );
  }
  return ret;
}

static esp_err_t ensure_cloud_registration(char *error_text, size_t error_text_size) {
  char installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t private_key_der_len = 0;
  char public_key_b64[256];
  char base_string[128];
  char request_proof[64];
  char *request_body = NULL;
  char *response_body = NULL;
  int status_code = 0;
  cJSON *body_root = NULL;
  lm_ctrl_cloud_http_header_t headers[3];
  esp_err_t ret;

  if (error_text != NULL && error_text_size > 0) {
    error_text[0] = '\0';
  }

  ret = ensure_cloud_installation();
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud installation setup failed.");
    }
    return ret;
  }

  lock_state();
  if (s_state.cloud_installation_registered) {
    unlock_state();
    return ESP_OK;
  }
  copy_text(installation_id, sizeof(installation_id), s_state.cloud_installation_id);
  memcpy(secret, s_state.cloud_secret, sizeof(secret));
  memcpy(private_key_der, s_state.cloud_private_key_der, s_state.cloud_private_key_der_len);
  private_key_der_len = s_state.cloud_private_key_der_len;
  unlock_state();

  ret = derive_installation_material(
    installation_id,
    private_key_der,
    private_key_der_len,
    public_key_b64,
    sizeof(public_key_b64),
    base_string,
    sizeof(base_string)
  );
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud key material could not be prepared.");
    }
    return ret;
  }

  ret = generate_request_proof_text(base_string, secret, request_proof, sizeof(request_proof));
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud registration proof failed.");
    }
    return ret;
  }

  body_root = cJSON_CreateObject();
  if (body_root == NULL) {
    return ESP_ERR_NO_MEM;
  }
  if (!cJSON_AddStringToObject(body_root, "pk", public_key_b64)) {
    cJSON_Delete(body_root);
    return ESP_ERR_NO_MEM;
  }
  request_body = cJSON_PrintUnformatted(body_root);
  cJSON_Delete(body_root);
  if (request_body == NULL) {
    return ESP_ERR_NO_MEM;
  }

  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "Content-Type", .value = "application/json" };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "X-App-Installation-Id", .value = installation_id };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "X-Request-Proof", .value = request_proof };

  ret = http_request_capture(
    LM_CTRL_CLOUD_HOST,
    LM_CTRL_CLOUD_AUTH_INIT_PATH,
    LM_CTRL_CLOUD_PORT,
    HTTP_METHOD_POST,
    headers,
    sizeof(headers) / sizeof(headers[0]),
    request_body,
    12000,
    &response_body,
    &status_code
  );
  free(request_body);
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud registration request failed.");
    }
    return ret;
  }

  if (status_code < 200 || status_code >= 300) {
    ESP_LOGW(TAG, "Cloud init failed with status %d: %.200s", status_code, response_body != NULL ? response_body : "");
    free(response_body);
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud registration failed with status %d.", status_code);
    }
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Cloud init ok with status %d", status_code);
  free(response_body);
  return set_cloud_installation_registration(true);
}

static esp_err_t build_signed_request_headers(
  const char *installation_id,
  const uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN],
  const uint8_t *private_key_der,
  size_t private_key_der_len,
  char *timestamp,
  size_t timestamp_size,
  char *nonce,
  size_t nonce_size,
  char *signature_b64,
  size_t signature_b64_size
) {
  lm_ctrl_cloud_installation_t installation = {0};

  if (
    installation_id == NULL ||
    secret == NULL ||
    private_key_der == NULL ||
    private_key_der_len == 0 ||
    timestamp == NULL ||
    nonce == NULL ||
    signature_b64 == NULL
  ) {
    return ESP_ERR_INVALID_ARG;
  }

  copy_text(installation.installation_id, sizeof(installation.installation_id), installation_id);
  memcpy(installation.secret, secret, sizeof(installation.secret));
  memcpy(installation.private_key_der, private_key_der, private_key_der_len);
  installation.private_key_der_len = private_key_der_len;

  return lm_ctrl_cloud_build_signed_request_headers(
    &installation,
    timestamp,
    timestamp_size,
    nonce,
    nonce_size,
    signature_b64,
    signature_b64_size
  );
}

static esp_err_t parse_cloud_access_token(
  const char *response_body,
  char *access_token,
  size_t access_token_size
) {
  return lm_ctrl_cloud_parse_access_token(response_body, access_token, access_token_size);
}

static void append_text(char *buffer, size_t buffer_size, const char *text) {
  size_t used;

  if (buffer == NULL || buffer_size == 0 || text == NULL || text[0] == '\0') {
    return;
  }

  used = strnlen(buffer, buffer_size);
  if (used >= buffer_size - 1) {
    return;
  }

  snprintf(buffer + used, buffer_size - used, "%s", text);
}

static void format_modes(cJSON *available_modes, char *buffer, size_t buffer_size) {
  cJSON *entry;
  bool first = true;

  if (buffer == NULL || buffer_size == 0) {
    return;
  }

  buffer[0] = '\0';
  if (!cJSON_IsArray(available_modes)) {
    return;
  }

  cJSON_ArrayForEach(entry, available_modes) {
    if (!cJSON_IsString(entry) || entry->valuestring == NULL) {
      continue;
    }
    if (!first) {
      append_text(buffer, buffer_size, ",");
    }
    append_text(buffer, buffer_size, entry->valuestring);
    first = false;
  }
}

static void log_widget_output(const char *code, cJSON *output) {
  char *printed;

  if (code == NULL || output == NULL) {
    return;
  }

  printed = cJSON_PrintUnformatted(output);
  if (printed == NULL) {
    ESP_LOGI(TAG, "Dashboard %s output: <print failed>", code);
    return;
  }

  ESP_LOGI(TAG, "Dashboard %s output: %s", code, printed);
  cJSON_free(printed);
}

static void summarize_prebrew_widget(
  cJSON *widget,
  char *summary,
  size_t summary_size,
  bool *found_summary
) {
  cJSON *code_item;
  cJSON *output;
  cJSON *mode_item;
  cJSON *available_modes;
  char modes_text[96];
  char local_summary[192];

  if (widget == NULL || summary == NULL || summary_size == 0 || found_summary == NULL) {
    return;
  }

  code_item = cJSON_GetObjectItemCaseSensitive(widget, "code");
  if (!cJSON_IsString(code_item) || code_item->valuestring == NULL) {
    return;
  }

  output = cJSON_GetObjectItemCaseSensitive(widget, "output");
  if (!cJSON_IsObject(output)) {
    return;
  }

  if (strcmp(code_item->valuestring, "CMPreExtraction") != 0 &&
      strcmp(code_item->valuestring, "CMPreBrewing") != 0 &&
      strcmp(code_item->valuestring, "CMPreInfusionEnable") != 0 &&
      strcmp(code_item->valuestring, "CMPreInfusion") != 0) {
    return;
  }

  log_widget_output(code_item->valuestring, output);

  if (*found_summary) {
    return;
  }

  mode_item = cJSON_GetObjectItemCaseSensitive(output, "mode");
  available_modes = cJSON_GetObjectItemCaseSensitive(output, "availableModes");
  format_modes(available_modes, modes_text, sizeof(modes_text));
  snprintf(
    local_summary,
    sizeof(local_summary),
    "%s mode=%s available=%s",
    code_item->valuestring,
    cJSON_IsString(mode_item) && mode_item->valuestring != NULL ? mode_item->valuestring : "-",
    modes_text[0] != '\0' ? modes_text : "-"
  );
  copy_text(summary, summary_size, local_summary);
  *found_summary = true;
}

static bool parse_prebrew_widget_values(cJSON *widget, float *seconds_in, float *seconds_out) {
  return lm_ctrl_cloud_parse_prebrew_widget_values(widget, seconds_in, seconds_out);
}

static esp_err_t parse_dashboard_root_values(
  cJSON *root,
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  bool *brew_active,
  int64_t *brew_start_epoch_ms
) {
  return lm_ctrl_cloud_parse_dashboard_root_values(
    root,
    values,
    loaded_mask,
    feature_mask,
    brew_active,
    brew_start_epoch_ms
  );
}

static lm_ctrl_cloud_command_status_t parse_cloud_command_status_string(const char *status) {
  if (status == NULL || status[0] == '\0') {
    return LM_CTRL_CLOUD_COMMAND_STATUS_UNKNOWN;
  }
  if (strcmp(status, "Success") == 0) {
    return LM_CTRL_CLOUD_COMMAND_STATUS_SUCCESS;
  }
  if (strcmp(status, "Error") == 0) {
    return LM_CTRL_CLOUD_COMMAND_STATUS_ERROR;
  }
  if (strcmp(status, "Timeout") == 0) {
    return LM_CTRL_CLOUD_COMMAND_STATUS_TIMEOUT;
  }
  if (strcmp(status, "Pending") == 0) {
    return LM_CTRL_CLOUD_COMMAND_STATUS_PENDING;
  }
  if (strcmp(status, "InProgress") == 0) {
    return LM_CTRL_CLOUD_COMMAND_STATUS_IN_PROGRESS;
  }
  return LM_CTRL_CLOUD_COMMAND_STATUS_UNKNOWN;
}

static size_t parse_dashboard_command_updates(
  cJSON *root,
  lm_ctrl_cloud_command_update_t *updates,
  size_t max_updates
) {
  cJSON *commands;
  cJSON *command;
  size_t count = 0;

  if (root == NULL || updates == NULL || max_updates == 0) {
    return 0;
  }

  commands = cJSON_GetObjectItemCaseSensitive(root, "commands");
  if (!cJSON_IsArray(commands)) {
    return 0;
  }

  cJSON_ArrayForEach(command, commands) {
    cJSON *id_item;
    cJSON *status_item;
    cJSON *error_code_item;

    if (count >= max_updates || !cJSON_IsObject(command)) {
      break;
    }

    id_item = cJSON_GetObjectItemCaseSensitive(command, "id");
    status_item = cJSON_GetObjectItemCaseSensitive(command, "status");
    error_code_item = cJSON_GetObjectItemCaseSensitive(command, "errorCode");
    if (!cJSON_IsString(id_item) || id_item->valuestring == NULL) {
      continue;
    }

    memset(&updates[count], 0, sizeof(updates[count]));
    copy_text(updates[count].command_id, sizeof(updates[count].command_id), id_item->valuestring);
    if (cJSON_IsString(error_code_item) && error_code_item->valuestring != NULL) {
      copy_text(updates[count].error_code, sizeof(updates[count].error_code), error_code_item->valuestring);
    }
    updates[count].status =
      cJSON_IsString(status_item) ? parse_cloud_command_status_string(status_item->valuestring)
                                  : LM_CTRL_CLOUD_COMMAND_STATUS_UNKNOWN;
    count++;
  }

  return count;
}

static esp_err_t fetch_cloud_access_token(
  const char *username,
  const char *password,
  char *access_token,
  size_t access_token_size,
  char *error_text,
  size_t error_text_size
) {
  char installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t private_key_der_len = 0;
  char timestamp[24];
  char nonce[LM_CTRL_INSTALLATION_ID_LEN];
  char signature_b64[256];
  char *request_body = NULL;
  char *response_body = NULL;
  cJSON *body_root = NULL;
  int status_code = 0;
  esp_err_t ret;

  if (error_text != NULL && error_text_size > 0) {
    error_text[0] = '\0';
  }

  for (int attempt = 0; attempt < 2; ++attempt) {
    lm_ctrl_cloud_http_header_t headers[5];

    ret = ensure_cloud_registration(error_text, error_text_size);
    if (ret != ESP_OK) {
      return ret;
    }

    lock_state();
    copy_text(installation_id, sizeof(installation_id), s_state.cloud_installation_id);
    memcpy(secret, s_state.cloud_secret, sizeof(secret));
    memcpy(private_key_der, s_state.cloud_private_key_der, s_state.cloud_private_key_der_len);
    private_key_der_len = s_state.cloud_private_key_der_len;
    unlock_state();

    ret = build_signed_request_headers(
      installation_id,
      secret,
      private_key_der,
      private_key_der_len,
      timestamp,
      sizeof(timestamp),
      nonce,
      sizeof(nonce),
      signature_b64,
      sizeof(signature_b64)
    );
    if (ret != ESP_OK) {
      if (error_text != NULL && error_text_size > 0) {
        snprintf(error_text, error_text_size, "Cloud request signing failed.");
      }
      return ret;
    }

    body_root = cJSON_CreateObject();
    if (body_root == NULL) {
      return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(body_root, "username", username) || !cJSON_AddStringToObject(body_root, "password", password)) {
      cJSON_Delete(body_root);
      return ESP_ERR_NO_MEM;
    }
    request_body = cJSON_PrintUnformatted(body_root);
    cJSON_Delete(body_root);
    if (request_body == NULL) {
      return ESP_ERR_NO_MEM;
    }

    headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "Content-Type", .value = "application/json" };
    headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "X-App-Installation-Id", .value = installation_id };
    headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "X-Timestamp", .value = timestamp };
    headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "X-Nonce", .value = nonce };
    headers[4] = (lm_ctrl_cloud_http_header_t){ .name = "X-Request-Signature", .value = signature_b64 };

    ret = http_request_capture(
      LM_CTRL_CLOUD_HOST,
      LM_CTRL_CLOUD_AUTH_SIGNIN_PATH,
      LM_CTRL_CLOUD_PORT,
      HTTP_METHOD_POST,
      headers,
      sizeof(headers) / sizeof(headers[0]),
      request_body,
      12000,
      &response_body,
      &status_code
    );
    free(request_body);
    request_body = NULL;

    if (ret != ESP_OK) {
      set_cloud_connected(false);
      if (error_text != NULL && error_text_size > 0) {
        snprintf(error_text, error_text_size, "Cloud login request failed.");
      }
      return ret;
    }

    if (status_code == 401) {
      ESP_LOGW(TAG, "Cloud signin rejected with status 401: %.200s", response_body != NULL ? response_body : "");
      free(response_body);
      set_cloud_connected(false);
      if (error_text != NULL && error_text_size > 0) {
        snprintf(error_text, error_text_size, "Cloud login failed. Check e-mail and password.");
      }
      return ESP_ERR_INVALID_RESPONSE;
    }
    if (status_code == 412 && attempt == 0) {
      ESP_LOGW(TAG, "Cloud signin needs re-init: %.200s", response_body != NULL ? response_body : "");
      free(response_body);
      response_body = NULL;
      set_cloud_installation_registration(false);
      continue;
    }
    if (status_code < 200 || status_code >= 300) {
      ESP_LOGW(TAG, "Cloud signin failed with status %d: %.200s", status_code, response_body != NULL ? response_body : "");
      free(response_body);
      set_cloud_connected(false);
      if (error_text != NULL && error_text_size > 0) {
        snprintf(error_text, error_text_size, "Cloud login failed with status %d.", status_code);
      }
      return ESP_FAIL;
    }

    ret = parse_cloud_access_token(response_body, access_token, access_token_size);
    free(response_body);
    set_cloud_connected(ret == ESP_OK);
    if (ret == ESP_OK) {
      store_cached_cloud_access_token(access_token);
    }
    if (ret != ESP_OK && error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Cloud login response could not be parsed.");
    }
    return ret;
  }

  if (error_text != NULL && error_text_size > 0) {
    snprintf(error_text, error_text_size, "Cloud installation registration retry failed.");
  }
  set_cloud_connected(false);
  return ESP_FAIL;
}

static esp_err_t fetch_cloud_access_token_cached(
  const char *username,
  const char *password,
  char *access_token,
  size_t access_token_size,
  char *error_text,
  size_t error_text_size
) {
  SemaphoreHandle_t auth_lock = NULL;

  if (access_token == NULL || access_token_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (copy_cached_cloud_access_token(access_token, access_token_size)) {
    if (error_text != NULL && error_text_size > 0) {
      error_text[0] = '\0';
    }
    return ESP_OK;
  }

  lock_state();
  auth_lock = s_state.cloud_auth_lock;
  unlock_state();

  if (auth_lock != NULL) {
    xSemaphoreTake(auth_lock, portMAX_DELAY);
  }

  if (copy_cached_cloud_access_token(access_token, access_token_size)) {
    if (auth_lock != NULL) {
      xSemaphoreGive(auth_lock);
    }
    if (error_text != NULL && error_text_size > 0) {
      error_text[0] = '\0';
    }
    return ESP_OK;
  }

  esp_err_t ret = fetch_cloud_access_token(username, password, access_token, access_token_size, error_text, error_text_size);

  if (auth_lock != NULL) {
    xSemaphoreGive(auth_lock);
  }

  return ret;
}

static void cloud_probe_task(void *arg) {
  char username[96];
  char password[128];
  char access_token[768];
  bool should_probe = false;
  TaskHandle_t current_task;

  (void)arg;
  current_task = xTaskGetCurrentTaskHandle();

  lock_state();
  should_probe = s_state.sta_connected && s_state.has_cloud_credentials;
  copy_text(username, sizeof(username), s_state.cloud_username);
  copy_text(password, sizeof(password), s_state.cloud_password);
  unlock_state();

  if (!should_probe || username[0] == '\0' || password[0] == '\0') {
    set_cloud_connected(false);
  } else {
    char error_text[160];

    error_text[0] = '\0';
    (void)fetch_cloud_access_token_cached(username, password, access_token, sizeof(access_token), error_text, sizeof(error_text));
  }

  lock_state();
  if (s_state.cloud_probe_task == current_task) {
    s_state.cloud_probe_task = NULL;
  }
  unlock_state();
  (void)ensure_cloud_websocket_task();
  vTaskDelete(NULL);
}

static bool should_run_cloud_websocket_locked(void) {
  if (LM_CTRL_ENABLE_CLOUD_LIVE_UPDATES == 0) {
    return false;
  }
  return s_state.initialized &&
         s_state.sta_connected &&
         s_state.has_cloud_credentials &&
         s_state.has_machine_selection;
}

static bool can_start_cloud_websocket_locked(void) {
  return should_run_cloud_websocket_locked() &&
         s_state.cloud_probe_task == NULL &&
         s_state.cloud_http_requests_in_flight == 0;
}

static void append_ws_header_line(char *buffer, size_t buffer_size, const char *name, const char *value) {
  if (buffer == NULL || buffer_size == 0 || name == NULL || value == NULL) {
    return;
  }
  if (buffer[0] != '\0') {
    append_text(buffer, buffer_size, "\r\n");
  }
  append_text(buffer, buffer_size, name);
  append_text(buffer, buffer_size, ": ");
  append_text(buffer, buffer_size, value);
}

static void build_stomp_frame(
  char *buffer,
  size_t buffer_size,
  const char *command,
  const lm_ctrl_cloud_http_header_t *headers,
  size_t header_count,
  const char *body
) {
  if (buffer == NULL || buffer_size == 0 || command == NULL) {
    return;
  }

  snprintf(buffer, buffer_size, "%s\n", command);
  for (size_t i = 0; i < header_count; ++i) {
    if (headers[i].name == NULL || headers[i].value == NULL) {
      continue;
    }
    append_text(buffer, buffer_size, headers[i].name);
    append_text(buffer, buffer_size, ":");
    append_text(buffer, buffer_size, headers[i].value);
    append_text(buffer, buffer_size, "\n");
  }
  append_text(buffer, buffer_size, "\n");
  if (body != NULL && body[0] != '\0') {
    append_text(buffer, buffer_size, body);
  }
  append_text(buffer, buffer_size, "\x00");
}

static bool parse_stomp_frame(char *frame, const char **command, char **body) {
  char *separator;
  char *nul;

  if (frame == NULL || command == NULL || body == NULL) {
    return false;
  }

  nul = strrchr(frame, '\x00');
  if (nul != NULL) {
    *nul = '\0';
  }

  separator = strstr(frame, "\n\n");
  if (separator == NULL) {
    return false;
  }

  *separator = '\0';
  *command = frame;
  *body = separator + 2;
  return true;
}

static void handle_cloud_dashboard_message(const char *message) {
  cJSON *root = NULL;
  ctrl_values_t values = {0};
  uint32_t loaded_mask = 0;
  uint32_t feature_mask = 0;
  bool brew_active = false;
  int64_t brew_start_epoch_ms = 0;
  lm_ctrl_cloud_command_update_t updates[LM_CTRL_CLOUD_COMMAND_UPDATE_MAX] = {0};
  size_t update_count = 0;

  if (message == NULL || message[0] == '\0') {
    return;
  }

  root = cJSON_Parse(message);
  if (root == NULL) {
    ESP_LOGW(TAG, "Cloud websocket JSON parse failed");
    return;
  }

  update_count = parse_dashboard_command_updates(root, updates, LM_CTRL_CLOUD_COMMAND_UPDATE_MAX);

  if (parse_dashboard_root_values(root, &values, &loaded_mask, &feature_mask, &brew_active, &brew_start_epoch_ms) == ESP_OK) {
    lm_ctrl_machine_link_apply_cloud_dashboard_values(&values, loaded_mask, feature_mask);
    set_brew_timer_state(brew_active, brew_start_epoch_ms);
  }
  if (update_count != 0) {
    lm_ctrl_machine_link_apply_cloud_command_updates(updates, update_count);
  }

  cJSON_Delete(root);
}

static void send_cloud_ws_connect_frame(esp_websocket_client_handle_t client) {
  char auth_header[LM_CTRL_CLOUD_WS_TOKEN_LEN + 8];
  char frame[1536];
  lm_ctrl_cloud_http_header_t headers[4];
  size_t frame_len;

  if (client == NULL) {
    return;
  }

  lock_state();
  snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_state.cloud_ws_access_token);
  unlock_state();

  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "host", .value = LM_CTRL_CLOUD_HOST };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "accept-version", .value = "1.2,1.1,1.0" };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "heart-beat", .value = "0,0" };
  headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "Authorization", .value = auth_header };
  build_stomp_frame(frame, sizeof(frame), "CONNECT", headers, 4, NULL);
  frame_len = strnlen(frame, sizeof(frame) - 1) + 1;
  (void)esp_websocket_client_send_text(client, frame, frame_len, portMAX_DELAY);
}

static void send_cloud_ws_subscribe_frame(esp_websocket_client_handle_t client) {
  char destination[128];
  char serial[32];
  char frame[512];
  lm_ctrl_cloud_http_header_t headers[4];
  size_t frame_len;

  if (client == NULL) {
    return;
  }

  lock_state();
  copy_text(serial, sizeof(serial), s_state.selected_machine.serial);
  unlock_state();
  snprintf(destination, sizeof(destination), "%s%s%s", LM_CTRL_CLOUD_WS_DEST_PREFIX, serial, LM_CTRL_CLOUD_WS_DEST_SUFFIX);

  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "destination", .value = destination };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "ack", .value = "auto" };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "id", .value = LM_CTRL_CLOUD_WS_SUBSCRIPTION_ID };
  headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "content-length", .value = "0" };
  build_stomp_frame(frame, sizeof(frame), "SUBSCRIBE", headers, 4, NULL);
  frame_len = strnlen(frame, sizeof(frame) - 1) + 1;
  (void)esp_websocket_client_send_text(client, frame, frame_len, portMAX_DELAY);
}

static void cloud_ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)handler_args;
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

  (void)base;

  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    ESP_LOGI(TAG, "Cloud websocket transport connected");
    lock_state();
    s_state.cloud_ws_transport_connected = true;
    s_state.cloud_ws_connected = false;
    s_state.cloud_ws_frame_len = 0;
    mark_status_dirty_locked();
    unlock_state();
    send_cloud_ws_connect_frame(client);
    return;
  }

  if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
    ESP_LOGI(TAG, "Cloud websocket disconnected");
    set_cloud_websocket_connected(false, false);
    set_brew_timer_state(false, 0);
    lm_ctrl_machine_link_handle_cloud_websocket_disconnect();
    return;
  }

  if (event_id != WEBSOCKET_EVENT_DATA || data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
    return;
  }

  lock_state();
  if (data->payload_offset == 0) {
    s_state.cloud_ws_frame_len = 0;
  }
  if ((size_t)data->payload_offset + (size_t)data->data_len < sizeof(s_state.cloud_ws_frame)) {
    memcpy(s_state.cloud_ws_frame + data->payload_offset, data->data_ptr, data->data_len);
    s_state.cloud_ws_frame_len = (size_t)data->payload_offset + (size_t)data->data_len;
    s_state.cloud_ws_frame[s_state.cloud_ws_frame_len] = '\0';
  } else {
    s_state.cloud_ws_frame_len = 0;
  }
  unlock_state();

  if ((size_t)data->payload_offset + (size_t)data->data_len < (size_t)data->payload_len) {
    return;
  }

  {
    char *frame_copy = NULL;
    const char *command = NULL;
    char *body = NULL;

    frame_copy = malloc(LM_CTRL_CLOUD_WS_FRAME_MAX);
    if (frame_copy == NULL) {
      ESP_LOGW(TAG, "Cloud websocket frame allocation failed");
      return;
    }

    lock_state();
    copy_text(frame_copy, LM_CTRL_CLOUD_WS_FRAME_MAX, s_state.cloud_ws_frame);
    unlock_state();

    if (!parse_stomp_frame(frame_copy, &command, &body) || command == NULL) {
      ESP_LOGW(TAG, "Cloud websocket STOMP frame parse failed");
      free(frame_copy);
      return;
    }

    if (strcmp(command, "CONNECTED") == 0) {
      ESP_LOGI(TAG, "Cloud websocket STOMP connected");
      set_cloud_websocket_connected(true, true);
      send_cloud_ws_subscribe_frame(client);
    } else if (strcmp(command, "MESSAGE") == 0) {
      handle_cloud_dashboard_message(body);
    } else if (strcmp(command, "ERROR") == 0) {
      ESP_LOGW(TAG, "Cloud websocket STOMP error: %s", body != NULL ? body : "");
    }

    free(frame_copy);
  }
}

static esp_err_t build_cloud_ws_headers(char *buffer, size_t buffer_size) {
  char installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t private_key_der_len = 0;
  char timestamp[24];
  char nonce[LM_CTRL_INSTALLATION_ID_LEN];
  char signature_b64[256];
  esp_err_t ret;

  if (buffer == NULL || buffer_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  buffer[0] = '\0';
  ret = ensure_cloud_installation();
  if (ret != ESP_OK) {
    return ret;
  }

  lock_state();
  copy_text(installation_id, sizeof(installation_id), s_state.cloud_installation_id);
  memcpy(secret, s_state.cloud_secret, sizeof(secret));
  memcpy(private_key_der, s_state.cloud_private_key_der, s_state.cloud_private_key_der_len);
  private_key_der_len = s_state.cloud_private_key_der_len;
  unlock_state();

  ret = build_signed_request_headers(
    installation_id,
    secret,
    private_key_der,
    private_key_der_len,
    timestamp,
    sizeof(timestamp),
    nonce,
    sizeof(nonce),
    signature_b64,
    sizeof(signature_b64)
  );
  if (ret != ESP_OK) {
    return ret;
  }

  append_ws_header_line(buffer, buffer_size, "X-App-Installation-Id", installation_id);
  append_ws_header_line(buffer, buffer_size, "X-Timestamp", timestamp);
  append_ws_header_line(buffer, buffer_size, "X-Nonce", nonce);
  append_ws_header_line(buffer, buffer_size, "X-Request-Signature", signature_b64);
  return ESP_OK;
}

static void cloud_websocket_task(void *arg) {
  char username[96];
  char password[128];
  char access_token[LM_CTRL_CLOUD_WS_TOKEN_LEN];
  char ws_headers[LM_CTRL_CLOUD_WS_HEADER_BUFFER_LEN];

  (void)arg;

  while (true) {
    esp_websocket_client_config_t ws_config = {
      .uri = LM_CTRL_CLOUD_WS_URI,
      .headers = ws_headers,
      .task_stack = 4096,
      .buffer_size = 512,
      .reconnect_timeout_ms = 0,
      .disable_auto_reconnect = true,
      .network_timeout_ms = 10000,
      .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_websocket_client_handle_t client = NULL;
    esp_err_t ret;

    lock_state();
    if (s_state.cloud_ws_stop_requested || !should_run_cloud_websocket_locked()) {
      unlock_state();
      break;
    }
    if (s_state.cloud_probe_task != NULL || s_state.cloud_http_requests_in_flight != 0) {
      unlock_state();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    copy_text(username, sizeof(username), s_state.cloud_username);
    copy_text(password, sizeof(password), s_state.cloud_password);
    unlock_state();

    ESP_LOGI(
      TAG,
      "Cloud websocket attempt: heap=%u internal=%u largest_internal=%u",
      (unsigned)esp_get_free_heap_size(),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
    );

    ret = fetch_cloud_access_token_cached(username, password, access_token, sizeof(access_token), NULL, 0);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Cloud websocket waiting for access token: %s", esp_err_to_name(ret));
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    ret = build_cloud_ws_headers(ws_headers, sizeof(ws_headers));
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Cloud websocket header setup failed: %s", esp_err_to_name(ret));
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    client = esp_websocket_client_init(&ws_config);
    if (client == NULL) {
      ESP_LOGW(TAG, "Cloud websocket client init failed");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    lock_state();
    copy_text(s_state.cloud_ws_access_token, sizeof(s_state.cloud_ws_access_token), access_token);
    s_state.cloud_ws_client = client;
    s_state.cloud_ws_frame_len = 0;
    unlock_state();

    (void)esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, cloud_ws_event_handler, client);
    ret = esp_websocket_client_start(client);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Cloud websocket start failed: %s", esp_err_to_name(ret));
    }
    if (ret == ESP_OK) {
      while (true) {
        bool should_stop = false;

        lock_state();
        should_stop = s_state.cloud_ws_stop_requested || !should_run_cloud_websocket_locked();
        unlock_state();

        if (should_stop || !esp_websocket_client_is_connected(client)) {
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
      }
    }

    if (esp_websocket_client_is_connected(client)) {
      esp_websocket_client_stop(client);
    }
    esp_websocket_client_destroy(client);

    lock_state();
    if (s_state.cloud_ws_client == client) {
      s_state.cloud_ws_client = NULL;
    }
    unlock_state();

    set_cloud_websocket_connected(false, false);
    set_brew_timer_state(false, 0);

    lock_state();
    if (s_state.cloud_ws_stop_requested || !should_run_cloud_websocket_locked()) {
      unlock_state();
      break;
    }
    unlock_state();
    vTaskDelay(pdMS_TO_TICKS(3000));
  }

  lock_state();
  s_state.cloud_ws_task = NULL;
  s_state.cloud_ws_client = NULL;
  s_state.cloud_ws_stop_requested = false;
  unlock_state();
  set_cloud_websocket_connected(false, false);
  set_brew_timer_state(false, 0);
  vTaskDelete(NULL);
}

static void stop_cloud_websocket(bool wait_for_stop) {
  esp_websocket_client_handle_t client = NULL;
  TaskHandle_t task = NULL;

  lock_state();
  s_state.cloud_ws_stop_requested = true;
  client = s_state.cloud_ws_client;
  task = s_state.cloud_ws_task;
  unlock_state();

  if (client != NULL && esp_websocket_client_is_connected(client)) {
    esp_websocket_client_stop(client);
  }

  if (wait_for_stop && task != NULL) {
    const int64_t deadline_us = esp_timer_get_time() + (3LL * 1000LL * 1000LL);
    while (esp_timer_get_time() < deadline_us) {
      lock_state();
      task = s_state.cloud_ws_task;
      unlock_state();
      if (task == NULL) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  set_cloud_websocket_connected(false, false);
  set_brew_timer_state(false, 0);
}

static esp_err_t ensure_cloud_websocket_task(void) {
  esp_err_t ret = ESP_OK;
  bool should_run = false;
  bool can_start = false;
  bool already_running = false;
  bool probe_active = false;
  uint8_t http_requests_in_flight = 0;

  lock_state();
  should_run = should_run_cloud_websocket_locked();
  can_start = can_start_cloud_websocket_locked();
  already_running = s_state.cloud_ws_task != NULL;
  probe_active = s_state.cloud_probe_task != NULL;
  http_requests_in_flight = s_state.cloud_http_requests_in_flight;
  if (!can_start) {
    unlock_state();
    if (!should_run) {
      ESP_LOGI(TAG, "Cloud websocket not needed right now");
      stop_cloud_websocket(false);
    } else if (probe_active || http_requests_in_flight != 0) {
      ESP_LOGI(
        TAG,
        "Cloud websocket deferred (probe_active=%d http_requests=%u)",
        probe_active,
        (unsigned)http_requests_in_flight
      );
    }
    return ESP_OK;
  }
  if (already_running) {
    unlock_state();
    return ESP_OK;
  }
  s_state.cloud_ws_stop_requested = false;
  unlock_state();

  ESP_LOGI(TAG, "Starting cloud websocket task");
  ESP_LOGI(
    TAG,
    "Cloud websocket resources before task create: heap=%u internal=%u largest_internal=%u",
    (unsigned)esp_get_free_heap_size(),
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
  );
  ret = xTaskCreate(cloud_websocket_task, "lm_cloud_ws", LM_CTRL_CLOUD_WS_STACK_SIZE, NULL, 5, &s_state.cloud_ws_task) == pdPASS
    ? ESP_OK
    : ESP_ERR_NO_MEM;
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Could not create cloud websocket task: %s", esp_err_to_name(ret));
    lock_state();
    s_state.cloud_ws_task = NULL;
    unlock_state();
  }
  return ret;
}

static esp_err_t parse_customer_fleet(
  const char *response_body,
  lm_ctrl_cloud_machine_t *machines,
  size_t max_machines,
  size_t *machine_count
) {
  return lm_ctrl_cloud_parse_customer_fleet(response_body, machines, max_machines, machine_count);
}

static esp_err_t refresh_cloud_fleet(char *banner_text, size_t banner_text_size) {
  char username[96];
  char password[128];
  char selected_serial[32];
  char installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t private_key_der_len = 0;
  char access_token[1024];
  char auth_header[1152];
  char timestamp[24];
  char nonce[LM_CTRL_INSTALLATION_ID_LEN];
  char signature_b64[256];
  char *response_body = NULL;
  lm_ctrl_cloud_machine_t machines[LM_CTRL_CLOUD_MAX_FLEET] = {0};
  size_t machine_count = 0;
  int status_code = 0;
  esp_err_t ret;
  bool restored_selection = false;
  lm_ctrl_cloud_machine_t selected_machine = {0};
  lm_ctrl_cloud_http_header_t headers[5];

  lock_state();
  copy_text(username, sizeof(username), s_state.cloud_username);
  copy_text(password, sizeof(password), s_state.cloud_password);
  copy_text(selected_serial, sizeof(selected_serial), s_state.selected_machine.serial);
  unlock_state();

  if (username[0] == '\0' || password[0] == '\0') {
    if (banner_text != NULL && banner_text_size > 0) {
      snprintf(banner_text, banner_text_size, "Save your cloud account first.");
    }
    return ESP_ERR_INVALID_STATE;
  }

  ret = fetch_cloud_access_token_cached(username, password, access_token, sizeof(access_token), banner_text, banner_text_size);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = ensure_cloud_installation();
  if (ret != ESP_OK) {
    if (banner_text != NULL && banner_text_size > 0) {
      snprintf(banner_text, banner_text_size, "Cloud installation data is missing.");
    }
    return ret;
  }

  lock_state();
  copy_text(installation_id, sizeof(installation_id), s_state.cloud_installation_id);
  memcpy(secret, s_state.cloud_secret, sizeof(secret));
  memcpy(private_key_der, s_state.cloud_private_key_der, s_state.cloud_private_key_der_len);
  private_key_der_len = s_state.cloud_private_key_der_len;
  unlock_state();

  ret = build_signed_request_headers(
    installation_id,
    secret,
    private_key_der,
    private_key_der_len,
    timestamp,
    sizeof(timestamp),
    nonce,
    sizeof(nonce),
    signature_b64,
    sizeof(signature_b64)
  );
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Cloud machine request signing failed");
    if (banner_text != NULL && banner_text_size > 0) {
      snprintf(banner_text, banner_text_size, "Machine request signing failed.");
    }
    return ret;
  }

  snprintf(auth_header, sizeof(auth_header), "Bearer %s", access_token);
  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "Authorization", .value = auth_header };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "X-App-Installation-Id", .value = installation_id };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "X-Timestamp", .value = timestamp };
  headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "X-Nonce", .value = nonce };
  headers[4] = (lm_ctrl_cloud_http_header_t){ .name = "X-Request-Signature", .value = signature_b64 };

  ret = http_request_capture(
    LM_CTRL_CLOUD_HOST,
    LM_CTRL_CLOUD_THINGS_PATH,
    LM_CTRL_CLOUD_PORT,
    HTTP_METHOD_GET,
    headers,
    sizeof(headers) / sizeof(headers[0]),
    NULL,
    12000,
    &response_body,
    &status_code
  );
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Machine lookup request transport failed: %s", esp_err_to_name(ret));
    if (banner_text != NULL && banner_text_size > 0) {
      snprintf(banner_text, banner_text_size, "Machine lookup request failed.");
    }
    return ret;
  }

  if (status_code < 200 || status_code >= 300) {
    if (status_code == 401) {
      clear_cached_cloud_access_token();
    }
    ESP_LOGW(TAG, "Machine lookup failed with status %d: %.200s", status_code, response_body != NULL ? response_body : "");
    free(response_body);
    if (banner_text != NULL && banner_text_size > 0) {
      snprintf(banner_text, banner_text_size, "Machine lookup failed with status %d.", status_code);
    }
    return ESP_FAIL;
  }

  ret = parse_customer_fleet(response_body, machines, LM_CTRL_CLOUD_MAX_FLEET, &machine_count);
  free(response_body);
  if (ret != ESP_OK) {
    if (banner_text != NULL && banner_text_size > 0) {
      snprintf(banner_text, banner_text_size, "No machines found in the La Marzocco account.");
    }
    lock_state();
    clear_fleet_locked();
    clear_selected_machine_locked();
    mark_status_dirty_locked();
    unlock_state();
    return ret;
  }

  lock_state();
  clear_fleet_locked();
  memcpy(s_state.fleet, machines, machine_count * sizeof(machines[0]));
  s_state.fleet_count = machine_count;
  if (selected_serial[0] != '\0') {
    for (size_t i = 0; i < machine_count; ++i) {
      if (strcmp(selected_serial, machines[i].serial) == 0) {
        s_state.selected_machine = machines[i];
        s_state.has_machine_selection = true;
        selected_machine = machines[i];
        restored_selection = true;
        break;
      }
    }
  }
  if (!restored_selection && machine_count == 1) {
    s_state.selected_machine = machines[0];
    s_state.has_machine_selection = true;
    selected_machine = machines[0];
    restored_selection = true;
  } else if (!restored_selection && selected_serial[0] != '\0') {
    clear_selected_machine_locked();
  }
  mark_status_dirty_locked();
  unlock_state();

  if (restored_selection) {
    ret = save_machine_selection(&selected_machine);
    if (ret != ESP_OK && banner_text != NULL && banner_text_size > 0) {
      snprintf(banner_text, banner_text_size, "Machines loaded, but saving the selection failed.");
      return ret;
    }
  }

  if (banner_text != NULL && banner_text_size > 0) {
    if (restored_selection && machine_count == 1) {
      snprintf(banner_text, banner_text_size, "Cloud account verified. One machine was found and selected automatically.");
    } else {
      snprintf(banner_text, banner_text_size, "Cloud account verified. Select your machine below.");
    }
  }

  return ESP_OK;
}

static void send_chunk_if_not_empty(httpd_req_t *req, const char *text) {
  if (req == NULL || text == NULL || text[0] == '\0') {
    return;
  }

  httpd_resp_sendstr_chunk(req, text);
}

static esp_err_t send_json_result(httpd_req_t *req, const char *status, bool ok, const char *message) {
  char message_json[256];

  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  json_escape_text(message != NULL ? message : "", message_json, sizeof(message_json));
  httpd_resp_set_status(req, status != NULL ? status : "200 OK");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, ok ? "{\"ok\":true,\"message\":\"" : "{\"ok\":false,\"message\":\"");
  httpd_resp_sendstr_chunk(req, message_json);
  httpd_resp_sendstr_chunk(req, "\"}");
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

static const char *get_setup_page_history_target(const char *uri) {
  if (uri == NULL) {
    return NULL;
  }

  if (
    strcmp(uri, "/controller") == 0 ||
    strcmp(uri, "/controller-logo-clear") == 0
  ) {
    return "/#controller-section";
  }

  if (
    strcmp(uri, "/cloud") == 0 ||
    strcmp(uri, "/cloud-refresh") == 0 ||
    strcmp(uri, "/cloud-machine") == 0 ||
    strcmp(uri, "/bbw") == 0
  ) {
    return "/#cloud-section";
  }

  return NULL;
}

static esp_err_t send_setup_page(httpd_req_t *req, const char *banner) {
  lm_ctrl_setup_page_ctx_t *ctx = calloc(1, sizeof(*ctx));
  ctrl_state_t preset_defaults;
  const char *history_target = get_setup_page_history_target(req != NULL ? req->uri : NULL);
  if (ctx == NULL) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_ERR_NO_MEM;
  }

  ctrl_state_init(&preset_defaults);
  lm_ctrl_wifi_get_info(&ctx->info);
  lock_state();
  ctx->fleet_count = s_state.fleet_count;
  if (ctx->fleet_count > LM_CTRL_CLOUD_MAX_FLEET) {
    ctx->fleet_count = LM_CTRL_CLOUD_MAX_FLEET;
  }
  memcpy(ctx->fleet, s_state.fleet, ctx->fleet_count * sizeof(ctx->fleet[0]));
  unlock_state();
  format_portal_summary(&ctx->info, ctx->status, sizeof(ctx->status));
  html_escape_text(ctx->status, ctx->status_html, sizeof(ctx->status_html));
  html_escape_text(banner != NULL ? banner : "", ctx->banner_html, sizeof(ctx->banner_html));
  html_escape_text(ctx->info.sta_ssid, ctx->ssid_html, sizeof(ctx->ssid_html));
  html_escape_text(ctx->info.hostname, ctx->hostname_html, sizeof(ctx->hostname_html));
  html_escape_text(ctx->info.cloud_username, ctx->cloud_user_html, sizeof(ctx->cloud_user_html));
  lm_ctrl_machine_link_get_info(&ctx->machine_info);
  lm_ctrl_machine_link_get_status(ctx->machine_status, sizeof(ctx->machine_status));
  format_debug_summary(&ctx->info, &ctx->machine_info, ctx->machine_status, ctx->debug_status, sizeof(ctx->debug_status));
  html_escape_text(ctx->debug_status, ctx->debug_status_html, sizeof(ctx->debug_status_html));
  snprintf(
    ctx->local_url,
    sizeof(ctx->local_url),
    "http://%s.local/",
    ctx->info.hostname[0] != '\0' ? ctx->info.hostname : LM_CTRL_WIFI_DEFAULT_HOSTNAME
  );
  html_escape_text(ctx->local_url, ctx->local_url_html, sizeof(ctx->local_url_html));
  ctx->selected_machine_text[0] = '\0';
  ctx->selected_machine_html[0] = '\0';
  ctx->dashboard_loaded_mask = 0;
  ctx->dashboard_feature_mask = 0;
  if (ctx->info.has_machine_selection) {
    snprintf(
      ctx->selected_machine_text,
      sizeof(ctx->selected_machine_text),
      "%s%s%s%s%s",
      ctx->info.machine_name[0] != '\0' ? ctx->info.machine_name : "Selected machine",
      ctx->info.machine_model[0] != '\0' ? " · " : "",
      ctx->info.machine_model,
      ctx->info.machine_serial[0] != '\0' ? " · " : "",
      ctx->info.machine_serial
    );
    html_escape_text(ctx->selected_machine_text, ctx->selected_machine_html, sizeof(ctx->selected_machine_html));
  }
  if (ctx->info.has_cloud_credentials && ctx->info.has_machine_selection) {
    (void)lm_ctrl_machine_link_get_values(
      &ctx->dashboard_values,
      &ctx->dashboard_loaded_mask,
      &ctx->dashboard_feature_mask
    );
    (void)lm_ctrl_machine_link_request_sync();
  }
  for (int preset_index = 0; preset_index < CTRL_PRESET_COUNT; ++preset_index) {
    if (ctrl_state_load_preset_slot(preset_index, &ctx->presets[preset_index]) != ESP_OK) {
      ctx->presets[preset_index] = preset_defaults.presets[preset_index];
    }
  }

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr_chunk(req, "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<link rel=\"icon\" href=\"data:,\">"
    "<title>La Marzocco Controller Setup</title>"
    "<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#120d0a;color:#f4efe7;padding:24px;}"
    "main{max-width:560px;margin:0 auto;}h1{margin:0 0 8px;font-size:28px;}h2{margin:0 0 8px;font-size:20px;color:#ffc685;}p{color:#d5c1ae;}"
    "form{background:#261a13;border:1px solid #926333;border-radius:20px;padding:18px;margin-top:18px;}"
    "label{display:block;margin:14px 0 6px;color:#ffc685;font-weight:600;}"
    "input,select{width:100%;box-sizing:border-box;padding:12px 14px;border-radius:14px;border:1px solid #5d4128;background:#1a120d;color:#f4efe7;}"
    "button{margin-top:18px;padding:12px 18px;border:none;border-radius:14px;background:#ffc685;color:#120d0a;"
    "font-weight:700;cursor:pointer;}pre{white-space:pre-wrap;background:#1a120d;padding:16px;border-radius:16px;"
    "border:1px solid #5d4128;} .banner{color:#ffc685;font-weight:700;margin:12px 0;} .section-note{margin-top:6px;font-size:14px;color:#d5c1ae;}"
    ".secondary{background:#3a281d;color:#f4efe7;} .danger{background:#7f3328;color:#fff7f2;} .machine-pill{margin-top:12px;padding:12px 14px;border-radius:14px;background:#1a120d;border:1px solid #5d4128;}"
    ".button-row{display:flex;gap:12px;flex-wrap:wrap;} .button-row button{flex:1 1 180px;}"
    ".topnav{display:flex;gap:10px;flex-wrap:wrap;margin:18px 0 8px;}.topnav a{padding:10px 14px;border-radius:999px;border:1px solid #5d4128;background:#261a13;color:#f4efe7;text-decoration:none;font-weight:600;}"
    ".topnav a:hover{border-color:#926333;color:#ffc685;} .preset-anchor{scroll-margin-top:24px;}</style></head><body><main>");
  httpd_resp_sendstr_chunk(req, "<h1>Controller Setup</h1><p>Use this page to store the La Marzocco cloud account and the home Wi-Fi settings on the controller. The Wi-Fi scan fills the SSID automatically.</p>");
  if (ctx->banner_html[0] != '\0') {
    httpd_resp_sendstr_chunk(req, "<div class=\"banner\">");
    httpd_resp_sendstr_chunk(req, ctx->banner_html);
    httpd_resp_sendstr_chunk(req, "</div>");
  }
  httpd_resp_sendstr_chunk(req, "<pre>");
  httpd_resp_sendstr_chunk(req, ctx->status_html);
  httpd_resp_sendstr_chunk(req, "</pre>");
  httpd_resp_sendstr_chunk(req, "<p class=\"section-note\">Use <strong>");
  httpd_resp_sendstr_chunk(req, ctx->local_url_html);
  httpd_resp_sendstr_chunk(req, "</strong> in your home network. The router IP can change after reconnects or flashes.</p>");
  httpd_resp_sendstr_chunk(req, "<nav class=\"topnav\"><a href=\"#controller-section\">Controller</a><a href=\"#network-section\">Network</a><a href=\"#cloud-section\">Cloud</a><a href=\"#presets-section\">Presets</a><a href=\"#advanced-section\">Advanced</a></nav>");

  httpd_resp_sendstr_chunk(req, "<form id=\"controller-section\" class=\"preset-anchor\" method=\"post\" action=\"/controller\"><h2>Controller</h2>");
  httpd_resp_sendstr_chunk(req, "<label for=\"hostname\">Controller Hostname</label><input id=\"hostname\" name=\"hostname\" maxlength=\"32\" value=\"");
  send_chunk_if_not_empty(req, ctx->hostname_html);
  httpd_resp_sendstr_chunk(req, "\">");
  httpd_resp_sendstr_chunk(req, "<label for=\"language\">Controller Language</label><select id=\"language\" name=\"language\">");
  httpd_resp_sendstr_chunk(req, "<option value=\"en\"");
  if (ctx->info.language == CTRL_LANGUAGE_EN) {
    httpd_resp_sendstr_chunk(req, " selected");
  }
  httpd_resp_sendstr_chunk(req, ">English</option>");
  httpd_resp_sendstr_chunk(req, "<option value=\"de\"");
  if (ctx->info.language == CTRL_LANGUAGE_DE) {
    httpd_resp_sendstr_chunk(req, " selected");
  }
  httpd_resp_sendstr_chunk(req, ">Deutsch</option></select>");
  httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Language only affects the on-device controller UI. English stays the default.</div><button type=\"submit\">Save Controller Settings</button></form>");

  httpd_resp_sendstr_chunk(req, "<form class=\"preset-anchor\"><h2>Header Logo</h2>");
  httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Current header: <strong id=\"header-logo-state\">");
  httpd_resp_sendstr_chunk(req, ctx->info.has_custom_logo ? "Custom logo installed" : "Default text");
  httpd_resp_sendstr_chunk(req, "</strong></div>");
  httpd_resp_sendstr_chunk(req, "<label for=\"logo-file\">Local SVG File</label><input id=\"logo-file\" type=\"file\" accept=\".svg,image/svg+xml\">");
  httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Optional controller setting. The project does not ship official La Marzocco logos. Uploaded SVGs are rasterized in your browser, stored locally on this controller, and shown only on the device display.</div>");
  httpd_resp_sendstr_chunk(req, "<div class=\"button-row\"><button type=\"button\" id=\"logo-upload-button\">Upload Logo</button>");
  httpd_resp_sendstr_chunk(req, "<button class=\"secondary\" type=\"submit\" formaction=\"/controller-logo-clear\" formmethod=\"post\" onclick=\"return confirm('Remove the custom controller logo and fall back to the text header?');\">Remove Logo</button></div>");
  httpd_resp_sendstr_chunk(req, "<div id=\"logo-upload-status\" class=\"section-note\"></div></form>");

  httpd_resp_sendstr_chunk(req, "<form id=\"network-section\" class=\"preset-anchor\" method=\"post\" action=\"/wifi\"><h2>Network</h2>");
  httpd_resp_sendstr_chunk(req, "<button type=\"button\" id=\"scan-button\">Scan Nearby Networks</button>");
  httpd_resp_sendstr_chunk(req, "<label for=\"network-list\">Nearby Networks</label><select id=\"network-list\"><option value=\"\">Select from scan results</option></select>");
  httpd_resp_sendstr_chunk(req, "<label for=\"ssid\">Home Wi-Fi SSID</label><input id=\"ssid\" name=\"ssid\" maxlength=\"32\" required value=\"");
  send_chunk_if_not_empty(req, ctx->ssid_html);
  httpd_resp_sendstr_chunk(req, "\">");
  httpd_resp_sendstr_chunk(req, "<label for=\"password\">Wi-Fi Password</label><input id=\"password\" name=\"password\" type=\"password\" maxlength=\"64\">");
  httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Saving Wi-Fi starts a reconnect attempt immediately. Leave the password blank to keep the stored password for the same SSID.</div><button type=\"submit\">Save Wi-Fi And Connect</button></form>");

  httpd_resp_sendstr_chunk(req, "<form id=\"cloud-section\" class=\"preset-anchor\" method=\"post\" action=\"/cloud\"><h2>La Marzocco Cloud</h2>");
  httpd_resp_sendstr_chunk(req, "<label for=\"cloud_username\">Account E-Mail</label><input id=\"cloud_username\" name=\"cloud_username\" type=\"email\" maxlength=\"95\" value=\"");
  send_chunk_if_not_empty(req, ctx->cloud_user_html);
  httpd_resp_sendstr_chunk(req, "\">");
  httpd_resp_sendstr_chunk(req, "<label for=\"cloud_password\">Account Password</label><input id=\"cloud_password\" name=\"cloud_password\" type=\"password\" maxlength=\"127\">");
  httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Saving verifies the account and loads the machine list from La Marzocco Cloud. It does not restart networking.</div><button type=\"submit\">Save Cloud Account And Load Machines</button></form>");

  if (ctx->info.has_cloud_credentials) {
    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/cloud-refresh\"><h2>Cloud Machines</h2>");
    if (ctx->info.has_machine_selection) {
      httpd_resp_sendstr_chunk(req, "<div class=\"machine-pill\">Selected: ");
      send_chunk_if_not_empty(req, ctx->selected_machine_html);
      httpd_resp_sendstr_chunk(req, "</div>");
    } else {
      httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">No machine selected yet.</div>");
    }
    httpd_resp_sendstr_chunk(req, "<button class=\"secondary\" type=\"submit\">Reload Machine List</button></form>");
  }

  if (ctx->fleet_count > 0) {
    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/cloud-machine\"><h2>Select Machine</h2>");
    httpd_resp_sendstr_chunk(req, "<label for=\"machine_serial\">Machine</label><select id=\"machine_serial\" name=\"machine_serial\" required>");
    for (size_t i = 0; i < ctx->fleet_count; ++i) {
      char machine_serial_html[96];
      char machine_name_html[160];
      char machine_model_html[96];

      html_escape_text(ctx->fleet[i].serial, machine_serial_html, sizeof(machine_serial_html));
      html_escape_text(ctx->fleet[i].name, machine_name_html, sizeof(machine_name_html));
      html_escape_text(ctx->fleet[i].model, machine_model_html, sizeof(machine_model_html));

      httpd_resp_sendstr_chunk(req, "<option value=\"");
      send_chunk_if_not_empty(req, machine_serial_html);
      httpd_resp_sendstr_chunk(req, "\"");
      if (ctx->info.has_machine_selection && strcmp(ctx->info.machine_serial, ctx->fleet[i].serial) == 0) {
        httpd_resp_sendstr_chunk(req, " selected");
      }
      httpd_resp_sendstr_chunk(req, ">");
      if (machine_name_html[0] != '\0') {
        httpd_resp_sendstr_chunk(req, machine_name_html);
      } else {
        httpd_resp_sendstr_chunk(req, "Unnamed machine");
      }
      if (machine_model_html[0] != '\0') {
        httpd_resp_sendstr_chunk(req, " · ");
        httpd_resp_sendstr_chunk(req, machine_model_html);
      }
      if (machine_serial_html[0] != '\0') {
        httpd_resp_sendstr_chunk(req, " · ");
        httpd_resp_sendstr_chunk(req, machine_serial_html);
      }
      httpd_resp_sendstr_chunk(req, "</option>");
    }
    httpd_resp_sendstr_chunk(req, "</select><div class=\"section-note\">The selected machine and communication key are stored locally on the controller.</div><button type=\"submit\">Use Selected Machine</button></form>");
  }

  if ((ctx->dashboard_feature_mask & LM_CTRL_MACHINE_FEATURE_BBW) != 0) {
    char bbw_dose_1[24];
    char bbw_dose_2[24];

    snprintf(
      bbw_dose_1,
      sizeof(bbw_dose_1),
      "%.1f",
      (double)((ctx->dashboard_loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 ? ctx->dashboard_values.bbw_dose_1_g : 32.0f)
    );
    snprintf(
      bbw_dose_2,
      sizeof(bbw_dose_2),
      "%.1f",
      (double)((ctx->dashboard_loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 ? ctx->dashboard_values.bbw_dose_2_g : 34.0f)
    );

    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/bbw\"><h2>Brew By Weight</h2>");
    httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Shown because the cloud dashboard reports brew by weight support for the selected machine.</div>");
    httpd_resp_sendstr_chunk(req, "<label for=\"bbw_mode\">Mode</label><select id=\"bbw_mode\" name=\"bbw_mode\">");
    httpd_resp_sendstr_chunk(req, "<option value=\"Dose1\"");
    if (ctx->dashboard_values.bbw_mode == CTRL_BBW_MODE_DOSE_1) {
      httpd_resp_sendstr_chunk(req, " selected");
    }
    httpd_resp_sendstr_chunk(req, ">Dose 1</option>");
    httpd_resp_sendstr_chunk(req, "<option value=\"Dose2\"");
    if (ctx->dashboard_values.bbw_mode == CTRL_BBW_MODE_DOSE_2) {
      httpd_resp_sendstr_chunk(req, " selected");
    }
    httpd_resp_sendstr_chunk(req, ">Dose 2</option>");
    httpd_resp_sendstr_chunk(req, "<option value=\"Continuous\"");
    if (ctx->dashboard_values.bbw_mode == CTRL_BBW_MODE_CONTINUOUS) {
      httpd_resp_sendstr_chunk(req, " selected");
    }
    httpd_resp_sendstr_chunk(req, ">Continuous</option></select>");
    httpd_resp_sendstr_chunk(req, "<label for=\"bbw_dose_1\">Dose 1 Target (g)</label><input id=\"bbw_dose_1\" name=\"bbw_dose_1\" type=\"number\" min=\"5\" max=\"100\" step=\"0.1\" value=\"");
    httpd_resp_sendstr_chunk(req, bbw_dose_1);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<label for=\"bbw_dose_2\">Dose 2 Target (g)</label><input id=\"bbw_dose_2\" name=\"bbw_dose_2\" type=\"number\" min=\"5\" max=\"100\" step=\"0.1\" value=\"");
    httpd_resp_sendstr_chunk(req, bbw_dose_2);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">These values are cloud-only and will also be available on the round controller when BBW is supported.</div><button type=\"submit\">Save Brew By Weight Settings</button></form>");
  }

  httpd_resp_sendstr_chunk(req, "<div id=\"presets-section\" class=\"preset-anchor\"></div><p class=\"section-note\">Preset edits here only change the stored preset definitions. They do not load onto the controller and do not send anything to the machine.</p>");
  for (int preset_index = 0; preset_index < CTRL_PRESET_COUNT; ++preset_index) {
    char preset_title[24];
    char preset_slot_text[4];
    char preset_default_name[CTRL_PRESET_NAME_LEN];
    char preset_display_name[CTRL_PRESET_NAME_LEN];
    char preset_display_name_html[96];
    char preset_name_html[96];
    char preset_name_placeholder_html[96];
    char temperature_text[24];
    char infuse_text[24];
    char pause_text[24];
    char bbw_dose_1_text[24];
    char bbw_dose_2_text[24];
    const ctrl_preset_t *preset = &ctx->presets[preset_index];

    ctrl_preset_default_name(preset_index, preset_default_name, sizeof(preset_default_name));
    ctrl_preset_display_name(preset, preset_index, preset_display_name, sizeof(preset_display_name));
    html_escape_text(preset_display_name, preset_display_name_html, sizeof(preset_display_name_html));
    html_escape_text(preset->name, preset_name_html, sizeof(preset_name_html));
    html_escape_text(preset_default_name, preset_name_placeholder_html, sizeof(preset_name_placeholder_html));
    snprintf(preset_title, sizeof(preset_title), "Preset %d", preset_index + 1);
    snprintf(preset_slot_text, sizeof(preset_slot_text), "%d", preset_index);
    snprintf(temperature_text, sizeof(temperature_text), "%.1f", (double)preset->values.temperature_c);
    snprintf(infuse_text, sizeof(infuse_text), "%.1f", (double)preset->values.infuse_s);
    snprintf(pause_text, sizeof(pause_text), "%.1f", (double)preset->values.pause_s);
    snprintf(bbw_dose_1_text, sizeof(bbw_dose_1_text), "%.1f", (double)preset->values.bbw_dose_1_g);
    snprintf(bbw_dose_2_text, sizeof(bbw_dose_2_text), "%.1f", (double)preset->values.bbw_dose_2_g);

    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/preset\"><h2>");
    httpd_resp_sendstr_chunk(req, preset_title);
    httpd_resp_sendstr_chunk(req, "</h2><div class=\"section-note\">Current display name: ");
    httpd_resp_sendstr_chunk(req, preset_display_name_html);
    httpd_resp_sendstr_chunk(req, "</div><input type=\"hidden\" name=\"preset_slot\" value=\"");
    httpd_resp_sendstr_chunk(req, preset_slot_text);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<label for=\"preset_name_");
    httpd_resp_sendstr_chunk(req, preset_slot_text);
    httpd_resp_sendstr_chunk(req, "\">Name</label><input id=\"preset_name_");
    httpd_resp_sendstr_chunk(req, preset_slot_text);
    httpd_resp_sendstr_chunk(req, "\" name=\"preset_name\" maxlength=\"32\" placeholder=\"");
    send_chunk_if_not_empty(req, preset_name_placeholder_html);
    httpd_resp_sendstr_chunk(req, "\" value=\"");
    send_chunk_if_not_empty(req, preset_name_html);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<label>Temperature (C)</label><input name=\"temperature_c\" type=\"number\" min=\"80\" max=\"103\" step=\"0.1\" value=\"");
    httpd_resp_sendstr_chunk(req, temperature_text);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<label>Prebrewing In (s)</label><input name=\"infuse_s\" type=\"number\" min=\"0\" max=\"9\" step=\"0.1\" value=\"");
    httpd_resp_sendstr_chunk(req, infuse_text);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<label>Prebrewing Out (s)</label><input name=\"pause_s\" type=\"number\" min=\"0\" max=\"9\" step=\"0.1\" value=\"");
    httpd_resp_sendstr_chunk(req, pause_text);
    httpd_resp_sendstr_chunk(req, "\">");
    if ((ctx->dashboard_feature_mask & LM_CTRL_MACHINE_FEATURE_BBW) != 0) {
      httpd_resp_sendstr_chunk(req, "<label>BBW Mode</label><select name=\"bbw_mode\">");
      httpd_resp_sendstr_chunk(req, "<option value=\"Dose1\"");
      if (preset->values.bbw_mode == CTRL_BBW_MODE_DOSE_1) {
        httpd_resp_sendstr_chunk(req, " selected");
      }
      httpd_resp_sendstr_chunk(req, ">Dose 1</option><option value=\"Dose2\"");
      if (preset->values.bbw_mode == CTRL_BBW_MODE_DOSE_2) {
        httpd_resp_sendstr_chunk(req, " selected");
      }
      httpd_resp_sendstr_chunk(req, ">Dose 2</option><option value=\"Continuous\"");
      if (preset->values.bbw_mode == CTRL_BBW_MODE_CONTINUOUS) {
        httpd_resp_sendstr_chunk(req, " selected");
      }
      httpd_resp_sendstr_chunk(req, ">Continuous</option></select>");
      httpd_resp_sendstr_chunk(req, "<label>BBW Dose 1 (g)</label><input name=\"bbw_dose_1\" type=\"number\" min=\"5\" max=\"100\" step=\"0.1\" value=\"");
      httpd_resp_sendstr_chunk(req, bbw_dose_1_text);
      httpd_resp_sendstr_chunk(req, "\">");
      httpd_resp_sendstr_chunk(req, "<label>BBW Dose 2 (g)</label><input name=\"bbw_dose_2\" type=\"number\" min=\"5\" max=\"100\" step=\"0.1\" value=\"");
      httpd_resp_sendstr_chunk(req, bbw_dose_2_text);
      httpd_resp_sendstr_chunk(req, "\">");
    }
    httpd_resp_sendstr_chunk(req, "<button type=\"submit\">Save Preset</button></form>");
  }

  httpd_resp_sendstr_chunk(req, "<form id=\"advanced-section\" class=\"preset-anchor\"><h2>Advanced</h2><div class=\"section-note\">Use these actions when you want to restart the controller or wipe onboarding data.</div><div class=\"button-row\">");
  httpd_resp_sendstr_chunk(req, "<button class=\"secondary\" type=\"submit\" formaction=\"/reboot\" formmethod=\"post\">Reboot Controller</button>");
  httpd_resp_sendstr_chunk(req, "<button class=\"secondary\" type=\"submit\" formaction=\"/reset-network\" formmethod=\"post\" onclick=\"return confirm('Clear Wi-Fi, cloud account, and machine selection?');\">Reset Network Setup</button>");
  httpd_resp_sendstr_chunk(req, "<button class=\"danger\" type=\"submit\" formaction=\"/factory-reset\" formmethod=\"post\" onclick=\"return confirm('Factory reset the controller and erase presets?');\">Factory Reset</button>");
  httpd_resp_sendstr_chunk(req, "</div></form>");
  httpd_resp_sendstr_chunk(req, "<form><h2>Diagnostics</h2><div class=\"section-note\">Read-only controller and link state for debugging reconnect or sync issues.</div><pre>");
  httpd_resp_sendstr_chunk(req, ctx->debug_status_html);
  httpd_resp_sendstr_chunk(req, "</pre></form>");

  httpd_resp_sendstr_chunk(req,
    "<script>"
    "const scanButton=document.getElementById('scan-button');"
    "const networkList=document.getElementById('network-list');"
    "const ssidInput=document.getElementById('ssid');"
    "const logoFileInput=document.getElementById('logo-file');"
    "const logoUploadButton=document.getElementById('logo-upload-button');"
    "const logoUploadStatus=document.getElementById('logo-upload-status');"
    "const logoState=document.getElementById('header-logo-state');"
    "const LOGO_WIDTH=150;"
    "const LOGO_HEIGHT=26;"
    "const LOGO_VERSION=1;"
    "const setLogoStatus=(text,isError)=>{if(!logoUploadStatus){return;}logoUploadStatus.textContent=text||'';logoUploadStatus.style.color=isError?'#ffb4a8':'#d5c1ae';};"
    "const readSvgText=(file)=>new Promise((resolve,reject)=>{const reader=new FileReader();reader.onload=()=>resolve(typeof reader.result==='string'?reader.result:'');reader.onerror=()=>reject(new Error('Could not read the SVG file.'));reader.readAsText(file);});"
    "const loadSvgImage=(svgText)=>new Promise((resolve,reject)=>{const url=URL.createObjectURL(new Blob([svgText],{type:'image/svg+xml'}));const image=new Image();image.onload=()=>{URL.revokeObjectURL(url);resolve(image);};image.onerror=()=>{URL.revokeObjectURL(url);reject(new Error('Could not decode the SVG file.'));};image.src=url;});"
    "const bytesToBase64=(bytes)=>{let binary='';const chunkSize=0x8000;for(let i=0;i<bytes.length;i+=chunkSize){binary+=String.fromCharCode(...bytes.subarray(i,i+chunkSize));}return btoa(binary);};"
    "const rasterizeSvgToLvgl=(image)=>{const canvas=document.createElement('canvas');canvas.width=LOGO_WIDTH;canvas.height=LOGO_HEIGHT;const ctx=canvas.getContext('2d');if(!ctx){throw new Error('Canvas rendering is not available.');}ctx.clearRect(0,0,LOGO_WIDTH,LOGO_HEIGHT);const sourceWidth=image.naturalWidth||image.width;const sourceHeight=image.naturalHeight||image.height;if(!sourceWidth||!sourceHeight){throw new Error('The SVG has no usable size.');}const scale=Math.min(LOGO_WIDTH/sourceWidth,LOGO_HEIGHT/sourceHeight);const drawWidth=sourceWidth*scale;const drawHeight=sourceHeight*scale;ctx.drawImage(image,(LOGO_WIDTH-drawWidth)/2,(LOGO_HEIGHT-drawHeight)/2,drawWidth,drawHeight);const rgba=ctx.getImageData(0,0,LOGO_WIDTH,LOGO_HEIGHT).data;const output=new Uint8Array(LOGO_WIDTH*LOGO_HEIGHT*3);for(let i=0,j=0;i<rgba.length;i+=4,j+=3){const r=rgba[i];const g=rgba[i+1];const b=rgba[i+2];const a=rgba[i+3];output[j]=((r&0xF8)|((g>>5)&0x07));output[j+1]=(((g&0x1C)<<3)|((b>>3)&0x1F));output[j+2]=a;}return output;};"
    "scanButton.addEventListener('click',async()=>{"
      "scanButton.disabled=true;scanButton.textContent='Scanning...';"
      "try{const response=await fetch('/wifi-scan');const data=await response.json();"
        "networkList.innerHTML='<option value=\"\">Select from scan results</option>';"
        "(data.ssids||[]).forEach((ssid)=>{const option=document.createElement('option');option.value=ssid;option.textContent=ssid;networkList.appendChild(option);});"
        "scanButton.textContent=(data.ssids||[]).length?'Scan Again':'No Networks Found';"
      "}catch(error){scanButton.textContent='Scan Failed';}"
      "finally{scanButton.disabled=false;}"
    "});"
    "networkList.addEventListener('change',()=>{if(networkList.value){ssidInput.value=networkList.value;}});"
    "logoUploadButton.addEventListener('click',async()=>{const file=logoFileInput&&logoFileInput.files?logoFileInput.files[0]:null;if(!file){setLogoStatus('Choose an SVG file first.',true);return;}if(!/\\.svg$/i.test(file.name)&&file.type!=='image/svg+xml'){setLogoStatus('Only SVG uploads are supported.',true);return;}logoUploadButton.disabled=true;logoUploadButton.textContent='Uploading...';setLogoStatus('',false);try{const svgText=await readSvgText(file);const image=await loadSvgImage(svgText);const logoBytes=rasterizeSvgToLvgl(image);const response=await fetch('/controller-logo',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({version:LOGO_VERSION,width:LOGO_WIDTH,height:LOGO_HEIGHT,data:bytesToBase64(logoBytes)})});const payload=await response.json().catch(()=>({ok:false,message:'Unexpected response from controller.'}));if(!response.ok||!payload.ok){throw new Error(payload.message||'Upload failed.');}if(logoState){logoState.textContent='Custom logo installed';}setLogoStatus(payload.message||'Custom logo saved.',false);logoFileInput.value='';}catch(error){setLogoStatus(error&&error.message?error.message:'Upload failed.',true);}finally{logoUploadButton.disabled=false;logoUploadButton.textContent='Upload Logo';}});"
    "</script>");
  if (history_target != NULL) {
    httpd_resp_sendstr_chunk(req, "<script>history.replaceState(null,'','");
    httpd_resp_sendstr_chunk(req, history_target);
    httpd_resp_sendstr_chunk(req, "');</script>");
  }
  httpd_resp_sendstr_chunk(req, "</main></body></html>");
  httpd_resp_sendstr_chunk(req, NULL);
  free(ctx);
  return ESP_OK;
}

static esp_err_t handle_root_get(httpd_req_t *req) {
  return send_setup_page(req, NULL);
}

static esp_err_t handle_controller_post(httpd_req_t *req) {
  char body[512];
  char hostname[33];
  char language_code[8];
  ctrl_language_t language = CTRL_LANGUAGE_EN;
  int received = 0;

  if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
    return ESP_FAIL;
  }

  while (received < req->content_len) {
    int chunk = httpd_req_recv(req, body + received, req->content_len - received);
    if (chunk <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form data");
      return ESP_FAIL;
    }
    received += chunk;
  }
  body[received] = '\0';

  parse_form_value(body, "hostname", hostname, sizeof(hostname));
  parse_form_value(body, "language", language_code, sizeof(language_code));
  if (hostname[0] == '\0') {
    copy_text(hostname, sizeof(hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  }
  language = ctrl_language_from_code(language_code);

  if (lm_ctrl_wifi_save_controller_preferences(hostname, language) != ESP_OK) {
    return send_setup_page(req, "Could not store controller settings.");
  }

  return send_setup_page(req, "Controller settings saved.");
}

static esp_err_t handle_controller_logo_post(httpd_req_t *req) {
  char *body = NULL;
  uint8_t *logo_blob = NULL;
  cJSON *root = NULL;
  cJSON *version_item = NULL;
  cJSON *width_item = NULL;
  cJSON *height_item = NULL;
  cJSON *data_item = NULL;
  size_t decoded_len = 0;
  int received = 0;
  esp_err_t ret = ESP_FAIL;

  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (req->content_len <= 0 || req->content_len >= 24576) {
    return send_json_result(req, "400 Bad Request", false, "Invalid upload payload.");
  }

  body = calloc((size_t)req->content_len + 1U, 1U);
  logo_blob = malloc(LM_CTRL_CUSTOM_LOGO_BLOB_SIZE);
  if (body == NULL || logo_blob == NULL) {
    free(body);
    free(logo_blob);
    return send_json_result(req, "500 Internal Server Error", false, "Out of memory while reading the logo upload.");
  }

  while (received < req->content_len) {
    int chunk = httpd_req_recv(req, body + received, req->content_len - received);
    if (chunk <= 0) {
      free(body);
      free(logo_blob);
      return send_json_result(req, "500 Internal Server Error", false, "Failed to read the upload body.");
    }
    received += chunk;
  }
  body[received] = '\0';

  root = cJSON_Parse(body);
  if (root == NULL) {
    free(body);
    free(logo_blob);
    return send_json_result(req, "400 Bad Request", false, "Upload body must be valid JSON.");
  }

  version_item = cJSON_GetObjectItemCaseSensitive(root, "version");
  width_item = cJSON_GetObjectItemCaseSensitive(root, "width");
  height_item = cJSON_GetObjectItemCaseSensitive(root, "height");
  data_item = cJSON_GetObjectItemCaseSensitive(root, "data");
  if (!cJSON_IsNumber(version_item) ||
      !cJSON_IsNumber(width_item) ||
      !cJSON_IsNumber(height_item) ||
      !cJSON_IsString(data_item) ||
      data_item->valuestring == NULL) {
    cJSON_Delete(root);
    free(body);
    free(logo_blob);
    return send_json_result(req, "400 Bad Request", false, "Upload JSON is missing required logo fields.");
  }

  if (version_item->valueint != LM_CTRL_CUSTOM_LOGO_SCHEMA_VERSION ||
      width_item->valueint != LM_CTRL_CUSTOM_LOGO_WIDTH ||
      height_item->valueint != LM_CTRL_CUSTOM_LOGO_HEIGHT) {
    cJSON_Delete(root);
    free(body);
    free(logo_blob);
    return send_json_result(req, "400 Bad Request", false, "Unexpected logo format. Rasterize the SVG to the controller header size first.");
  }

  ret = base64_decode_bytes(data_item->valuestring, logo_blob, LM_CTRL_CUSTOM_LOGO_BLOB_SIZE, &decoded_len);
  if (ret != ESP_OK || decoded_len != LM_CTRL_CUSTOM_LOGO_BLOB_SIZE) {
    cJSON_Delete(root);
    free(body);
    free(logo_blob);
    return send_json_result(req, "400 Bad Request", false, "Logo data could not be decoded or has the wrong size.");
  }

  ret = lm_ctrl_wifi_save_controller_logo((uint8_t)version_item->valueint, logo_blob, decoded_len);
  cJSON_Delete(root);
  free(body);
  free(logo_blob);
  if (ret != ESP_OK) {
    return send_json_result(req, "500 Internal Server Error", false, "Could not store the custom controller logo.");
  }

  return send_json_result(req, "200 OK", true, "Custom controller logo saved. The on-device header has been updated.");
}

static esp_err_t handle_controller_logo_clear_post(httpd_req_t *req) {
  if (lm_ctrl_wifi_clear_controller_logo() != ESP_OK) {
    return send_setup_page(req, "Could not remove the custom controller logo.");
  }

  return send_setup_page(req, "Custom controller logo removed. The header now uses the default text again.");
}

static esp_err_t handle_wifi_post(httpd_req_t *req) {
  char body[768];
  char ssid[33];
  char password[65];
  char hostname[33];
  char current_ssid[33];
  char current_password[65];
  char current_hostname[33];
  ctrl_language_t language;
  int received = 0;

  if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
    return ESP_FAIL;
  }

  while (received < req->content_len) {
    int chunk = httpd_req_recv(req, body + received, req->content_len - received);
    if (chunk <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form data");
      return ESP_FAIL;
    }
    received += chunk;
  }
  body[received] = '\0';

  parse_form_value(body, "ssid", ssid, sizeof(ssid));
  parse_form_value(body, "password", password, sizeof(password));

  if (ssid[0] == '\0') {
    return send_setup_page(req, "SSID is required.");
  }

  lock_state();
  copy_text(current_ssid, sizeof(current_ssid), s_state.sta_ssid);
  copy_text(current_password, sizeof(current_password), s_state.sta_password);
  copy_text(current_hostname, sizeof(current_hostname), s_state.hostname);
  language = s_state.language;
  unlock_state();
  copy_text(hostname, sizeof(hostname), current_hostname[0] != '\0' ? current_hostname : LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  if (password[0] == '\0' && current_ssid[0] != '\0' && strcmp(ssid, current_ssid) == 0) {
    copy_text(password, sizeof(password), current_password);
  }

  if (save_credentials(ssid, password, hostname, language) != ESP_OK) {
    return send_setup_page(req, "Could not store Wi-Fi credentials.");
  }

  if (apply_station_credentials() != ESP_OK) {
    return send_setup_page(req, "Credentials saved, but station connect did not start.");
  }

  ESP_LOGI(TAG, "Stored Wi-Fi credentials for SSID '%s'", ssid);
  return send_setup_page(req, "Saved. The controller is now trying to join the configured Wi-Fi.");
}

static esp_err_t handle_reboot_post(httpd_req_t *req) {
  if (lm_ctrl_wifi_schedule_reboot() != ESP_OK) {
    return send_setup_page(req, "Could not schedule a reboot.");
  }

  return send_setup_page(req, "Controller reboot scheduled.");
}

static esp_err_t handle_reset_network_post(httpd_req_t *req) {
  if (lm_ctrl_wifi_reset_network() != ESP_OK) {
    return send_setup_page(req, "Could not reset network settings.");
  }

  return send_setup_page(req, "Network settings cleared. The controller will reboot into setup mode.");
}

static esp_err_t handle_factory_reset_post(httpd_req_t *req) {
  if (lm_ctrl_wifi_factory_reset() != ESP_OK) {
    return send_setup_page(req, "Could not reset the controller.");
  }

  return send_setup_page(req, "Factory reset scheduled. The controller will reboot into setup mode.");
}

static esp_err_t handle_cloud_post(httpd_req_t *req) {
  char body[768];
  char cloud_username[96];
  char cloud_password[128];
  char banner[160];
  int received = 0;

  if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
    return ESP_FAIL;
  }

  while (received < req->content_len) {
    int chunk = httpd_req_recv(req, body + received, req->content_len - received);
    if (chunk <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form data");
      return ESP_FAIL;
    }
    received += chunk;
  }
  body[received] = '\0';

  parse_form_value(body, "cloud_username", cloud_username, sizeof(cloud_username));
  parse_form_value(body, "cloud_password", cloud_password, sizeof(cloud_password));

  if (cloud_username[0] == '\0') {
    return send_setup_page(req, "Cloud e-mail is required.");
  }
  if (cloud_password[0] == '\0') {
    return send_setup_page(req, "Cloud password is required.");
  }

  if (save_cloud_credentials(cloud_username, cloud_password) != ESP_OK) {
    return send_setup_page(req, "Could not store cloud credentials.");
  }

  ESP_LOGI(TAG, "Stored cloud credentials for '%s'", cloud_username);
  if (refresh_cloud_fleet(banner, sizeof(banner)) != ESP_OK) {
    return send_setup_page(req, banner[0] != '\0' ? banner : "Cloud account stored, but machine lookup failed.");
  }
  return send_setup_page(req, banner);
}

static esp_err_t handle_cloud_refresh_post(httpd_req_t *req) {
  char banner[160];

  if (refresh_cloud_fleet(banner, sizeof(banner)) != ESP_OK) {
    return send_setup_page(req, banner[0] != '\0' ? banner : "Machine lookup failed.");
  }

  return send_setup_page(req, banner);
}

static esp_err_t handle_cloud_machine_post(httpd_req_t *req) {
  char body[512];
  char machine_serial[32];
  int received = 0;
  lm_ctrl_cloud_machine_t selected_machine = {0};
  bool found = false;

  if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
    return ESP_FAIL;
  }

  while (received < req->content_len) {
    int chunk = httpd_req_recv(req, body + received, req->content_len - received);
    if (chunk <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form data");
      return ESP_FAIL;
    }
    received += chunk;
  }
  body[received] = '\0';

  parse_form_value(body, "machine_serial", machine_serial, sizeof(machine_serial));
  if (machine_serial[0] == '\0') {
    return send_setup_page(req, "Select a machine first.");
  }

  lock_state();
  for (size_t i = 0; i < s_state.fleet_count; ++i) {
    if (strcmp(machine_serial, s_state.fleet[i].serial) == 0) {
      selected_machine = s_state.fleet[i];
      found = true;
      break;
    }
  }
  unlock_state();

  if (!found) {
    char banner[160];

    if (refresh_cloud_fleet(banner, sizeof(banner)) != ESP_OK) {
      return send_setup_page(req, banner[0] != '\0' ? banner : "Machine list is stale. Reload it first.");
    }

    lock_state();
    for (size_t i = 0; i < s_state.fleet_count; ++i) {
      if (strcmp(machine_serial, s_state.fleet[i].serial) == 0) {
        selected_machine = s_state.fleet[i];
        found = true;
        break;
      }
    }
    unlock_state();
  }

  if (!found) {
    return send_setup_page(req, "Selected machine was not found in the current cloud account.");
  }

  if (save_machine_selection(&selected_machine) != ESP_OK) {
    return send_setup_page(req, "Could not store the selected machine.");
  }

  (void)lm_ctrl_machine_link_request_sync();
  ESP_LOGI(TAG, "Stored machine '%s' (%s)", selected_machine.name, selected_machine.serial);
  return send_setup_page(req, "Machine selection saved on the controller.");
}

static esp_err_t handle_bbw_post(httpd_req_t *req) {
  char body[512];
  char bbw_mode_code[24];
  char bbw_dose_1_text[24];
  char bbw_dose_2_text[24];
  char payload[128];
  char status_text[192];
  char *endptr = NULL;
  float dose_1 = 0.0f;
  float dose_2 = 0.0f;
  int received = 0;

  if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
    return ESP_FAIL;
  }

  while (received < req->content_len) {
    int chunk = httpd_req_recv(req, body + received, req->content_len - received);
    if (chunk <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form data");
      return ESP_FAIL;
    }
    received += chunk;
  }
  body[received] = '\0';

  parse_form_value(body, "bbw_mode", bbw_mode_code, sizeof(bbw_mode_code));
  parse_form_value(body, "bbw_dose_1", bbw_dose_1_text, sizeof(bbw_dose_1_text));
  parse_form_value(body, "bbw_dose_2", bbw_dose_2_text, sizeof(bbw_dose_2_text));

  if (bbw_mode_code[0] == '\0' || bbw_dose_1_text[0] == '\0' || bbw_dose_2_text[0] == '\0') {
    return send_setup_page(req, "Brew by weight mode and both dose targets are required.");
  }

  dose_1 = strtof(bbw_dose_1_text, &endptr);
  if (endptr == bbw_dose_1_text || *endptr != '\0') {
    return send_setup_page(req, "Dose 1 must be a valid number.");
  }
  dose_2 = strtof(bbw_dose_2_text, &endptr);
  if (endptr == bbw_dose_2_text || *endptr != '\0') {
    return send_setup_page(req, "Dose 2 must be a valid number.");
  }

  if (dose_1 < 5.0f || dose_1 > 100.0f || dose_2 < 5.0f || dose_2 > 100.0f) {
    return send_setup_page(req, "Brew by weight doses must stay between 5.0 g and 100.0 g.");
  }

  snprintf(payload, sizeof(payload), "{\"mode\":\"%s\"}", bbw_mode_code);
  status_text[0] = '\0';
  if (lm_ctrl_wifi_execute_machine_command("CoffeeMachineBrewByWeightChangeMode", payload, NULL, status_text, sizeof(status_text)) != ESP_OK) {
    return send_setup_page(req, status_text[0] != '\0' ? status_text : "Could not update the brew by weight mode.");
  }

  snprintf(
    payload,
    sizeof(payload),
    "{\"doses\":{\"Dose1\":%.1f,\"Dose2\":%.1f}}",
    (double)dose_1,
    (double)dose_2
  );
  status_text[0] = '\0';
  if (lm_ctrl_wifi_execute_machine_command("CoffeeMachineBrewByWeightSettingDoses", payload, NULL, status_text, sizeof(status_text)) != ESP_OK) {
    return send_setup_page(req, status_text[0] != '\0' ? status_text : "Could not update the brew by weight doses.");
  }

  (void)lm_ctrl_machine_link_request_sync();
  return send_setup_page(req, "Brew by weight settings saved.");
}

static esp_err_t handle_preset_post(httpd_req_t *req) {
  char body[768];
  char slot_text[8];
  char preset_name[CTRL_PRESET_NAME_LEN];
  char temperature_text[24];
  char infuse_text[24];
  char pause_text[24];
  char bbw_mode_code[24];
  char bbw_dose_1_text[24];
  char bbw_dose_2_text[24];
  char *endptr = NULL;
  ctrl_preset_t preset = {0};
  ctrl_values_t machine_values = {0};
  uint32_t loaded_mask = 0;
  uint32_t feature_mask = 0;
  float parsed_value = 0.0f;
  int preset_index = -1;
  int received = 0;
  bool bbw_available = false;
  char banner[96];

  if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
    return ESP_FAIL;
  }

  while (received < req->content_len) {
    int chunk = httpd_req_recv(req, body + received, req->content_len - received);
    if (chunk <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form data");
      return ESP_FAIL;
    }
    received += chunk;
  }
  body[received] = '\0';

  parse_form_value(body, "preset_slot", slot_text, sizeof(slot_text));
  parse_form_value(body, "preset_name", preset_name, sizeof(preset_name));
  parse_form_value(body, "temperature_c", temperature_text, sizeof(temperature_text));
  parse_form_value(body, "infuse_s", infuse_text, sizeof(infuse_text));
  parse_form_value(body, "pause_s", pause_text, sizeof(pause_text));

  if (slot_text[0] == '\0') {
    return send_setup_page(req, "Preset slot is required.");
  }

  preset_index = (int)strtol(slot_text, &endptr, 10);
  if (endptr == slot_text || *endptr != '\0' || preset_index < 0 || preset_index >= CTRL_PRESET_COUNT) {
    return send_setup_page(req, "Preset slot is invalid.");
  }

  if (ctrl_state_load_preset_slot(preset_index, &preset) != ESP_OK) {
    return send_setup_page(req, "Could not load the stored preset.");
  }

  parsed_value = strtof(temperature_text, &endptr);
  if (temperature_text[0] == '\0' || endptr == temperature_text || *endptr != '\0' || parsed_value < 80.0f || parsed_value > 103.0f) {
    return send_setup_page(req, "Temperature must stay between 80.0 C and 103.0 C.");
  }
  preset.values.temperature_c = parsed_value;

  parsed_value = strtof(infuse_text, &endptr);
  if (infuse_text[0] == '\0' || endptr == infuse_text || *endptr != '\0' || parsed_value < 0.0f || parsed_value > 9.0f) {
    return send_setup_page(req, "Prebrewing In must stay between 0.0 s and 9.0 s.");
  }
  preset.values.infuse_s = parsed_value;

  parsed_value = strtof(pause_text, &endptr);
  if (pause_text[0] == '\0' || endptr == pause_text || *endptr != '\0' || parsed_value < 0.0f || parsed_value > 9.0f) {
    return send_setup_page(req, "Prebrewing Out must stay between 0.0 s and 9.0 s.");
  }
  preset.values.pause_s = parsed_value;

  copy_text(preset.name, sizeof(preset.name), preset_name);

  if (lm_ctrl_machine_link_get_values(&machine_values, &loaded_mask, &feature_mask)) {
    bbw_available = (feature_mask & LM_CTRL_MACHINE_FEATURE_BBW) != 0;
  }

  if (bbw_available) {
    parse_form_value(body, "bbw_mode", bbw_mode_code, sizeof(bbw_mode_code));
    parse_form_value(body, "bbw_dose_1", bbw_dose_1_text, sizeof(bbw_dose_1_text));
    parse_form_value(body, "bbw_dose_2", bbw_dose_2_text, sizeof(bbw_dose_2_text));

    if (bbw_mode_code[0] == '\0' || bbw_dose_1_text[0] == '\0' || bbw_dose_2_text[0] == '\0') {
      return send_setup_page(req, "BBW mode and both dose targets are required when BBW is available.");
    }

    preset.values.bbw_mode = ctrl_bbw_mode_from_cloud_code(bbw_mode_code);

    parsed_value = strtof(bbw_dose_1_text, &endptr);
    if (endptr == bbw_dose_1_text || *endptr != '\0' || parsed_value < 5.0f || parsed_value > 100.0f) {
      return send_setup_page(req, "BBW Dose 1 must stay between 5.0 g and 100.0 g.");
    }
    preset.values.bbw_dose_1_g = parsed_value;

    parsed_value = strtof(bbw_dose_2_text, &endptr);
    if (endptr == bbw_dose_2_text || *endptr != '\0' || parsed_value < 5.0f || parsed_value > 100.0f) {
      return send_setup_page(req, "BBW Dose 2 must stay between 5.0 g and 100.0 g.");
    }
    preset.values.bbw_dose_2_g = parsed_value;
  }

  if (ctrl_state_store_preset_slot(preset_index, &preset) != ESP_OK) {
    return send_setup_page(req, "Could not store the preset.");
  }

  snprintf(banner, sizeof(banner), "Preset %d saved.", preset_index + 1);
  return send_setup_page(req, banner);
}

static esp_err_t handle_wifi_scan_get(httpd_req_t *req) {
  wifi_scan_config_t scan_config = {0};
  wifi_ap_record_t *ap_records = NULL;
  uint16_t ap_count = 0;
  bool first = true;
  char escaped_ssid[128];
  esp_err_t ret;

  ret = esp_wifi_scan_start(&scan_config, true);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Wi-Fi scan failed to start: %s", esp_err_to_name(ret));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi scan failed");
    return ret;
  }

  ret = esp_wifi_scan_get_ap_num(&ap_count);
  if (ret != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read scan count");
    return ret;
  }

  if (ap_count > 20) {
    ap_count = 20;
  }

  if (ap_count > 0) {
    ap_records = calloc(ap_count, sizeof(*ap_records));
    if (ap_records == NULL) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
      return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (ret != ESP_OK) {
      free(ap_records);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read scan results");
      return ret;
    }
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, "{\"ssids\":[");

  for (uint16_t i = 0; i < ap_count; ++i) {
    if (ap_records[i].ssid[0] == '\0') {
      continue;
    }

    json_escape_text((const char *)ap_records[i].ssid, escaped_ssid, sizeof(escaped_ssid));
    httpd_resp_sendstr_chunk(req, first ? "\"" : ",\"");
    httpd_resp_sendstr_chunk(req, escaped_ssid);
    httpd_resp_sendstr_chunk(req, "\"");
    first = false;
  }

  httpd_resp_sendstr_chunk(req, "]}");
  httpd_resp_sendstr_chunk(req, NULL);
  free(ap_records);
  return ESP_OK;
}

static esp_err_t handle_debug_screenshot_get(httpd_req_t *req) {
#if LV_USE_SNAPSHOT
  enum {
    BMP_FILE_HEADER_SIZE = 14,
    BMP_INFO_HEADER_SIZE = 40,
    BMP_HEADER_SIZE = BMP_FILE_HEADER_SIZE + BMP_INFO_HEADER_SIZE,
  };
  lv_img_dsc_t *snapshot = NULL;
  uint8_t *row_buffer = NULL;
  size_t row_stride = 0;
  size_t row_stride_padded = 0;
  uint32_t pixel_data_size = 0;
  uint32_t file_size = 0;
  uint8_t header[BMP_HEADER_SIZE] = {0};
  esp_err_t ret = ESP_OK;

  if (esp_lv_adapter_lock(-1) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not lock UI");
    return ESP_FAIL;
  }

  snapshot = lv_snapshot_take(lv_scr_act(), LV_IMG_CF_TRUE_COLOR);
  esp_lv_adapter_unlock();

  if (snapshot == NULL || snapshot->data == NULL || snapshot->header.w == 0 || snapshot->header.h == 0) {
    if (snapshot != NULL) {
      if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_snapshot_free(snapshot);
        esp_lv_adapter_unlock();
      }
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not capture screenshot");
    return ESP_FAIL;
  }

  row_stride = (size_t)snapshot->header.w * 3U;
  row_stride_padded = (row_stride + 3U) & ~((size_t)3U);
  pixel_data_size = (uint32_t)(row_stride_padded * (size_t)snapshot->header.h);
  file_size = (uint32_t)(sizeof(header) + pixel_data_size);
  row_buffer = malloc(row_stride_padded);
  if (row_buffer == NULL) {
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
      lv_snapshot_free(snapshot);
      esp_lv_adapter_unlock();
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_ERR_NO_MEM;
  }

  header[0] = 'B';
  header[1] = 'M';
  write_u32_le(&header[2], file_size);
  write_u32_le(&header[10], BMP_HEADER_SIZE);
  write_u32_le(&header[14], BMP_INFO_HEADER_SIZE);
  write_i32_le(&header[18], snapshot->header.w);
  write_i32_le(&header[22], snapshot->header.h);
  write_u16_le(&header[26], 1);
  write_u16_le(&header[28], 24);
  write_u32_le(&header[34], pixel_data_size);

  httpd_resp_set_type(req, "image/bmp");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"lm-controller-screenshot.bmp\"");

  ret = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
  if (ret == ESP_OK) {
    const lv_color_t *pixels = (const lv_color_t *)snapshot->data;

    for (int32_t y = snapshot->header.h - 1; y >= 0 && ret == ESP_OK; --y) {
      memset(row_buffer, 0, row_stride_padded);
      for (int32_t x = 0; x < snapshot->header.w; ++x) {
        const lv_color32_t color32 = {.full = lv_color_to32(pixels[(size_t)y * (size_t)snapshot->header.w + (size_t)x])};
        const size_t offset = (size_t)x * 3U;
        row_buffer[offset + 0] = color32.ch.blue;
        row_buffer[offset + 1] = color32.ch.green;
        row_buffer[offset + 2] = color32.ch.red;
      }
      ret = httpd_resp_send_chunk(req, (const char *)row_buffer, row_stride_padded);
    }
  }

  free(row_buffer);
  if (esp_lv_adapter_lock(-1) == ESP_OK) {
    lv_snapshot_free(snapshot);
    esp_lv_adapter_unlock();
  }

  if (ret == ESP_OK) {
    ret = httpd_resp_send_chunk(req, NULL, 0);
  }

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Screenshot response failed: %s", esp_err_to_name(ret));
  }
  return ret;
#else
  httpd_resp_send_err(req, HTTPD_501_NOT_IMPLEMENTED, "LVGL snapshot support is disabled");
  return ESP_FAIL;
#endif
}

static esp_err_t send_captive_redirect_page(httpd_req_t *req) {
  bool portal_running = false;
  const char *target = "/";

  lock_state();
  portal_running = s_state.portal_running;
  unlock_state();
  if (portal_running) {
    target = "http://" LM_CTRL_PORTAL_IP "/";
  }

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr_chunk(req,
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<link rel=\"icon\" href=\"data:,\">"
    "<meta http-equiv=\"refresh\" content=\"0; url=");
  httpd_resp_sendstr_chunk(req, target);
  httpd_resp_sendstr_chunk(req,
    "\">"
    "<title>Controller Setup</title></head><body style=\"font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#120d0a;color:#f4efe7;padding:24px;\">"
    "<p>Open <a style=\"color:#ffc685\" href=\"");
  httpd_resp_sendstr_chunk(req, target);
  httpd_resp_sendstr_chunk(req,
    "\">Controller Setup</a>.</p>"
    "<script>location.replace('");
  httpd_resp_sendstr_chunk(req, target);
  httpd_resp_sendstr_chunk(req, "');</script></body></html>");
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

static esp_err_t send_captive_redirect_response(httpd_req_t *req) {
  bool portal_running = false;
  const char *target = "/";

  lock_state();
  portal_running = s_state.portal_running;
  unlock_state();
  if (portal_running) {
    target = "http://" LM_CTRL_PORTAL_IP "/";
  }

  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", target);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_sendstr(req, "Redirecting to controller setup.");
  return ESP_OK;
}

static esp_err_t handle_captive_probe_get(httpd_req_t *req) {
  return send_captive_redirect_response(req);
}

static esp_err_t handle_captive_get(httpd_req_t *req) {
  return send_captive_redirect_page(req);
}

static esp_err_t start_http_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = handle_root_get,
  };
  httpd_uri_t controller_uri = {
    .uri = "/controller",
    .method = HTTP_POST,
    .handler = handle_controller_post,
  };
  httpd_uri_t controller_logo_uri = {
    .uri = "/controller-logo",
    .method = HTTP_POST,
    .handler = handle_controller_logo_post,
  };
  httpd_uri_t controller_logo_clear_uri = {
    .uri = "/controller-logo-clear",
    .method = HTTP_POST,
    .handler = handle_controller_logo_clear_post,
  };
  httpd_uri_t wifi_uri = {
    .uri = "/wifi",
    .method = HTTP_POST,
    .handler = handle_wifi_post,
  };
  httpd_uri_t cloud_uri = {
    .uri = "/cloud",
    .method = HTTP_POST,
    .handler = handle_cloud_post,
  };
  httpd_uri_t cloud_refresh_uri = {
    .uri = "/cloud-refresh",
    .method = HTTP_POST,
    .handler = handle_cloud_refresh_post,
  };
  httpd_uri_t cloud_machine_uri = {
    .uri = "/cloud-machine",
    .method = HTTP_POST,
    .handler = handle_cloud_machine_post,
  };
  httpd_uri_t bbw_uri = {
    .uri = "/bbw",
    .method = HTTP_POST,
    .handler = handle_bbw_post,
  };
  httpd_uri_t preset_uri = {
    .uri = "/preset",
    .method = HTTP_POST,
    .handler = handle_preset_post,
  };
  httpd_uri_t scan_uri = {
    .uri = "/wifi-scan",
    .method = HTTP_GET,
    .handler = handle_wifi_scan_get,
  };
  httpd_uri_t screenshot_uri = {
    .uri = "/debug/screenshot.bmp",
    .method = HTTP_GET,
    .handler = handle_debug_screenshot_get,
  };
  httpd_uri_t reboot_uri = {
    .uri = "/reboot",
    .method = HTTP_POST,
    .handler = handle_reboot_post,
  };
  httpd_uri_t reset_network_uri = {
    .uri = "/reset-network",
    .method = HTTP_POST,
    .handler = handle_reset_network_post,
  };
  httpd_uri_t factory_reset_uri = {
    .uri = "/factory-reset",
    .method = HTTP_POST,
    .handler = handle_factory_reset_post,
  };
  httpd_uri_t android_probe_uri = {
    .uri = "/generate_204",
    .method = HTTP_GET,
    .handler = handle_captive_probe_get,
  };
  httpd_uri_t android_fallback_probe_uri = {
    .uri = "/gen_204",
    .method = HTTP_GET,
    .handler = handle_captive_probe_get,
  };
  httpd_uri_t apple_probe_uri = {
    .uri = "/hotspot-detect.html",
    .method = HTTP_GET,
    .handler = handle_captive_probe_get,
  };
  httpd_uri_t apple_success_uri = {
    .uri = "/library/test/success.html",
    .method = HTTP_GET,
    .handler = handle_captive_probe_get,
  };
  httpd_uri_t windows_connect_uri = {
    .uri = "/connecttest.txt",
    .method = HTTP_GET,
    .handler = handle_captive_probe_get,
  };
  httpd_uri_t windows_ncsi_uri = {
    .uri = "/ncsi.txt",
    .method = HTTP_GET,
    .handler = handle_captive_probe_get,
  };
  httpd_uri_t captive_uri = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = handle_captive_get,
  };

  if (s_state.http_server != NULL) {
    return ESP_OK;
  }

  config.max_uri_handlers = 24;
  config.stack_size = 12288;
  config.uri_match_fn = httpd_uri_match_wildcard;
  ESP_RETURN_ON_ERROR(httpd_start(&s_state.http_server, &config), TAG, "Failed to start setup web server");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &root_uri), TAG, "Failed to register root handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &controller_uri), TAG, "Failed to register controller handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &controller_logo_uri), TAG, "Failed to register controller logo handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &controller_logo_clear_uri), TAG, "Failed to register controller logo clear handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &wifi_uri), TAG, "Failed to register Wi-Fi handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &cloud_uri), TAG, "Failed to register cloud handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &cloud_refresh_uri), TAG, "Failed to register cloud refresh handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &cloud_machine_uri), TAG, "Failed to register cloud machine handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &bbw_uri), TAG, "Failed to register brew by weight handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &preset_uri), TAG, "Failed to register preset handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &scan_uri), TAG, "Failed to register scan handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &screenshot_uri), TAG, "Failed to register screenshot handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &reboot_uri), TAG, "Failed to register reboot handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &reset_network_uri), TAG, "Failed to register network reset handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &factory_reset_uri), TAG, "Failed to register factory reset handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &android_probe_uri), TAG, "Failed to register Android captive probe");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &android_fallback_probe_uri), TAG, "Failed to register Android fallback captive probe");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &apple_probe_uri), TAG, "Failed to register Apple captive probe");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &apple_success_uri), TAG, "Failed to register Apple success probe");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &windows_connect_uri), TAG, "Failed to register Windows connect probe");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &windows_ncsi_uri), TAG, "Failed to register Windows NCSI probe");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &captive_uri), TAG, "Failed to register captive handler");
  return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  bool should_retry = false;

  (void)arg;
  (void)event_data;

  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_AP_START:
        break;
      case WIFI_EVENT_AP_STOP:
        lock_state();
        s_state.portal_running = false;
        mark_status_dirty_locked();
        unlock_state();
        stop_dns_server();
        break;
      case WIFI_EVENT_STA_DISCONNECTED:
        lock_state();
        s_state.sta_connected = false;
        s_state.sta_connecting = s_state.has_credentials;
        s_state.sta_ip[0] = '\0';
        s_state.cloud_connected = false;
        clear_cached_cloud_access_token_locked();
        should_retry = s_state.has_credentials;
        mark_status_dirty_locked();
        unlock_state();
        stop_cloud_websocket(false);
        break;
      default:
        break;
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *ip_event = (ip_event_got_ip_t *)event_data;

    lock_state();
    s_state.sta_connected = true;
    s_state.sta_connecting = false;
    snprintf(s_state.sta_ip, sizeof(s_state.sta_ip), IPSTR, IP2STR(&ip_event->ip_info.ip));
    mark_status_dirty_locked();
    unlock_state();
    ensure_station_dns();
    (void)disable_setup_ap();
    (void)lm_ctrl_wifi_request_cloud_probe();
  }

  if (should_retry) {
    esp_wifi_connect();
  }
}

esp_err_t lm_ctrl_wifi_init(void) {
  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  wifi_mode_t initial_mode = WIFI_MODE_STA;
  bool start_in_setup = false;
  esp_err_t ret;

  if (s_state.initialized) {
    return ESP_OK;
  }

  s_state.lock = xSemaphoreCreateMutex();
  if (s_state.lock == NULL) {
    return ESP_ERR_NO_MEM;
  }
  s_state.cloud_auth_lock = xSemaphoreCreateMutex();
  if (s_state.cloud_auth_lock == NULL) {
    return ESP_ERR_NO_MEM;
  }

  fill_portal_ssid_locked();
  fill_portal_password_locked();
  lock_state();
  ret = load_credentials_locked();
  unlock_state();
  ESP_RETURN_ON_ERROR(ret, TAG, "Failed to load Wi-Fi credentials");

  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Failed to init esp-netif");
  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    return ret;
  }

  s_state.sta_netif = esp_netif_create_default_wifi_sta();
  s_state.ap_netif = esp_netif_create_default_wifi_ap();
  ESP_RETURN_ON_FALSE(s_state.sta_netif != NULL && s_state.ap_netif != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create Wi-Fi netifs");
  ESP_RETURN_ON_ERROR(update_mdns_hostname(), TAG, "Failed to initialize mDNS");

  ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Failed to init Wi-Fi driver");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL), TAG, "Failed to register Wi-Fi handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL), TAG, "Failed to register IP handler");

  lock_state();
  if (!s_state.has_credentials) {
    initial_mode = WIFI_MODE_APSTA;
    start_in_setup = true;
  }
  unlock_state();

  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Failed to set Wi-Fi storage mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(initial_mode), TAG, "Failed to set initial Wi-Fi mode");

  if (start_in_setup) {
    ESP_RETURN_ON_ERROR(configure_ap(), TAG, "Failed to configure initial setup AP");
  }

  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start Wi-Fi");
  ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Failed to disable Wi-Fi power save");

  s_state.wifi_started = true;
  s_state.initialized = true;
  lock_state();
  mark_status_dirty_locked();
  unlock_state();

  if (start_in_setup) {
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "Failed to start setup portal");
    lock_state();
    s_state.portal_running = true;
    mark_status_dirty_locked();
    unlock_state();
    ESP_RETURN_ON_ERROR(start_dns_server(), TAG, "Failed to start captive DNS");
  } else {
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "Failed to start setup web server");
    ESP_RETURN_ON_ERROR(apply_station_credentials(), TAG, "Failed to apply saved station credentials");
    lock_state();
    s_state.sta_connecting = true;
    mark_status_dirty_locked();
    unlock_state();
  }

  ESP_LOGI(TAG, "Wi-Fi service initialized");
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_start_portal(void) {
  bool sta_connected = false;

  if (!s_state.initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  lock_state();
  sta_connected = s_state.sta_connected;
  if (s_state.portal_running && s_state.http_server != NULL) {
    unlock_state();
    return ESP_OK;
  }
  unlock_state();

  if (sta_connected) {
    ESP_LOGI(TAG, "Skipping setup AP because STA is already connected");
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(configure_ap(), TAG, "Failed to configure setup AP");
  ESP_RETURN_ON_ERROR(start_http_server(), TAG, "Failed to start setup portal");

  lock_state();
  s_state.portal_running = true;
  mark_status_dirty_locked();
  unlock_state();
  ESP_RETURN_ON_ERROR(start_dns_server(), TAG, "Failed to start captive DNS");
  ESP_LOGI(TAG, "Setup portal started on http://192.168.4.1");
  return ESP_OK;
}

void lm_ctrl_wifi_get_info(lm_ctrl_wifi_info_t *info) {
  if (info == NULL) {
    return;
  }

  lock_state();
  memset(info, 0, sizeof(*info));
  info->has_credentials = s_state.has_credentials;
  info->portal_running = s_state.portal_running;
  info->sta_connecting = s_state.sta_connecting;
  info->sta_connected = s_state.sta_connected;
  info->language = s_state.language;
  info->has_cloud_credentials = s_state.has_cloud_credentials;
  info->cloud_connected = s_state.cloud_connected;
  info->has_machine_selection = s_state.has_machine_selection;
  info->has_custom_logo = s_state.has_custom_logo;
  copy_text(info->portal_ssid, sizeof(info->portal_ssid), s_state.portal_ssid);
  copy_text(info->portal_password, sizeof(info->portal_password), s_state.portal_password);
  copy_text(info->sta_ssid, sizeof(info->sta_ssid), s_state.sta_ssid);
  copy_text(info->hostname, sizeof(info->hostname), s_state.hostname);
  copy_text(info->sta_ip, sizeof(info->sta_ip), s_state.sta_ip);
  copy_text(info->cloud_username, sizeof(info->cloud_username), s_state.cloud_username);
  copy_text(info->machine_name, sizeof(info->machine_name), s_state.selected_machine.name);
  copy_text(info->machine_model, sizeof(info->machine_model), s_state.selected_machine.model);
  copy_text(info->machine_serial, sizeof(info->machine_serial), s_state.selected_machine.serial);
  unlock_state();
}

const lv_img_dsc_t *lm_ctrl_wifi_get_custom_logo(void) {
  bool has_custom_logo = false;

  lock_state();
  has_custom_logo = s_state.has_custom_logo;
  if (has_custom_logo) {
    s_custom_logo_dsc.data = s_state.custom_logo_blob;
  }
  unlock_state();

  return has_custom_logo ? &s_custom_logo_dsc : NULL;
}

void lm_ctrl_wifi_get_setup_qr_payload(char *buffer, size_t buffer_size) {
  lm_ctrl_wifi_info_t info;
  const char *portal_host;

  if (buffer == NULL || buffer_size == 0) {
    return;
  }

  lm_ctrl_wifi_get_info(&info);

  if (info.portal_running) {
    if (info.portal_ssid[0] == '\0' || info.portal_password[0] == '\0') {
      buffer[0] = '\0';
      return;
    }

    snprintf(
      buffer,
      buffer_size,
      "WIFI:T:WPA;S:%s;P:%s;;",
      info.portal_ssid,
      info.portal_password
    );
    return;
  }

  if (info.sta_connected) {
    if (info.sta_ip[0] != '\0') {
      snprintf(buffer, buffer_size, "http://%s/", info.sta_ip);
      return;
    }

    portal_host = info.hostname[0] != '\0' ? info.hostname : LM_CTRL_WIFI_DEFAULT_HOSTNAME;
    snprintf(buffer, buffer_size, "http://%s.local/", portal_host);
    return;
  }

  if (info.portal_ssid[0] == '\0' || info.portal_password[0] == '\0') {
    buffer[0] = '\0';
    return;
  }

  snprintf(
    buffer,
    buffer_size,
    "WIFI:T:WPA;S:%s;P:%s;;",
    info.portal_ssid,
    info.portal_password
  );
}

esp_err_t lm_ctrl_wifi_request_cloud_probe(void) {
  lock_state();
  if (!(s_state.initialized && s_state.sta_connected && s_state.has_cloud_credentials)) {
    unlock_state();
    set_cloud_connected(false);
    return ESP_ERR_INVALID_STATE;
  }
  if (s_state.cloud_probe_task != NULL ||
      s_state.cloud_ws_task != NULL ||
      s_state.cloud_ws_transport_connected ||
      s_state.cloud_ws_connected ||
      s_state.cloud_http_requests_in_flight != 0) {
    unlock_state();
    return ESP_OK;
  }

  if (
    xTaskCreate(
      cloud_probe_task,
      "lm_cloud_probe",
      LM_CTRL_CLOUD_PROBE_STACK_SIZE,
      NULL,
      4,
      &s_state.cloud_probe_task
    ) != pdPASS
  ) {
    s_state.cloud_probe_task = NULL;
    unlock_state();
    return ESP_ERR_NO_MEM;
  }
  unlock_state();
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_request_live_updates(void) {
  bool should_run = false;

  lock_state();
  should_run = should_run_cloud_websocket_locked();
  if (!should_run) {
    unlock_state();
    stop_cloud_websocket(false);
    return ESP_ERR_INVALID_STATE;
  }
  unlock_state();

  ESP_LOGI(TAG, "Live cloud updates requested");
  return ensure_cloud_websocket_task();
}

bool lm_ctrl_wifi_live_updates_active(void) {
  bool active = false;

  lock_state();
  active = s_state.cloud_ws_task != NULL ||
           s_state.cloud_ws_transport_connected ||
           s_state.cloud_ws_connected;
  unlock_state();

  return active;
}

bool lm_ctrl_wifi_live_updates_connected(void) {
  bool connected = false;

  lock_state();
  connected = s_state.cloud_ws_connected;
  unlock_state();

  return connected;
}

bool lm_ctrl_wifi_get_shot_timer_info(lm_ctrl_shot_timer_info_t *info) {
  bool websocket_connected = false;
  bool brew_active = false;
  int64_t brew_start_epoch_ms = 0;
  int64_t brew_start_local_us = 0;
  int64_t elapsed_us = 0;

  if (info == NULL) {
    return false;
  }

  memset(info, 0, sizeof(*info));

  lock_state();
  websocket_connected = s_state.cloud_ws_connected;
  brew_active = s_state.brew_active;
  brew_start_epoch_ms = s_state.brew_start_epoch_ms;
  brew_start_local_us = s_state.brew_start_local_us;
  unlock_state();

  info->websocket_connected = websocket_connected;
  info->brew_active = brew_active;
  if (!brew_active) {
    return true;
  }

  if (brew_start_epoch_ms > 0) {
    int64_t now_epoch_ms = current_epoch_ms();
    if (now_epoch_ms > brew_start_epoch_ms) {
      elapsed_us = (now_epoch_ms - brew_start_epoch_ms) * 1000LL;
    }
  }
  if (elapsed_us <= 0 && brew_start_local_us > 0) {
    elapsed_us = esp_timer_get_time() - brew_start_local_us;
  }
  if (elapsed_us < 0) {
    elapsed_us = 0;
  }

  info->available = true;
  info->seconds = (uint32_t)(elapsed_us / 1000000LL);
  return true;
}

void lm_ctrl_wifi_format_status(char *buffer, size_t buffer_size) {
  lm_ctrl_wifi_info_t info;

  if (buffer == NULL || buffer_size == 0) {
    return;
  }

  lm_ctrl_wifi_get_info(&info);
  if (info.language == CTRL_LANGUAGE_DE) {
    format_controller_status_de(&info, buffer, buffer_size);
  } else {
    format_controller_status_en(&info, buffer, buffer_size);
  }
}

uint32_t lm_ctrl_wifi_status_version(void) {
  uint32_t version;

  lock_state();
  version = s_state.status_version;
  unlock_state();
  return version;
}

bool lm_ctrl_wifi_get_machine_binding(lm_ctrl_machine_binding_t *binding) {
  if (binding == NULL) {
    return false;
  }

  lock_state();
  memset(binding, 0, sizeof(*binding));
  binding->configured = s_state.has_machine_selection;
  copy_text(binding->serial, sizeof(binding->serial), s_state.selected_machine.serial);
  copy_text(binding->name, sizeof(binding->name), s_state.selected_machine.name);
  copy_text(binding->model, sizeof(binding->model), s_state.selected_machine.model);
  copy_text(binding->communication_key, sizeof(binding->communication_key), s_state.selected_machine.communication_key);
  unlock_state();
  return binding->configured;
}

static void parse_cloud_command_response_body(const char *response_body, lm_ctrl_cloud_command_result_t *result) {
  cJSON *root = NULL;
  cJSON *command_item = NULL;
  cJSON *id_item;
  cJSON *status_item;
  cJSON *error_code_item;

  if (result == NULL) {
    return;
  }

  memset(result, 0, sizeof(*result));
  if (response_body == NULL || response_body[0] == '\0') {
    return;
  }

  root = cJSON_Parse(response_body);
  if (root == NULL) {
    return;
  }

  if (cJSON_IsArray(root)) {
    command_item = cJSON_GetArrayItem(root, 0);
  } else if (cJSON_IsObject(root)) {
    command_item = root;
  }

  if (!cJSON_IsObject(command_item)) {
    cJSON_Delete(root);
    return;
  }

  id_item = cJSON_GetObjectItemCaseSensitive(command_item, "id");
  status_item = cJSON_GetObjectItemCaseSensitive(command_item, "status");
  error_code_item = cJSON_GetObjectItemCaseSensitive(command_item, "errorCode");
  if (cJSON_IsString(id_item) && id_item->valuestring != NULL) {
    copy_text(result->command_id, sizeof(result->command_id), id_item->valuestring);
  }
  if (cJSON_IsString(status_item) && status_item->valuestring != NULL) {
    copy_text(result->command_status, sizeof(result->command_status), status_item->valuestring);
  }
  if (cJSON_IsString(error_code_item) && error_code_item->valuestring != NULL) {
    copy_text(result->error_code, sizeof(result->error_code), error_code_item->valuestring);
  }
  result->accepted = result->command_id[0] != '\0';
  cJSON_Delete(root);
}

esp_err_t lm_ctrl_wifi_execute_machine_command(
  const char *command,
  const char *json_body,
  lm_ctrl_cloud_command_result_t *result,
  char *status_text,
  size_t status_text_size
) {
  char username[96];
  char password[128];
  char serial[32];
  char access_token[1024];
  char auth_header[1152];
  char installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t private_key_der_len = 0;
  char timestamp[24];
  char nonce[LM_CTRL_INSTALLATION_ID_LEN];
  char signature_b64[256];
  char path[192];
  char *response_body = NULL;
  int status_code = 0;
  esp_err_t ret;
  lm_ctrl_cloud_http_header_t headers[6];

  if (status_text != NULL && status_text_size > 0) {
    status_text[0] = '\0';
  }
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
  }

  if (command == NULL || command[0] == '\0' || json_body == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  lock_state();
  copy_text(username, sizeof(username), s_state.cloud_username);
  copy_text(password, sizeof(password), s_state.cloud_password);
  copy_text(serial, sizeof(serial), s_state.selected_machine.serial);
  unlock_state();

  if (username[0] == '\0' || password[0] == '\0') {
    if (status_text != NULL && status_text_size > 0) {
      snprintf(status_text, status_text_size, "Cloud credentials are missing.");
    }
    return ESP_ERR_INVALID_STATE;
  }
  if (serial[0] == '\0') {
    if (status_text != NULL && status_text_size > 0) {
      snprintf(status_text, status_text_size, "No machine selected.");
    }
    return ESP_ERR_INVALID_STATE;
  }

  ret = fetch_cloud_access_token_cached(username, password, access_token, sizeof(access_token), status_text, status_text_size);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = ensure_cloud_installation();
  if (ret != ESP_OK) {
    if (status_text != NULL && status_text_size > 0) {
      snprintf(status_text, status_text_size, "Cloud installation data is missing.");
    }
    return ret;
  }

  lock_state();
  copy_text(installation_id, sizeof(installation_id), s_state.cloud_installation_id);
  memcpy(secret, s_state.cloud_secret, sizeof(secret));
  memcpy(private_key_der, s_state.cloud_private_key_der, s_state.cloud_private_key_der_len);
  private_key_der_len = s_state.cloud_private_key_der_len;
  unlock_state();

  ret = build_signed_request_headers(
    installation_id,
    secret,
    private_key_der,
    private_key_der_len,
    timestamp,
    sizeof(timestamp),
    nonce,
    sizeof(nonce),
    signature_b64,
    sizeof(signature_b64)
  );
  if (ret != ESP_OK) {
    if (status_text != NULL && status_text_size > 0) {
      snprintf(status_text, status_text_size, "Cloud command request signing failed.");
    }
    return ret;
  }

  snprintf(auth_header, sizeof(auth_header), "Bearer %s", access_token);
  snprintf(path, sizeof(path), "%s/%s/command/%s", LM_CTRL_CLOUD_THINGS_PATH, serial, command);

  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "Content-Type", .value = "application/json" };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "Authorization", .value = auth_header };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "X-App-Installation-Id", .value = installation_id };
  headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "X-Timestamp", .value = timestamp };
  headers[4] = (lm_ctrl_cloud_http_header_t){ .name = "X-Nonce", .value = nonce };
  headers[5] = (lm_ctrl_cloud_http_header_t){ .name = "X-Request-Signature", .value = signature_b64 };

  ret = http_request_capture(
    LM_CTRL_CLOUD_HOST,
    path,
    LM_CTRL_CLOUD_PORT,
    HTTP_METHOD_POST,
    headers,
    sizeof(headers) / sizeof(headers[0]),
    json_body,
    12000,
    &response_body,
    &status_code
  );
  if (ret != ESP_OK) {
    if (status_text != NULL && status_text_size > 0 && status_text[0] == '\0') {
      snprintf(status_text, status_text_size, "Cloud command request failed.");
    }
    return ret;
  }

  if (status_code < 200 || status_code >= 300) {
    if (status_code == 401) {
      clear_cached_cloud_access_token();
    }
    ESP_LOGW(TAG, "Cloud command %s failed with status %d: %.200s", command, status_code, response_body != NULL ? response_body : "");
    if (status_text != NULL && status_text_size > 0) {
      snprintf(status_text, status_text_size, "Cloud command failed with status %d.", status_code);
    }
    free(response_body);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Cloud command %s accepted for %s", command, serial);
  if (result != NULL) {
    parse_cloud_command_response_body(response_body, result);
  }
  if (status_text != NULL && status_text_size > 0) {
    if (result != NULL && result->command_id[0] != '\0') {
      snprintf(status_text, status_text_size, "Cloud command accepted (%s).", result->command_id);
    } else {
      snprintf(status_text, status_text_size, "Cloud command accepted.");
    }
  }

  free(response_body);
  return ESP_OK;
}

static esp_err_t fetch_dashboard_root(cJSON **out_root, char *error_text, size_t error_text_size) {
  char username[96];
  char password[128];
  char serial[32];
  char access_token[1024];
  char auth_header[1152];
  char installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
  size_t private_key_der_len = 0;
  char timestamp[24];
  char nonce[LM_CTRL_INSTALLATION_ID_LEN];
  char signature_b64[256];
  char path[192];
  char *response_body = NULL;
  int status_code = 0;
  esp_err_t ret;
  lm_ctrl_cloud_http_header_t headers[5];

  if (out_root == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_root = NULL;

  lock_state();
  copy_text(username, sizeof(username), s_state.cloud_username);
  copy_text(password, sizeof(password), s_state.cloud_password);
  copy_text(serial, sizeof(serial), s_state.selected_machine.serial);
  unlock_state();

  if (username[0] == '\0' || password[0] == '\0' || serial[0] == '\0') {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Dashboard unavailable: cloud setup incomplete.");
    }
    return ESP_ERR_INVALID_STATE;
  }

  ret = fetch_cloud_access_token_cached(username, password, access_token, sizeof(access_token), error_text, error_text_size);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = ensure_cloud_installation();
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Dashboard unavailable: cloud installation missing.");
    }
    return ret;
  }

  lock_state();
  copy_text(installation_id, sizeof(installation_id), s_state.cloud_installation_id);
  memcpy(secret, s_state.cloud_secret, sizeof(secret));
  memcpy(private_key_der, s_state.cloud_private_key_der, s_state.cloud_private_key_der_len);
  private_key_der_len = s_state.cloud_private_key_der_len;
  unlock_state();

  ret = build_signed_request_headers(
    installation_id,
    secret,
    private_key_der,
    private_key_der_len,
    timestamp,
    sizeof(timestamp),
    nonce,
    sizeof(nonce),
    signature_b64,
    sizeof(signature_b64)
  );
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Dashboard request signing failed.");
    }
    return ret;
  }

  snprintf(auth_header, sizeof(auth_header), "Bearer %s", access_token);
  snprintf(path, sizeof(path), "%s/%s/dashboard", LM_CTRL_CLOUD_THINGS_PATH, serial);
  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "Authorization", .value = auth_header };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "X-App-Installation-Id", .value = installation_id };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "X-Timestamp", .value = timestamp };
  headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "X-Nonce", .value = nonce };
  headers[4] = (lm_ctrl_cloud_http_header_t){ .name = "X-Request-Signature", .value = signature_b64 };

  ret = http_request_capture(
    LM_CTRL_CLOUD_HOST,
    path,
    LM_CTRL_CLOUD_PORT,
    HTTP_METHOD_GET,
    headers,
    sizeof(headers) / sizeof(headers[0]),
    NULL,
    12000,
    &response_body,
    &status_code
  );
  if (ret != ESP_OK) {
    if (error_text != NULL && error_text_size > 0 && error_text[0] == '\0') {
      snprintf(error_text, error_text_size, "Dashboard request failed.");
    }
    ESP_LOGW(TAG, "Dashboard request failed: %s", error_text != NULL ? error_text : "request error");
    return ret;
  }

  if (status_code < 200 || status_code >= 300) {
    if (status_code == 401) {
      clear_cached_cloud_access_token();
    }
    ESP_LOGW(TAG, "Dashboard failed with status %d: %.200s", status_code, response_body != NULL ? response_body : "");
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Dashboard request failed with status %d.", status_code);
    }
    free(response_body);
    return ESP_FAIL;
  }

  *out_root = cJSON_Parse(response_body);
  free(response_body);
  if (!cJSON_IsObject(*out_root)) {
    ESP_LOGW(TAG, "Dashboard response was not valid JSON.");
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Dashboard response was not valid JSON.");
    }
    cJSON_Delete(*out_root);
    *out_root = NULL;
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_fetch_prebrewing_values(float *seconds_in, float *seconds_out) {
  char error_text[160];
  cJSON *root = NULL;
  cJSON *widgets;
  cJSON *widget;
  esp_err_t ret;

  if (seconds_in == NULL || seconds_out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  error_text[0] = '\0';
  ret = fetch_dashboard_root(&root, error_text, sizeof(error_text));
  if (ret != ESP_OK) {
    return ret;
  }

  widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
  if (!cJSON_IsArray(widgets)) {
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  cJSON_ArrayForEach(widget, widgets) {
    if (parse_prebrew_widget_values(widget, seconds_in, seconds_out)) {
      cJSON_Delete(root);
      return ESP_OK;
    }
  }

  cJSON_Delete(root);
  return ESP_ERR_NOT_FOUND;
}

esp_err_t lm_ctrl_wifi_fetch_dashboard_values(ctrl_values_t *values, uint32_t *loaded_mask, uint32_t *feature_mask) {
  char error_text[160];
  cJSON *root = NULL;
  esp_err_t ret;

  if (values == NULL || loaded_mask == NULL || feature_mask == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  error_text[0] = '\0';
  ret = fetch_dashboard_root(&root, error_text, sizeof(error_text));
  if (ret != ESP_OK) {
    return ret;
  }

  ret = parse_dashboard_root_values(root, values, loaded_mask, feature_mask, NULL, NULL);
  cJSON_Delete(root);
  return ret;
}

esp_err_t lm_ctrl_wifi_log_prebrew_dashboard_state(char *status_text, size_t status_text_size) {
  char summary[192];
  char error_text[160];
  cJSON *root = NULL;
  cJSON *widgets;
  cJSON *widget;
  bool found_summary = false;
  esp_err_t ret;

  if (status_text != NULL && status_text_size > 0) {
    status_text[0] = '\0';
  }

  error_text[0] = '\0';
  ret = fetch_dashboard_root(&root, error_text, sizeof(error_text));
  if (ret != ESP_OK) {
    if (status_text != NULL && status_text_size > 0) {
      copy_text(status_text, status_text_size, error_text);
    }
    return ret;
  }

  widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
  if (!cJSON_IsArray(widgets)) {
    if (status_text != NULL && status_text_size > 0) {
      snprintf(status_text, status_text_size, "Dashboard response had no widgets.");
    }
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  summary[0] = '\0';
  cJSON_ArrayForEach(widget, widgets) {
    summarize_prebrew_widget(widget, summary, sizeof(summary), &found_summary);
  }
  cJSON_Delete(root);

  if (!found_summary) {
    ESP_LOGW(TAG, "No prebrew widget found in dashboard.");
    if (status_text != NULL && status_text_size > 0) {
      snprintf(status_text, status_text_size, "No prebrew widget found in dashboard.");
    }
    return ESP_ERR_NOT_FOUND;
  }

  ESP_LOGI(TAG, "Dashboard prebrew summary: %s", summary);
  if (status_text != NULL && status_text_size > 0) {
    copy_text(status_text, status_text_size, summary);
  }
  return ESP_OK;
}
