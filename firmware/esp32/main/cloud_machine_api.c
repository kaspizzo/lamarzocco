#include "cloud_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "cloud_auth_internal.h"
#include "cloud_machine_selection.h"
#include "controller_settings.h"

static const char *TAG = "lm_cloud_machine";

static esp_err_t parse_customer_fleet(
  const char *response_body,
  lm_ctrl_cloud_machine_t *machines,
  size_t max_machines,
  size_t *machine_count
) {
  return lm_ctrl_cloud_parse_customer_fleet(response_body, machines, max_machines, machine_count);
}

esp_err_t lm_ctrl_cloud_session_refresh_fleet(char *banner_text, size_t banner_text_size, bool *selection_changed) {
  char username[96];
  char password[128];
  char selected_serial[32];
  char *response_body = NULL;
  lm_ctrl_cloud_machine_t machines[LM_CTRL_CLOUD_MAX_FLEET] = {0};
  size_t machine_count = 0;
  int status_code = 0;
  esp_err_t ret;
  bool restored_selection = false;
  bool auto_selected = false;
  lm_ctrl_cloud_machine_t selected_machine = {0};
  lm_ctrl_cloud_http_header_t headers[5];
  lm_ctrl_cloud_request_auth_t auth = {0};

  if (selection_changed != NULL) {
    *selection_changed = false;
  }

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

  ret = lm_ctrl_cloud_auth_prepare_request_auth(username, password, &auth, banner_text, banner_text_size);
  if (ret != ESP_OK) {
    return ret;
  }

  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "Authorization", .value = auth.auth_header };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "X-App-Installation-Id", .value = auth.installation_id };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "X-Timestamp", .value = auth.timestamp };
  headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "X-Nonce", .value = auth.nonce };
  headers[4] = (lm_ctrl_cloud_http_header_t){ .name = "X-Request-Signature", .value = auth.signature_b64 };

  ret = lm_ctrl_cloud_auth_http_request_capture(
    LM_CTRL_CLOUD_HOST,
    LM_CTRL_CLOUD_THINGS_PATH,
    LM_CTRL_CLOUD_PORT,
    HTTP_METHOD_GET,
    headers,
    sizeof(headers) / sizeof(headers[0]),
    NULL,
    12000,
    &response_body,
    &status_code,
    NULL
  );
  if (ret != ESP_OK) {
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
  restored_selection = lm_ctrl_cloud_find_machine_by_serial(selected_serial, machines, machine_count, &selected_machine);
  if (!restored_selection) {
    restored_selection = lm_ctrl_cloud_resolve_effective_machine_selection(
      NULL,
      false,
      machines,
      machine_count,
      &selected_machine,
      &auto_selected
    );
  }
  if (restored_selection) {
    s_state.selected_machine = selected_machine;
    s_state.cloud_machine_status = selected_machine.cloud_status;
    s_state.has_machine_selection = true;
  } else if (selected_serial[0] != '\0') {
    clear_selected_machine_locked();
  }
  mark_status_dirty_locked();
  unlock_state();

  if (restored_selection) {
    ret = lm_ctrl_settings_save_machine_selection(&selected_machine);
    if (ret != ESP_OK) {
      if (banner_text != NULL && banner_text_size > 0) {
        snprintf(banner_text, banner_text_size, "Machines loaded, but saving the selection failed.");
      }
      return ret;
    }
    if (selection_changed != NULL) {
      *selection_changed = true;
    }
  }

  if (banner_text != NULL && banner_text_size > 0) {
    if (auto_selected) {
      snprintf(banner_text, banner_text_size, "Cloud account verified. One machine was found and selected automatically.");
    } else {
      snprintf(banner_text, banner_text_size, "Cloud account verified. Select your machine below.");
    }
  }

  return ESP_OK;
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

esp_err_t lm_ctrl_cloud_session_execute_machine_command(
  const char *command,
  const char *json_body,
  lm_ctrl_cloud_command_result_t *result,
  char *status_text,
  size_t status_text_size
) {
  char username[96];
  char password[128];
  char serial[32];
  char path[192];
  char *response_body = NULL;
  int status_code = 0;
  esp_err_t ret;
  lm_ctrl_cloud_http_header_t headers[6];
  lm_ctrl_cloud_request_auth_t auth = {0};
  lm_ctrl_cloud_machine_t selected_machine = {0};

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
  unlock_state();
  (void)lm_ctrl_settings_get_effective_selected_machine(&selected_machine);
  copy_text(serial, sizeof(serial), selected_machine.serial);

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

  ret = lm_ctrl_cloud_auth_prepare_request_auth(username, password, &auth, status_text, status_text_size);
  if (ret != ESP_OK) {
    return ret;
  }

  snprintf(path, sizeof(path), "%s/%s/command/%s", LM_CTRL_CLOUD_THINGS_PATH, serial, command);
  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "Content-Type", .value = "application/json" };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "Authorization", .value = auth.auth_header };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "X-App-Installation-Id", .value = auth.installation_id };
  headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "X-Timestamp", .value = auth.timestamp };
  headers[4] = (lm_ctrl_cloud_http_header_t){ .name = "X-Nonce", .value = auth.nonce };
  headers[5] = (lm_ctrl_cloud_http_header_t){ .name = "X-Request-Signature", .value = auth.signature_b64 };

  ret = lm_ctrl_cloud_auth_http_request_capture(
    LM_CTRL_CLOUD_HOST,
    path,
    LM_CTRL_CLOUD_PORT,
    HTTP_METHOD_POST,
    headers,
    sizeof(headers) / sizeof(headers[0]),
    json_body,
    12000,
    &response_body,
    &status_code,
    NULL
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
