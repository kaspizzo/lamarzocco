#include "setup_portal_presenter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine_link.h"
#include "setup_portal_http.h"
#include "setup_portal_page.h"
#include "wifi_setup_internal.h"

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
  ctrl_state_t controller_state;
  ctrl_values_t dashboard_values;
  uint32_t dashboard_loaded_mask;
  uint32_t dashboard_feature_mask;
  lm_ctrl_wifi_info_t info;
  lm_ctrl_machine_link_info_t machine_info;
  lm_ctrl_cloud_machine_t fleet[LM_CTRL_CLOUD_MAX_FLEET];
  size_t fleet_count;
} lm_ctrl_setup_page_ctx_t;

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
      case '\'':
        replacement = "&#39;";
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

esp_err_t lm_ctrl_setup_portal_send_response(httpd_req_t *req, const char *banner) {
  lm_ctrl_setup_page_ctx_t *ctx = calloc(1, sizeof(*ctx));
  lm_ctrl_setup_portal_view_t page_view = {0};
  ctrl_state_t preset_defaults;
  esp_err_t ret;
  const char *history_target = lm_ctrl_setup_portal_history_target(req != NULL ? req->uri : NULL);

  if (ctx == NULL) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_ERR_NO_MEM;
  }

  ctrl_state_init(&preset_defaults);
  ctrl_state_init(&ctx->controller_state);
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
  }
  if (ctrl_state_load(&ctx->controller_state) != ESP_OK) {
    ctx->controller_state = preset_defaults;
  }

  memcpy(page_view.status_html, ctx->status_html, sizeof(page_view.status_html));
  memcpy(page_view.machine_status, ctx->machine_status, sizeof(page_view.machine_status));
  memcpy(page_view.debug_status_html, ctx->debug_status_html, sizeof(page_view.debug_status_html));
  memcpy(page_view.banner_html, ctx->banner_html, sizeof(page_view.banner_html));
  memcpy(page_view.ssid_html, ctx->ssid_html, sizeof(page_view.ssid_html));
  memcpy(page_view.hostname_html, ctx->hostname_html, sizeof(page_view.hostname_html));
  memcpy(page_view.cloud_user_html, ctx->cloud_user_html, sizeof(page_view.cloud_user_html));
  memcpy(page_view.local_url_html, ctx->local_url_html, sizeof(page_view.local_url_html));
  memcpy(page_view.selected_machine_html, ctx->selected_machine_html, sizeof(page_view.selected_machine_html));
  page_view.dashboard_values = ctx->dashboard_values;
  page_view.dashboard_loaded_mask = ctx->dashboard_loaded_mask;
  page_view.dashboard_feature_mask = ctx->dashboard_feature_mask;
  memcpy(page_view.presets, ctx->controller_state.presets, sizeof(page_view.presets));
  page_view.preset_count = ctx->controller_state.preset_count;
  page_view.temperature_step_c = ctx->controller_state.temperature_step_c;
  page_view.time_step_s = ctx->controller_state.time_step_s;
  page_view.info = ctx->info;
  page_view.machine_info = ctx->machine_info;
  memcpy(page_view.fleet, ctx->fleet, sizeof(page_view.fleet));
  page_view.fleet_count = ctx->fleet_count;

  ret = lm_ctrl_setup_portal_send_page(req, &page_view, history_target);
  free(ctx);
  return ret;
}
