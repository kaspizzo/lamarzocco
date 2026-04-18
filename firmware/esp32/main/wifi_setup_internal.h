#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include "cloud_api.h"
#include "wifi_setup.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/** Shared Wi-Fi/setup/cloud runtime state used by the internal setup modules. */
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

extern lm_ctrl_wifi_state_t s_state;
extern lv_img_dsc_t s_custom_logo_dsc;

/** Acquire the shared Wi-Fi/setup runtime mutex. */
void lock_state(void);
/** Release the shared Wi-Fi/setup runtime mutex. */
void unlock_state(void);
/** Bump the monotonic status version while the state lock is already held. */
void mark_status_dirty_locked(void);
/** Copy text into a fixed-size destination buffer with guaranteed NUL termination. */
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

/** Return the current wall clock in milliseconds, or 0 if the clock is not usable yet. */
int64_t current_epoch_ms(void);
/** Persist Wi-Fi credentials and controller-local network settings. */
esp_err_t lm_ctrl_wifi_store_credentials(const char *ssid, const char *password, const char *hostname, ctrl_language_t language);
/** Apply the persisted station credentials to the active Wi-Fi driver. */
esp_err_t lm_ctrl_wifi_apply_station_credentials(void);
/** Update the cached cloud connectivity flag and status version if it changed. */
void set_cloud_connected(bool connected);
/** Clear the cached cloud access token while the state lock is already held. */
void clear_cached_cloud_access_token_locked(void);
/** Clear the cached cloud access token. */
void clear_cached_cloud_access_token(void);
/** Store the cached cloud access token with its derived expiry deadline. */
void store_cached_cloud_access_token(const char *access_token);
/** Copy the cached cloud access token if it is still valid. */
bool copy_cached_cloud_access_token(char *buffer, size_t buffer_size);
/** Clear the in-memory fleet snapshot while the state lock is already held. */
void clear_fleet_locked(void);
/** Clear the active machine binding while the state lock is already held. */
void clear_selected_machine_locked(void);
/** Clear the custom logo state while the state lock is already held. */
void clear_custom_logo_locked(void);

#ifdef __cplusplus
}
#endif
