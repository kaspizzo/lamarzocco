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
#include "controller_settings.h"
#include "setup_portal_routes.h"
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
  if (s_state.cloud_connected != connected) {
    s_state.cloud_connected = connected;
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
}

void clear_custom_logo_locked(void) {
  s_state.has_custom_logo = false;
  s_state.custom_logo_schema_version = 0;
  memset(s_state.custom_logo_blob, 0, sizeof(s_state.custom_logo_blob));
}

static void delayed_restart_task(void *arg) {
  const int delay_ms = (int)(intptr_t)arg;

  vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 250));
  esp_restart();
}

static esp_err_t update_mdns_hostname(void);
static esp_err_t start_dns_server(void);
static void stop_dns_server(void);
static esp_err_t disable_setup_ap(void);

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

  return lm_ctrl_wifi_schedule_reboot();
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
  ret = lm_ctrl_settings_load();
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

esp_err_t lm_ctrl_wifi_fetch_dashboard_values(ctrl_values_t *values, uint32_t *loaded_mask, uint32_t *feature_mask) {
  return lm_ctrl_cloud_session_fetch_dashboard_values(values, loaded_mask, feature_mask);
}
