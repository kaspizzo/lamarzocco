#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include "cloud_api.h"
#include "lm_ctrl_cloud_endpoints.h"
#include "lm_ctrl_nvs_keys.h"
#include "lm_ctrl_secure_utils.h"
#include "wifi_setup.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LM_CTRL_ENABLE_CLOUD_LIVE_UPDATES 1

#define LM_CTRL_CLOUD_PROBE_STACK_SIZE (24 * 1024)
#define LM_CTRL_CLOUD_WS_STACK_SIZE (16 * 1024)
#define LM_CTRL_PORTAL_IP "192.168.4.1"
#define LM_CTRL_PORTAL_DNS_PORT 53
#define LM_CTRL_WEB_SESSION_IDLE_TIMEOUT_US (60LL * 60LL * 1000LL * 1000LL)

/** Shared Wi-Fi/setup/cloud runtime state used by the internal setup modules. */
typedef struct {
  bool initialized;
  bool wifi_started;
  bool has_credentials;
  bool has_cloud_credentials;
  bool cloud_connected;
  bool has_machine_selection;
  bool has_custom_logo;
  bool has_cloud_provisioning;
  bool heat_display_enabled;
  bool debug_screenshot_enabled;
  bool portal_running;
  bool sta_connecting;
  bool sta_connected;
  lm_ctrl_web_auth_mode_t web_auth_mode;
  ctrl_language_t language;
  uint8_t web_admin_salt[LM_CTRL_WEB_ADMIN_SALT_LEN];
  uint8_t web_admin_hash[LM_CTRL_WEB_ADMIN_HASH_LEN];
  uint32_t web_admin_iterations;
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
  lm_ctrl_cloud_machine_status_t cloud_machine_status;
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
  bool sta_reconnect_pending;
  bool brew_active;
  int64_t brew_start_epoch_ms;
  int64_t brew_start_local_us;
  char web_session_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN];
  char web_csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN];
  int64_t web_session_valid_until_us;
  char cloud_access_token[LM_CTRL_CLOUD_WS_TOKEN_LEN];
  int64_t cloud_access_token_valid_until_us;
  int64_t cloud_server_epoch_ms;
  int64_t cloud_server_epoch_captured_us;
  char cloud_ws_access_token[LM_CTRL_CLOUD_WS_TOKEN_LEN];
  char cloud_ws_frame[LM_CTRL_CLOUD_WS_FRAME_MAX];
  size_t cloud_ws_frame_len;
  uint8_t sta_disconnect_count;
  uint8_t cloud_http_requests_in_flight;
  uint32_t sta_retry_delay_ms;
  uint32_t sta_retry_generation;
  esp_netif_t *sta_netif;
  esp_netif_t *ap_netif;
  /*
   * Lock ordering contract for the Wi-Fi/setup/cloud runtime:
   *
   * 1. `lock` protects the shared `s_state` snapshot and should only be held
   *    for short, in-memory reads/writes.
   * 2. `cloud_auth_lock` serializes token/sign-in work and must only be taken
   *    after `lock` has been released again.
   * 3. Blocking work such as HTTP, websocket operations, NVS I/O, or other
   *    cross-module callbacks should run on copied state after releasing
   *    `lock`, so future changes keep a simple apply-then-notify pattern.
   */
  SemaphoreHandle_t lock;
  SemaphoreHandle_t cloud_auth_lock;
} lm_ctrl_wifi_state_t;

extern lm_ctrl_wifi_state_t s_state;
extern lv_img_dsc_t s_custom_logo_dsc;

/** Acquire the shared Wi-Fi/setup runtime mutex for short, non-blocking state access only. */
void lock_state(void);
/** Release the shared Wi-Fi/setup runtime mutex before any blocking or cross-module work. */
void unlock_state(void);
/** Bump the monotonic status version while the state lock is already held. */
void mark_status_dirty_locked(void);

/** Return the current wall clock in milliseconds, or 0 if the clock is not usable yet. */
int64_t current_epoch_ms(void);
/** Note a trusted cloud/server UTC epoch and use it as a fallback time base. */
void note_cloud_server_epoch_ms(int64_t server_epoch_ms);
/** Return the best available UTC epoch in milliseconds from wall clock or cloud fallback. */
int64_t current_cloud_epoch_ms(void);
/** Persist Wi-Fi credentials and controller-local network settings. */
esp_err_t lm_ctrl_wifi_store_credentials(const char *ssid, const char *password, const char *hostname, ctrl_language_t language);
/** Apply the persisted station credentials to the active Wi-Fi driver. */
esp_err_t lm_ctrl_wifi_apply_station_credentials(void);
/** Update the cached cloud connectivity flag and status version if it changed. */
void set_cloud_connected(bool connected);
/** Clear the selected-machine cloud reachability snapshot while the state lock is already held. */
void clear_cloud_machine_status_locked(void);
/** Clear the cached cloud access token while the state lock is already held. */
void clear_cached_cloud_access_token_locked(void);
/** Clear the cached cloud access token. */
void clear_cached_cloud_access_token(void);
/** Clear the selected-machine cloud reachability snapshot. */
void clear_cloud_machine_status(void);
/** Replace the selected-machine cloud reachability snapshot. */
void set_cloud_machine_status(const lm_ctrl_cloud_machine_status_t *status);
/** Merge any known reachability fields into the selected-machine cloud snapshot. */
void merge_cloud_machine_status(const lm_ctrl_cloud_machine_status_t *status);
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
