/**
 * Wi-Fi/AP/DNS façade and setup portal orchestration for the controller.
 *
 * This module now owns the setup AP, captive DNS, station orchestration, and
 * shared Wi-Fi/setup runtime state. Portal routes, page rendering, persisted
 * settings, signed cloud requests, and cloud live updates are delegated to
 * dedicated internal modules.
 */
#include "wifi_setup.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "cloud_live_updates.h"
#include "cloud_session.h"
#include "board_leds.h"
#include "controller_settings.h"
#include "setup_portal_routes.h"
#include "storage_security.h"
#include "wifi_reconnect_policy.h"
#include "wifi_setup_internal.h"
#include "mdns.h"

static const char *TAG = "lm_wifi";

lm_ctrl_wifi_state_t s_state = {
  .hostname = LM_CTRL_WIFI_DEFAULT_HOSTNAME,
  .language = CTRL_LANGUAGE_EN,
  .dns_socket = -1,
};
lv_img_dsc_t s_custom_logo_dsc = {
  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = LM_CTRL_CUSTOM_LOGO_WIDTH,
  .header.h = LM_CTRL_CUSTOM_LOGO_HEIGHT,
  .data_size = LM_CTRL_CUSTOM_LOGO_BLOB_SIZE,
};

void lock_state(void) {
  if (s_state.lock != NULL) {
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
  }
}

void unlock_state(void) {
  if (s_state.lock != NULL) {
    xSemaphoreGive(s_state.lock);
  }
}

void mark_status_dirty_locked(void) {
  s_state.status_version++;
}

void set_cloud_connected(bool connected) {
  lock_state();
  if (!connected) {
    clear_cloud_machine_status_locked();
  }
  if (s_state.cloud_connected != connected) {
    s_state.cloud_connected = connected;
    mark_status_dirty_locked();
  }
  unlock_state();
}

void clear_cloud_machine_status_locked(void) {
  if (!lm_ctrl_cloud_machine_status_is_known(&s_state.cloud_machine_status)) {
    return;
  }

  memset(&s_state.cloud_machine_status, 0, sizeof(s_state.cloud_machine_status));
  mark_status_dirty_locked();
}

void clear_cloud_machine_status(void) {
  lock_state();
  clear_cloud_machine_status_locked();
  unlock_state();
}

void set_cloud_machine_status(const lm_ctrl_cloud_machine_status_t *status) {
  lm_ctrl_cloud_machine_status_t next_status = {0};

  if (status != NULL) {
    next_status = *status;
  }

  lock_state();
  if (memcmp(&s_state.cloud_machine_status, &next_status, sizeof(next_status)) != 0) {
    s_state.cloud_machine_status = next_status;
    mark_status_dirty_locked();
  }
  unlock_state();
}

void merge_cloud_machine_status(const lm_ctrl_cloud_machine_status_t *status) {
  lm_ctrl_cloud_machine_status_t merged_status;

  if (status == NULL) {
    return;
  }

  lock_state();
  merged_status = s_state.cloud_machine_status;
  if (status->connected_known) {
    merged_status.connected_known = true;
    merged_status.connected = status->connected;
  }
  if (status->offline_mode_known) {
    merged_status.offline_mode_known = true;
    merged_status.offline_mode = status->offline_mode;
  }
  if (memcmp(&s_state.cloud_machine_status, &merged_status, sizeof(merged_status)) != 0) {
    s_state.cloud_machine_status = merged_status;
    mark_status_dirty_locked();
  }
  unlock_state();
}

int64_t current_epoch_ms(void) {
  struct timeval tv = {0};

  if (gettimeofday(&tv, NULL) != 0) {
    return 0;
  }
  if (tv.tv_sec < 1700000000) {
    return 0;
  }
  return ((int64_t)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}

void note_cloud_server_epoch_ms(int64_t server_epoch_ms) {
  const int64_t captured_us = esp_timer_get_time();
  const int64_t current_ms = current_epoch_ms();
  bool should_set_wall_clock = false;

  if (server_epoch_ms <= 0) {
    return;
  }

  lock_state();
  s_state.cloud_server_epoch_ms = server_epoch_ms;
  s_state.cloud_server_epoch_captured_us = captured_us;
  unlock_state();

  should_set_wall_clock = current_ms == 0;
  if (!should_set_wall_clock && current_ms > 0) {
    int64_t delta_ms = current_ms - server_epoch_ms;

    if (delta_ms < 0) {
      delta_ms = -delta_ms;
    }
    should_set_wall_clock = delta_ms > 2000LL;
  }
  if (should_set_wall_clock) {
    struct timeval tv = {
      .tv_sec = (time_t)(server_epoch_ms / 1000LL),
      .tv_usec = (suseconds_t)((server_epoch_ms % 1000LL) * 1000LL),
    };

    if (settimeofday(&tv, NULL) == 0) {
      ESP_LOGI(TAG, "Synchronized wall clock from cloud server date: %lld", (long long)server_epoch_ms);
    } else {
      ESP_LOGW(TAG, "Failed to set wall clock from cloud server date");
    }
  }
}

int64_t current_cloud_epoch_ms(void) {
  int64_t epoch_ms = current_epoch_ms();
  int64_t captured_epoch_ms = 0;
  int64_t captured_us = 0;
  int64_t now_us = 0;

  if (epoch_ms > 0) {
    return epoch_ms;
  }

  lock_state();
  captured_epoch_ms = s_state.cloud_server_epoch_ms;
  captured_us = s_state.cloud_server_epoch_captured_us;
  unlock_state();

  if (captured_epoch_ms <= 0 || captured_us <= 0) {
    return 0;
  }

  now_us = esp_timer_get_time();
  if (now_us <= captured_us) {
    return captured_epoch_ms;
  }

  return captured_epoch_ms + ((now_us - captured_us) / 1000LL);
}

void clear_cached_cloud_access_token_locked(void) {
  s_state.cloud_access_token[0] = '\0';
  s_state.cloud_access_token_valid_until_us = 0;
}

void clear_cached_cloud_access_token(void) {
  lock_state();
  clear_cached_cloud_access_token_locked();
  unlock_state();
}

void clear_fleet_locked(void) {
  memset(s_state.fleet, 0, sizeof(s_state.fleet));
  s_state.fleet_count = 0;
}

void clear_selected_machine_locked(void) {
  memset(&s_state.selected_machine, 0, sizeof(s_state.selected_machine));
  s_state.has_machine_selection = false;
  clear_cloud_machine_status_locked();
}

void clear_custom_logo_locked(void) {
  s_state.has_custom_logo = false;
  s_state.custom_logo_schema_version = 0;
  memset(s_state.custom_logo_blob, 0, sizeof(s_state.custom_logo_blob));
}

static void delayed_restart_task(void *arg) {
  const int delay_ms = (int)(intptr_t)arg;

  vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 250));
  if (lm_ctrl_leds_prepare_for_reset() != ESP_OK) {
    ESP_LOGW(TAG, "Failed to release LED ring before restart");
  }
  vTaskDelay(pdMS_TO_TICKS(20));
  esp_restart();
}

static esp_err_t update_mdns_hostname(void);
static esp_err_t start_dns_server(void);
static void stop_dns_server(void);
static esp_err_t disable_setup_ap(void);
static esp_err_t restart_setup_portal_after_network_reset(void);

typedef struct {
  uint32_t delay_ms;
  uint32_t generation;
} lm_ctrl_sta_reconnect_task_args_t;

static void reset_sta_reconnect_locked(void) {
  s_state.sta_reconnect_pending = false;
  s_state.sta_disconnect_count = 0;
  s_state.sta_retry_delay_ms = 0;
  s_state.sta_retry_generation++;
}

static void sta_reconnect_task(void *arg) {
  lm_ctrl_sta_reconnect_task_args_t *task_args = (lm_ctrl_sta_reconnect_task_args_t *)arg;
  uint32_t delay_ms = 0;
  uint32_t generation = 0;
  uint8_t disconnect_count = 0;
  bool should_connect = false;

  if (task_args != NULL) {
    delay_ms = task_args->delay_ms;
    generation = task_args->generation;
  }
  if (delay_ms > 0) {
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }

  lock_state();
  if (task_args != NULL && s_state.sta_retry_generation == generation) {
    s_state.sta_reconnect_pending = false;
    s_state.sta_retry_delay_ms = 0;
    disconnect_count = s_state.sta_disconnect_count;
    if (s_state.has_credentials && !s_state.sta_connected) {
      s_state.sta_connecting = true;
      mark_status_dirty_locked();
      should_connect = true;
    }
  }
  unlock_state();

  free(task_args);
  if (should_connect) {
    ESP_LOGI(
      TAG,
      "Retrying Wi-Fi station connect after %u ms (disconnect count %u)",
      (unsigned)delay_ms,
      (unsigned)disconnect_count
    );
    if (esp_wifi_connect() != ESP_OK) {
      ESP_LOGW(TAG, "Scheduled Wi-Fi reconnect could not start");
    }
  }
  vTaskDelete(NULL);
}

static esp_err_t schedule_sta_reconnect(uint32_t delay_ms) {
  lm_ctrl_sta_reconnect_task_args_t *task_args = NULL;
  uint32_t generation = 0;

  lock_state();
  if (s_state.sta_reconnect_pending) {
    unlock_state();
    return ESP_OK;
  }
  generation = s_state.sta_retry_generation;
  s_state.sta_reconnect_pending = true;
  s_state.sta_retry_delay_ms = delay_ms;
  mark_status_dirty_locked();
  unlock_state();

  task_args = calloc(1, sizeof(*task_args));
  if (task_args == NULL) {
    lock_state();
    if (s_state.sta_retry_generation == generation) {
      s_state.sta_reconnect_pending = false;
      s_state.sta_retry_delay_ms = 0;
      mark_status_dirty_locked();
    }
    unlock_state();
    return ESP_ERR_NO_MEM;
  }

  task_args->delay_ms = delay_ms;
  task_args->generation = generation;
  if (xTaskCreate(sta_reconnect_task, "lm_sta_retry", 3072, task_args, 5, NULL) != pdPASS) {
    free(task_args);
    lock_state();
    if (s_state.sta_retry_generation == generation) {
      s_state.sta_reconnect_pending = false;
      s_state.sta_retry_delay_ms = 0;
      mark_status_dirty_locked();
    }
    unlock_state();
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

static void fill_portal_ssid_locked(void) {
  uint8_t mac[6] = {0};

  if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
    snprintf(s_state.portal_ssid, sizeof(s_state.portal_ssid), "LM-CTRL-SETUP");
    return;
  }

  snprintf(s_state.portal_ssid, sizeof(s_state.portal_ssid), "LM-CTRL-%02X%02X", mac[4], mac[5]);
}

static esp_err_t ensure_portal_password(void) {
  char random_suffix[9];
  char generated_password[9];
  bool needs_password = false;

  lock_state();
  needs_password = s_state.portal_password[0] == '\0';
  unlock_state();
  if (!needs_password) {
    return ESP_OK;
  }

  fill_random_hex(random_suffix, sizeof(random_suffix), 4);
  snprintf(generated_password, sizeof(generated_password), "%s", random_suffix);
  secure_zero(random_suffix, sizeof(random_suffix));
  ESP_RETURN_ON_ERROR(lm_ctrl_settings_save_portal_password(generated_password), TAG, "Failed to persist setup AP password");
  secure_zero(generated_password, sizeof(generated_password));
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_store_credentials(const char *ssid, const char *password, const char *hostname, ctrl_language_t language) {
  esp_err_t ret = lm_ctrl_settings_save_wifi_credentials(ssid, password, hostname, language);

  if (ret == ESP_OK) {
    if (s_state.sta_netif != NULL) {
      esp_netif_set_hostname(s_state.sta_netif, hostname[0] != '\0' ? hostname : LM_CTRL_WIFI_DEFAULT_HOSTNAME);
    }
    update_mdns_hostname();
  }

  return ret;
}

esp_err_t lm_ctrl_wifi_save_controller_preferences(const char *hostname, ctrl_language_t language) {
  char effective_hostname[33];
  esp_err_t ret = ESP_OK;

  copy_text(effective_hostname, sizeof(effective_hostname), hostname);
  if (effective_hostname[0] == '\0') {
    copy_text(effective_hostname, sizeof(effective_hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  }

  ret = lm_ctrl_settings_save_controller_preferences(effective_hostname, language);
  if (ret == ESP_OK) {
    if (s_state.sta_netif != NULL) {
      esp_netif_set_hostname(s_state.sta_netif, effective_hostname);
    }
    update_mdns_hostname();
  }

  return ret;
}

esp_err_t lm_ctrl_wifi_save_controller_logo(uint8_t schema_version, const uint8_t *logo_data, size_t logo_size) {
  return lm_ctrl_settings_save_controller_logo(schema_version, logo_data, logo_size);
}

esp_err_t lm_ctrl_wifi_clear_controller_logo(void) {
  return lm_ctrl_settings_clear_controller_logo();
}

esp_err_t lm_ctrl_wifi_save_cloud_provisioning(
  const char *installation_id,
  const uint8_t *secret,
  const uint8_t *private_key_der,
  size_t private_key_der_len
) {
  return lm_ctrl_settings_save_cloud_provisioning(installation_id, secret, private_key_der, private_key_der_len);
}

esp_err_t lm_ctrl_wifi_save_web_admin_password(const char *password) {
  return lm_ctrl_settings_save_web_admin_password(password);
}

esp_err_t lm_ctrl_wifi_clear_web_admin_password(void) {
  return lm_ctrl_settings_clear_web_admin_password();
}

bool lm_ctrl_wifi_verify_web_admin_password(const char *password) {
  return lm_ctrl_settings_verify_web_admin_password(password);
}

esp_err_t lm_ctrl_wifi_set_debug_screenshot_enabled(bool enabled) {
  return lm_ctrl_settings_set_debug_screenshot_enabled(enabled);
}

esp_err_t lm_ctrl_wifi_set_heat_display_enabled(bool enabled) {
  return lm_ctrl_settings_set_heat_display_enabled(enabled);
}

esp_err_t lm_ctrl_wifi_schedule_reboot(void) {
  if (xTaskCreate(delayed_restart_task, "lm_reboot", 3072, (void *)(intptr_t)1200, 5, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_reset_network(void) {
  esp_err_t ret;

  lm_ctrl_cloud_live_updates_stop(false);
  ret = lm_ctrl_settings_reset_network();
  if (ret != ESP_OK) {
    return ret;
  }

  return restart_setup_portal_after_network_reset();
}

esp_err_t lm_ctrl_wifi_factory_reset(void) {
  esp_err_t ret;

  lm_ctrl_cloud_live_updates_stop(false);
  ret = lm_ctrl_settings_factory_reset();
  if (ret != ESP_OK) {
    return ret;
  }
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

static esp_err_t restart_setup_portal_after_network_reset(void) {
  if (!s_state.initialized || !s_state.wifi_started) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t ret = esp_wifi_disconnect();
  if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED && ret != ESP_ERR_WIFI_CONN) {
    return ret;
  }

  ret = esp_wifi_set_mode(WIFI_MODE_STA);
  if (ret != ESP_OK) {
    return ret;
  }

  lock_state();
  s_state.portal_running = false;
  s_state.sta_connecting = false;
  s_state.sta_connected = false;
  s_state.sta_ip[0] = '\0';
  reset_sta_reconnect_locked();
  mark_status_dirty_locked();
  unlock_state();
  stop_dns_server();

  ESP_RETURN_ON_ERROR(ensure_portal_password(), TAG, "Failed to recreate setup AP password");
  ESP_RETURN_ON_ERROR(configure_ap(), TAG, "Failed to reconfigure setup AP after network reset");
  ESP_RETURN_ON_ERROR(lm_ctrl_setup_portal_start_http_server(), TAG, "Failed to ensure setup portal web server");

  lock_state();
  s_state.portal_running = true;
  mark_status_dirty_locked();
  unlock_state();

  ESP_RETURN_ON_ERROR(start_dns_server(), TAG, "Failed to restart captive DNS after network reset");
  ESP_LOGI(TAG, "Network reset completed; setup portal resumed on http://192.168.4.1");
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_apply_station_credentials(void) {
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
  reset_sta_reconnect_locked();
  copy_text(ssid, sizeof(ssid), s_state.sta_ssid);
  copy_text(password, sizeof(password), s_state.sta_password);
  copy_text(hostname, sizeof(hostname), s_state.hostname);
  mode = s_state.portal_running ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  s_state.sta_connecting = true;
  mark_status_dirty_locked();
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
    // This captive DNS path intentionally stays minimal: any plain DNS query
    // with room for one answer gets a single A/IN record pointing at the setup
    // portal. We do not parse names, AAAA, EDNS, or multi-question packets
    // because the goal is only to trigger common captive portal flows and keep
    // the fallback manual URL (`http://192.168.4.1/`) deterministic.
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

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  lm_ctrl_wifi_reconnect_plan_t reconnect_plan = {0};
  bool retry_already_pending = false;
  bool should_enable_setup_ap = false;
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
        clear_cloud_machine_status_locked();
        clear_cached_cloud_access_token_locked();
        if (s_state.has_credentials) {
          if (s_state.sta_reconnect_pending) {
            retry_already_pending = true;
            reconnect_plan.disconnect_count = s_state.sta_disconnect_count;
            reconnect_plan.retry_delay_ms = s_state.sta_retry_delay_ms;
          } else {
            reconnect_plan = lm_ctrl_wifi_reconnect_plan_next(s_state.sta_disconnect_count, s_state.portal_running);
            s_state.sta_disconnect_count = reconnect_plan.disconnect_count;
            s_state.sta_retry_delay_ms = reconnect_plan.retry_delay_ms;
            should_enable_setup_ap = reconnect_plan.should_enable_setup_ap;
          }
          should_retry = true;
        } else {
          reset_sta_reconnect_locked();
        }
        mark_status_dirty_locked();
        unlock_state();
        lm_ctrl_cloud_live_updates_stop(false);
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
    reset_sta_reconnect_locked();
    mark_status_dirty_locked();
    unlock_state();
    ensure_station_dns();
    (void)disable_setup_ap();
    (void)lm_ctrl_wifi_request_cloud_probe();
  }

  if (should_enable_setup_ap) {
    ESP_LOGW(TAG, "Wi-Fi reconnect threshold reached; keeping setup AP available during STA recovery");
    (void)lm_ctrl_wifi_start_portal();
  }
  if (should_retry) {
    if (retry_already_pending) {
      ESP_LOGI(
        TAG,
        "Wi-Fi STA disconnected again while reconnect is already pending (%u ms, disconnect count %u)",
        (unsigned)reconnect_plan.retry_delay_ms,
        (unsigned)reconnect_plan.disconnect_count
      );
    } else {
      ESP_LOGI(
        TAG,
        "Wi-Fi STA disconnected; scheduling reconnect in %u ms (disconnect count %u)",
        (unsigned)reconnect_plan.retry_delay_ms,
        (unsigned)reconnect_plan.disconnect_count
      );
    }
    if (!retry_already_pending && schedule_sta_reconnect(reconnect_plan.retry_delay_ms) != ESP_OK) {
      ESP_LOGW(TAG, "Reconnect backoff scheduling failed; retrying immediately");
      (void)esp_wifi_connect();
    }
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

  ESP_RETURN_ON_ERROR(lm_ctrl_secure_storage_init(), TAG, "Failed to initialize encrypted controller storage");
  ret = lm_ctrl_settings_load();
  ESP_RETURN_ON_ERROR(ret, TAG, "Failed to load Wi-Fi credentials");
  ret = lm_ctrl_settings_ensure_cloud_provisioning();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Cloud provisioning auto-generation unavailable: %s", esp_err_to_name(ret));
  }
  lock_state();
  fill_portal_ssid_locked();
  unlock_state();
  ESP_RETURN_ON_ERROR(ensure_portal_password(), TAG, "Failed to create setup AP password");

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
    ESP_RETURN_ON_ERROR(lm_ctrl_setup_portal_start_http_server(), TAG, "Failed to start setup portal");
    lock_state();
    s_state.portal_running = true;
    mark_status_dirty_locked();
    unlock_state();
    ESP_RETURN_ON_ERROR(start_dns_server(), TAG, "Failed to start captive DNS");
  } else {
    ESP_RETURN_ON_ERROR(lm_ctrl_setup_portal_start_http_server(), TAG, "Failed to start setup web server");
    ESP_RETURN_ON_ERROR(lm_ctrl_wifi_apply_station_credentials(), TAG, "Failed to apply saved station credentials");
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
  ESP_RETURN_ON_ERROR(lm_ctrl_setup_portal_start_http_server(), TAG, "Failed to start setup portal");

  lock_state();
  s_state.portal_running = true;
  mark_status_dirty_locked();
  unlock_state();
  ESP_RETURN_ON_ERROR(start_dns_server(), TAG, "Failed to start captive DNS");
  ESP_LOGI(TAG, "Setup portal started on http://192.168.4.1");
  return ESP_OK;
}

void lm_ctrl_wifi_get_info(lm_ctrl_wifi_info_t *info) {
  lm_ctrl_cloud_machine_t effective_machine = {0};
  bool has_effective_machine = false;

  if (info == NULL) {
    return;
  }

  has_effective_machine = lm_ctrl_settings_get_effective_selected_machine(&effective_machine);

  lock_state();
  memset(info, 0, sizeof(*info));
  info->has_credentials = s_state.has_credentials;
  info->portal_running = s_state.portal_running;
  info->sta_connecting = s_state.sta_connecting;
  info->sta_connected = s_state.sta_connected;
  info->language = s_state.language;
  info->has_cloud_credentials = s_state.has_cloud_credentials;
  info->cloud_connected = s_state.cloud_connected;
  info->cloud_probe_active = s_state.cloud_probe_task != NULL;
  info->cloud_live_updates_active =
    s_state.cloud_ws_task != NULL ||
    s_state.cloud_ws_transport_connected ||
    s_state.cloud_ws_connected;
  info->cloud_ws_transport_connected = s_state.cloud_ws_transport_connected;
  info->cloud_ws_connected = s_state.cloud_ws_connected;
  info->cloud_machine_status = s_state.cloud_machine_status;
  info->cloud_machine_status_known = lm_ctrl_cloud_machine_status_is_known(&s_state.cloud_machine_status);
  info->machine_cloud_online = lm_ctrl_cloud_machine_status_is_online(&s_state.cloud_machine_status);
  info->has_machine_selection = has_effective_machine;
  info->has_custom_logo = s_state.has_custom_logo;
  info->has_cloud_provisioning = s_state.has_cloud_provisioning;
  info->heat_display_enabled = s_state.heat_display_enabled;
  info->debug_screenshot_enabled = s_state.debug_screenshot_enabled;
  info->web_auth_mode = s_state.web_auth_mode;
  info->cloud_http_requests_in_flight = s_state.cloud_http_requests_in_flight;
  copy_text(info->portal_ssid, sizeof(info->portal_ssid), s_state.portal_ssid);
  copy_text(info->portal_password, sizeof(info->portal_password), s_state.portal_password);
  copy_text(info->sta_ssid, sizeof(info->sta_ssid), s_state.sta_ssid);
  copy_text(info->hostname, sizeof(info->hostname), s_state.hostname);
  copy_text(info->sta_ip, sizeof(info->sta_ip), s_state.sta_ip);
  copy_text(info->cloud_username, sizeof(info->cloud_username), s_state.cloud_username);
  copy_text(info->machine_name, sizeof(info->machine_name), has_effective_machine ? effective_machine.name : "");
  copy_text(info->machine_model, sizeof(info->machine_model), has_effective_machine ? effective_machine.model : "");
  copy_text(info->machine_serial, sizeof(info->machine_serial), has_effective_machine ? effective_machine.serial : "");
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
  return lm_ctrl_cloud_live_updates_request_probe();
}

esp_err_t lm_ctrl_wifi_request_live_updates(void) {
  return lm_ctrl_cloud_live_updates_request_start();
}

bool lm_ctrl_wifi_get_shot_timer_info(lm_ctrl_shot_timer_info_t *info) {
  return lm_ctrl_cloud_live_updates_get_shot_timer_info(info);
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
  return lm_ctrl_settings_get_machine_binding(binding);
}

esp_err_t lm_ctrl_wifi_execute_machine_command(
  const char *command,
  const char *json_body,
  lm_ctrl_cloud_command_result_t *result,
  char *status_text,
  size_t status_text_size
) {
  return lm_ctrl_cloud_session_execute_machine_command(command, json_body, result, status_text, status_text_size);
}

esp_err_t lm_ctrl_wifi_fetch_prebrewing_values(float *seconds_in, float *seconds_out) {
  return lm_ctrl_cloud_session_fetch_prebrewing_values(seconds_in, seconds_out);
}

esp_err_t lm_ctrl_wifi_fetch_dashboard_values(
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  lm_ctrl_machine_heat_info_t *heat_info,
  lm_ctrl_machine_water_status_t *water_status
) {
  return lm_ctrl_cloud_session_fetch_dashboard_values(values, loaded_mask, feature_mask, heat_info, water_status);
}
