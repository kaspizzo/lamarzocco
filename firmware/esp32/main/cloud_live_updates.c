#include "cloud_live_updates.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "cloud_api.h"
#include "cloud_machine_selection.h"
#include "cloud_session.h"
#include "controller_settings.h"
#include "machine_link.h"
#include "wifi_setup_internal.h"

static const char *TAG = "lm_cloud_live";
static bool stomp_command_matches(const char *command, const char *expected) {
  size_t expected_len;

  if (command == NULL || expected == NULL) {
    return false;
  }

  expected_len = strlen(expected);
  return strncmp(command, expected, expected_len) == 0 &&
         (command[expected_len] == '\0' ||
          command[expected_len] == '\n' ||
          command[expected_len] == '\r');
}

static const char *websocket_event_name(int32_t event_id) {
  switch (event_id) {
    case WEBSOCKET_EVENT_ERROR:
      return "ERROR";
#if WS_TRANSPORT_HEADER_CALLBACK_SUPPORT
    case WEBSOCKET_EVENT_HEADER_RECEIVED:
      return "HEADER_RECEIVED";
#endif
    case WEBSOCKET_EVENT_CONNECTED:
      return "CONNECTED";
    case WEBSOCKET_EVENT_DISCONNECTED:
      return "DISCONNECTED";
    case WEBSOCKET_EVENT_DATA:
      return "DATA";
    case WEBSOCKET_EVENT_CLOSED:
      return "CLOSED";
    case WEBSOCKET_EVENT_BEFORE_CONNECT:
      return "BEFORE_CONNECT";
    case WEBSOCKET_EVENT_BEGIN:
      return "BEGIN";
    case WEBSOCKET_EVENT_FINISH:
      return "FINISH";
    default:
      return "UNKNOWN";
  }
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

static void set_cloud_websocket_connected(bool transport_connected, bool stomp_connected) {
  bool active = false;

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
  active = s_state.cloud_ws_task != NULL || transport_connected || stomp_connected;
  unlock_state();

  lm_ctrl_machine_link_set_live_updates_state(active, stomp_connected);
}

static void set_brew_timer_state(bool brew_active, int64_t brew_start_epoch_ms) {
  const int64_t now_us = esp_timer_get_time();
  const int64_t now_epoch_ms = current_cloud_epoch_ms();
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

static bool should_run_cloud_websocket_locked(void) {
  bool has_effective_machine_selection = false;

  if (LM_CTRL_ENABLE_CLOUD_LIVE_UPDATES == 0) {
    return false;
  }
  has_effective_machine_selection = lm_ctrl_cloud_resolve_effective_machine_selection(
    &s_state.selected_machine,
    s_state.has_machine_selection,
    s_state.fleet,
    s_state.fleet_count,
    NULL,
    NULL
  );
  return s_state.initialized &&
         s_state.sta_connected &&
         s_state.has_cloud_credentials &&
         has_effective_machine_selection;
}

static bool can_start_cloud_websocket_locked(void) {
  return should_run_cloud_websocket_locked() &&
         s_state.cloud_probe_task == NULL &&
         s_state.cloud_http_requests_in_flight == 0;
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
  char *command_end;
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
  command_end = strchr(frame, '\n');
  if (command_end == NULL || command_end > separator) {
    return false;
  }

  *command_end = '\0';
  *separator = '\0';
  *command = frame;
  *body = separator + 2;
  return true;
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

static void inspect_heat_widgets(cJSON *root, bool *coffee_present, bool *steam_present) {
  cJSON *widgets;
  cJSON *widget;

  if (coffee_present != NULL) {
    *coffee_present = false;
  }
  if (steam_present != NULL) {
    *steam_present = false;
  }
  if (root == NULL) {
    return;
  }

  widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
  if (!cJSON_IsArray(widgets)) {
    return;
  }

  cJSON_ArrayForEach(widget, widgets) {
    cJSON *code_item = cJSON_GetObjectItemCaseSensitive(widget, "code");

    if (!cJSON_IsString(code_item) || code_item->valuestring == NULL) {
      continue;
    }
    if (strcmp(code_item->valuestring, "CMCoffeeBoiler") == 0) {
      if (coffee_present != NULL) {
        *coffee_present = true;
      }
    } else if (strcmp(code_item->valuestring, "CMSteamBoilerLevel") == 0 ||
               strcmp(code_item->valuestring, "CMSteamBoilerTemperature") == 0) {
      if (steam_present != NULL) {
        *steam_present = true;
      }
    }
  }
}

static void recompute_heat_info(lm_ctrl_machine_heat_info_t *heat_info) {
  if (heat_info == NULL) {
    return;
  }

  heat_info->heating = heat_info->coffee_heating || heat_info->steam_heating;
  heat_info->eta_available = heat_info->coffee_eta_available || heat_info->steam_eta_available;
  heat_info->ready_epoch_ms = 0;
  if (heat_info->coffee_eta_available && heat_info->coffee_ready_epoch_ms > heat_info->ready_epoch_ms) {
    heat_info->ready_epoch_ms = heat_info->coffee_ready_epoch_ms;
  }
  if (heat_info->steam_eta_available && heat_info->steam_ready_epoch_ms > heat_info->ready_epoch_ms) {
    heat_info->ready_epoch_ms = heat_info->steam_ready_epoch_ms;
  }
}

static void merge_partial_heat_info(
  lm_ctrl_machine_heat_info_t *heat_info,
  bool coffee_present,
  bool steam_present
) {
  lm_ctrl_machine_heat_info_t current_heat_info = {0};

  if (heat_info == NULL || !heat_info->available) {
    return;
  }
  if (coffee_present && steam_present) {
    recompute_heat_info(heat_info);
    return;
  }

  lm_ctrl_machine_link_get_heat_info(&current_heat_info);
  if (!current_heat_info.available) {
    recompute_heat_info(heat_info);
    return;
  }

  if (!coffee_present) {
    heat_info->coffee_heating = current_heat_info.coffee_heating;
    heat_info->coffee_eta_available = current_heat_info.coffee_eta_available;
    heat_info->coffee_ready_epoch_ms = current_heat_info.coffee_ready_epoch_ms;
  }
  if (!steam_present) {
    heat_info->steam_heating = current_heat_info.steam_heating;
    heat_info->steam_eta_available = current_heat_info.steam_eta_available;
    heat_info->steam_ready_epoch_ms = current_heat_info.steam_ready_epoch_ms;
  }
  if (heat_info->observed_epoch_ms == 0) {
    heat_info->observed_epoch_ms = current_heat_info.observed_epoch_ms;
  }
  heat_info->available = true;
  recompute_heat_info(heat_info);
}

static void handle_cloud_dashboard_message(const char *message) {
  cJSON *root = NULL;
  ctrl_values_t values = {0};
  uint32_t loaded_mask = 0;
  uint32_t feature_mask = 0;
  lm_ctrl_machine_heat_info_t heat_info = {0};
  lm_ctrl_machine_water_status_t water_status = {0};
  lm_ctrl_cloud_machine_status_t machine_status = {0};
  bool brew_active = false;
  int64_t brew_start_epoch_ms = 0;
  bool coffee_heat_present = false;
  bool steam_heat_present = false;
  lm_ctrl_cloud_command_update_t updates[LM_CTRL_CLOUD_COMMAND_UPDATE_MAX] = {0};
  size_t update_count = 0;
  bool parsed_values = false;

  if (message == NULL || message[0] == '\0') {
    return;
  }

  root = cJSON_Parse(message);
  if (root == NULL) {
    ESP_LOGW(TAG, "Cloud websocket JSON parse failed");
    return;
  }

  ESP_LOGI(TAG, "Cloud websocket dashboard message received");

  update_count = parse_dashboard_command_updates(root, updates, LM_CTRL_CLOUD_COMMAND_UPDATE_MAX);
  if (lm_ctrl_cloud_parse_dashboard_machine_status(root, &machine_status)) {
    merge_cloud_machine_status(&machine_status);
  }
  inspect_heat_widgets(root, &coffee_heat_present, &steam_heat_present);

  parsed_values = lm_ctrl_cloud_parse_dashboard_root_values(
      root,
      &values,
      &loaded_mask,
      &feature_mask,
      &heat_info,
      &brew_active,
      &brew_start_epoch_ms
    ) == ESP_OK;
  (void)lm_ctrl_cloud_parse_dashboard_water_status(root, &water_status);

  if (parsed_values) {
    if (heat_info.available) {
      merge_partial_heat_info(&heat_info, coffee_heat_present, steam_heat_present);
    }
    if (heat_info.available && heat_info.observed_epoch_ms == 0) {
      heat_info.observed_epoch_ms = current_cloud_epoch_ms();
    }
    if (heat_info.available) {
      ESP_LOGI(
        TAG,
        "Cloud websocket heat update coffee_present=%d steam_present=%d heating=%d eta=%d ready=%lld observed=%lld",
        coffee_heat_present ? 1 : 0,
        steam_heat_present ? 1 : 0,
        heat_info.heating ? 1 : 0,
        heat_info.eta_available ? 1 : 0,
        (long long)heat_info.ready_epoch_ms,
        (long long)heat_info.observed_epoch_ms
      );
    }
    lm_ctrl_machine_link_apply_cloud_dashboard_values(
      &values,
      loaded_mask,
      feature_mask,
      &heat_info,
      water_status.available ? &water_status : NULL
    );
    set_brew_timer_state(brew_active, brew_start_epoch_ms);
  } else if (water_status.available) {
    lm_ctrl_machine_link_apply_cloud_dashboard_values(&values, 0, 0, NULL, &water_status);
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
  int sent_len;

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
  ESP_LOGI(TAG, "Cloud websocket transport connected; sending STOMP CONNECT len=%u", (unsigned)frame_len);
  sent_len = esp_websocket_client_send_text(client, frame, frame_len, portMAX_DELAY);
  ESP_LOGI(TAG, "Cloud websocket STOMP CONNECT send returned %d", sent_len);
}

static void send_cloud_ws_subscribe_frame(esp_websocket_client_handle_t client) {
  char destination[128];
  char serial[32];
  char frame[512];
  lm_ctrl_cloud_http_header_t headers[4];
  size_t frame_len;
  int sent_len;

  if (client == NULL) {
    return;
  }

  {
    lm_ctrl_cloud_machine_t selected_machine = {0};

    (void)lm_ctrl_settings_get_effective_selected_machine(&selected_machine);
    copy_text(serial, sizeof(serial), selected_machine.serial);
  }
  if (serial[0] == '\0') {
    return;
  }
  snprintf(destination, sizeof(destination), "%s%s%s", LM_CTRL_CLOUD_WS_DEST_PREFIX, serial, LM_CTRL_CLOUD_WS_DEST_SUFFIX);

  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "destination", .value = destination };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "ack", .value = "auto" };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "id", .value = LM_CTRL_CLOUD_WS_SUBSCRIPTION_ID };
  headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "content-length", .value = "0" };
  build_stomp_frame(frame, sizeof(frame), "SUBSCRIBE", headers, 4, NULL);
  frame_len = strnlen(frame, sizeof(frame) - 1) + 1;
  ESP_LOGI(TAG, "Cloud websocket STOMP connected; subscribing to %s len=%u", destination, (unsigned)frame_len);
  sent_len = esp_websocket_client_send_text(client, frame, frame_len, portMAX_DELAY);
  ESP_LOGI(TAG, "Cloud websocket SUBSCRIBE send returned %d", sent_len);
}

static void cloud_ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)handler_args;
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

  (void)base;
  if (data != NULL) {
    ESP_LOGI(
      TAG,
      "Cloud websocket event=%s(%ld) data_len=%d payload_len=%d offset=%d opcode=%u fin=%d",
      websocket_event_name(event_id),
      (long)event_id,
      data->data_len,
      data->payload_len,
      data->payload_offset,
      (unsigned)data->op_code,
      data->fin ? 1 : 0
    );
  } else {
    ESP_LOGI(TAG, "Cloud websocket event=%s(%ld)", websocket_event_name(event_id), (long)event_id);
  }

  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
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
    ESP_LOGW(TAG, "Cloud websocket disconnected");
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
    ESP_LOGI(TAG, "Cloud websocket raw frame: %s", frame_copy);

    if (!parse_stomp_frame(frame_copy, &command, &body) || command == NULL) {
      ESP_LOGW(TAG, "Cloud websocket frame parse failed");
      free(frame_copy);
      return;
    }
    ESP_LOGI(TAG, "Cloud websocket STOMP command=%s", command);

    if (stomp_command_matches(command, "CONNECTED")) {
      set_cloud_websocket_connected(true, true);
    } else if (stomp_command_matches(command, "ERROR")) {
      ESP_LOGW(TAG, "Cloud websocket STOMP ERROR body: %s", body != NULL ? body : "");
    } else if (stomp_command_matches(command, "MESSAGE")) {
      handle_cloud_dashboard_message(body);
    } else {
      ESP_LOGI(TAG, "Cloud websocket unhandled STOMP command=%s body=%s", command, body != NULL ? body : "");
    }

    free(frame_copy);
  }
}

static void cloud_websocket_task(void *arg) {
  char username[96];
  char password[128];
  char access_token[LM_CTRL_CLOUD_WS_TOKEN_LEN];
  char ws_headers[LM_CTRL_CLOUD_WS_HEADER_BUFFER_LEN];

  (void)arg;
  ESP_LOGI(TAG, "Cloud websocket worker starting");

  while (true) {
    esp_websocket_client_config_t ws_config = {
      .uri = LM_CTRL_CLOUD_WS_URI,
      .headers = ws_headers,
      .task_stack = 8192,
      .buffer_size = 2048,
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

    ret = lm_ctrl_cloud_session_fetch_access_token_cached(username, password, access_token, sizeof(access_token), NULL, 0);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Cloud websocket access token fetch failed: %s", esp_err_to_name(ret));
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    ret = lm_ctrl_cloud_session_build_websocket_headers(ws_headers, sizeof(ws_headers));
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Cloud websocket header signing failed: %s", esp_err_to_name(ret));
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
    ESP_LOGI(TAG, "Cloud websocket client start returned %s", esp_err_to_name(ret));
    if (ret == ESP_OK) {
      const int64_t connect_deadline_us = esp_timer_get_time() + (15LL * 1000LL * 1000LL);
      bool saw_transport_connected = false;
      bool subscribe_sent = false;

      while (true) {
        bool should_stop = false;
        bool transport_connected = false;
        bool stomp_connected = false;
        bool client_connected = false;

        lock_state();
        should_stop = s_state.cloud_ws_stop_requested || !should_run_cloud_websocket_locked();
        transport_connected = s_state.cloud_ws_transport_connected;
        stomp_connected = s_state.cloud_ws_connected;
        unlock_state();
        client_connected = esp_websocket_client_is_connected(client);
        if (transport_connected) {
          saw_transport_connected = true;
        }

        if (should_stop) {
          ESP_LOGW(
            TAG,
            "Cloud websocket worker loop exiting: should_stop=1 client_connected=%d transport_connected=%d stomp_connected=%d stop_requested=%d http_in_flight=%u",
            client_connected ? 1 : 0,
            transport_connected ? 1 : 0,
            stomp_connected ? 1 : 0,
            s_state.cloud_ws_stop_requested ? 1 : 0,
            (unsigned)s_state.cloud_http_requests_in_flight
          );
          break;
        }
        if (!client_connected) {
          if (saw_transport_connected || esp_timer_get_time() >= connect_deadline_us) {
            ESP_LOGW(
              TAG,
              "Cloud websocket worker loop exiting: should_stop=0 client_connected=0 transport_connected=%d stomp_connected=%d timeout=%d",
              transport_connected ? 1 : 0,
              stomp_connected ? 1 : 0,
              esp_timer_get_time() >= connect_deadline_us ? 1 : 0
            );
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(50));
          continue;
        }
        if (stomp_connected && !subscribe_sent) {
          send_cloud_ws_subscribe_frame(client);
          subscribe_sent = true;
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

void lm_ctrl_cloud_live_updates_stop(bool wait_for_stop) {
  esp_websocket_client_handle_t client = NULL;
  TaskHandle_t task = NULL;

  lock_state();
  s_state.cloud_ws_stop_requested = true;
  client = s_state.cloud_ws_client;
  task = s_state.cloud_ws_task;
  unlock_state();

  ESP_LOGW(
    TAG,
    "Cloud websocket stop requested wait=%d client=%p task=%p",
    wait_for_stop ? 1 : 0,
    (void *)client,
    (void *)task
  );

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

esp_err_t lm_ctrl_cloud_live_updates_ensure_task(void) {
  esp_err_t ret = ESP_OK;
  bool should_run = false;
  bool can_start = false;
  bool already_running = false;

  lock_state();
  should_run = should_run_cloud_websocket_locked();
  can_start = can_start_cloud_websocket_locked();
  already_running = s_state.cloud_ws_task != NULL;
  if (!can_start) {
    unlock_state();
    if (!should_run) {
      lm_ctrl_cloud_live_updates_stop(false);
    } else {
      ESP_LOGI(TAG, "Cloud websocket start deferred (probe/http in flight or already busy)");
    }
    return ESP_OK;
  }
  if (already_running) {
    unlock_state();
    return ESP_OK;
  }
  s_state.cloud_ws_stop_requested = false;
  unlock_state();

  ret = xTaskCreate(cloud_websocket_task, "lm_cloud_ws", LM_CTRL_CLOUD_WS_STACK_SIZE, NULL, 5, &s_state.cloud_ws_task) == pdPASS
    ? ESP_OK
    : ESP_ERR_NO_MEM;
  if (ret != ESP_OK) {
    lock_state();
    s_state.cloud_ws_task = NULL;
    unlock_state();
  } else {
    ESP_LOGI(TAG, "Cloud websocket worker scheduled");
    lm_ctrl_machine_link_set_live_updates_state(true, false);
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
    (void)lm_ctrl_cloud_session_fetch_access_token_cached(username, password, access_token, sizeof(access_token), error_text, sizeof(error_text));
  }

  lock_state();
  if (s_state.cloud_probe_task == current_task) {
    s_state.cloud_probe_task = NULL;
  }
  unlock_state();
  (void)lm_ctrl_cloud_live_updates_ensure_task();
  vTaskDelete(NULL);
}

esp_err_t lm_ctrl_cloud_live_updates_request_probe(void) {
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

  if (xTaskCreate(
        cloud_probe_task,
        "lm_cloud_probe",
        LM_CTRL_CLOUD_PROBE_STACK_SIZE,
        NULL,
        4,
        &s_state.cloud_probe_task
      ) != pdPASS) {
    s_state.cloud_probe_task = NULL;
    unlock_state();
    return ESP_ERR_NO_MEM;
  }
  unlock_state();
  return ESP_OK;
}

esp_err_t lm_ctrl_cloud_live_updates_request_start(void) {
  bool should_run = false;

  lock_state();
  should_run = should_run_cloud_websocket_locked();
  if (!should_run) {
    unlock_state();
    lm_ctrl_cloud_live_updates_stop(false);
    return ESP_ERR_INVALID_STATE;
  }
  unlock_state();

  return lm_ctrl_cloud_live_updates_ensure_task();
}

bool lm_ctrl_cloud_live_updates_active(void) {
  bool active;

  lock_state();
  active = s_state.cloud_ws_task != NULL ||
           s_state.cloud_ws_transport_connected ||
           s_state.cloud_ws_connected;
  unlock_state();

  return active;
}

bool lm_ctrl_cloud_live_updates_connected(void) {
  bool connected;

  lock_state();
  connected = s_state.cloud_ws_connected;
  unlock_state();

  return connected;
}

bool lm_ctrl_cloud_live_updates_get_shot_timer_info(lm_ctrl_shot_timer_info_t *info) {
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
    int64_t now_epoch_ms = current_cloud_epoch_ms();

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
