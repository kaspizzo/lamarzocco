#include "cloud_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "cloud_auth_internal.h"
#include "controller_settings.h"

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

static bool parse_prebrew_widget_values(cJSON *widget, float *seconds_in, float *seconds_out) {
  return lm_ctrl_cloud_parse_prebrew_widget_values(widget, seconds_in, seconds_out);
}

static bool is_heat_debug_widget_code(const char *code) {
  return code != NULL &&
         (strcmp(code, "CMMachineStatus") == 0 ||
          strcmp(code, "CMCoffeeBoiler") == 0 ||
          strcmp(code, "CMSteamBoilerLevel") == 0 ||
          strcmp(code, "CMSteamBoilerTemperature") == 0);
}

static bool dashboard_output_is_heating(cJSON *output) {
  cJSON *status_item;

  if (!cJSON_IsObject(output)) {
    return false;
  }

  status_item = cJSON_GetObjectItemCaseSensitive(output, "status");
  return cJSON_IsString(status_item) &&
         status_item->valuestring != NULL &&
         strcmp(status_item->valuestring, "HeatingUp") == 0;
}

static bool dashboard_output_has_ready_start_time(cJSON *output) {
  cJSON *ready_start_time;

  if (!cJSON_IsObject(output)) {
    return false;
  }

  ready_start_time = cJSON_GetObjectItemCaseSensitive(output, "readyStartTime");
  if (cJSON_IsNumber(ready_start_time)) {
    return ready_start_time->valuedouble > 0.0;
  }
  return cJSON_IsString(ready_start_time) &&
         ready_start_time->valuestring != NULL &&
         ready_start_time->valuestring[0] != '\0';
}

static esp_err_t parse_dashboard_root_values(
  cJSON *root,
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  lm_ctrl_machine_heat_info_t *heat_info,
  bool *brew_active,
  int64_t *brew_start_epoch_ms
) {
  return lm_ctrl_cloud_parse_dashboard_root_values(
    root,
    values,
    loaded_mask,
    feature_mask,
    heat_info,
    brew_active,
    brew_start_epoch_ms
  );
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
    return;
  }

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

static esp_err_t fetch_dashboard_root(
  cJSON **out_root,
  int64_t *server_epoch_ms,
  char *error_text,
  size_t error_text_size
) {
  char username[96];
  char password[128];
  char serial[32];
  char path[192];
  char *response_body = NULL;
  int status_code = 0;
  esp_err_t ret;
  lm_ctrl_cloud_http_header_t headers[5];
  lm_ctrl_cloud_http_response_meta_t response_meta = {0};
  lm_ctrl_cloud_request_auth_t auth = {0};
  lm_ctrl_cloud_machine_t selected_machine = {0};

  if (out_root == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_root = NULL;
  if (server_epoch_ms != NULL) {
    *server_epoch_ms = 0;
  }

  lock_state();
  copy_text(username, sizeof(username), s_state.cloud_username);
  copy_text(password, sizeof(password), s_state.cloud_password);
  unlock_state();
  (void)lm_ctrl_settings_get_effective_selected_machine(&selected_machine);
  copy_text(serial, sizeof(serial), selected_machine.serial);

  if (username[0] == '\0' || password[0] == '\0' || serial[0] == '\0') {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Dashboard unavailable: cloud setup incomplete.");
    }
    return ESP_ERR_INVALID_STATE;
  }

  ret = lm_ctrl_cloud_auth_prepare_request_auth(username, password, &auth, error_text, error_text_size);
  if (ret != ESP_OK) {
    return ret;
  }

  snprintf(path, sizeof(path), "%s/%s/dashboard", LM_CTRL_CLOUD_THINGS_PATH, serial);
  headers[0] = (lm_ctrl_cloud_http_header_t){ .name = "Authorization", .value = auth.auth_header };
  headers[1] = (lm_ctrl_cloud_http_header_t){ .name = "X-App-Installation-Id", .value = auth.installation_id };
  headers[2] = (lm_ctrl_cloud_http_header_t){ .name = "X-Timestamp", .value = auth.timestamp };
  headers[3] = (lm_ctrl_cloud_http_header_t){ .name = "X-Nonce", .value = auth.nonce };
  headers[4] = (lm_ctrl_cloud_http_header_t){ .name = "X-Request-Signature", .value = auth.signature_b64 };

  ret = lm_ctrl_cloud_auth_http_request_capture(
    LM_CTRL_CLOUD_HOST,
    path,
    LM_CTRL_CLOUD_PORT,
    HTTP_METHOD_GET,
    headers,
    sizeof(headers) / sizeof(headers[0]),
    NULL,
    12000,
    &response_body,
    &status_code,
    &response_meta
  );
  if (ret != ESP_OK) {
    clear_cloud_machine_status();
    if (error_text != NULL && error_text_size > 0 && error_text[0] == '\0') {
      snprintf(error_text, error_text_size, "Dashboard request failed.");
    }
    return ret;
  }

  if (status_code < 200 || status_code >= 300) {
    if (status_code == 401) {
      clear_cached_cloud_access_token();
    }
    clear_cloud_machine_status();
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Dashboard request failed with status %d.", status_code);
    }
    free(response_body);
    return ESP_FAIL;
  }

  *out_root = cJSON_Parse(response_body);
  free(response_body);
  if (!cJSON_IsObject(*out_root)) {
    clear_cloud_machine_status();
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Dashboard response was not valid JSON.");
    }
    cJSON_Delete(*out_root);
    *out_root = NULL;
    return ESP_FAIL;
  }

  if (server_epoch_ms != NULL) {
    *server_epoch_ms = response_meta.server_epoch_ms;
  }
  {
    lm_ctrl_cloud_machine_status_t machine_status = {0};

    if (lm_ctrl_cloud_parse_dashboard_machine_status(*out_root, &machine_status)) {
      set_cloud_machine_status(&machine_status);
    }
  }

  return ESP_OK;
}

esp_err_t lm_ctrl_cloud_session_fetch_prebrewing_values(float *seconds_in, float *seconds_out) {
  char error_text[160];
  cJSON *root = NULL;
  cJSON *widgets;
  cJSON *widget;
  esp_err_t ret;

  if (seconds_in == NULL || seconds_out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  error_text[0] = '\0';
  ret = fetch_dashboard_root(&root, NULL, error_text, sizeof(error_text));
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

esp_err_t lm_ctrl_cloud_session_fetch_dashboard_values(
  ctrl_values_t *values,
  uint32_t *loaded_mask,
  uint32_t *feature_mask,
  lm_ctrl_machine_heat_info_t *heat_info,
  lm_ctrl_machine_water_status_t *water_status
) {
  char error_text[160];
  cJSON *root = NULL;
  int64_t server_epoch_ms = 0;
  esp_err_t ret;
  bool water_status_available = false;

  if (values == NULL || loaded_mask == NULL || feature_mask == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  error_text[0] = '\0';
  ret = fetch_dashboard_root(&root, &server_epoch_ms, error_text, sizeof(error_text));
  if (ret != ESP_OK) {
    return ret;
  }

  ret = parse_dashboard_root_values(root, values, loaded_mask, feature_mask, heat_info, NULL, NULL);
  if (ret == ESP_OK && heat_info != NULL) {
    heat_info->observed_epoch_ms = server_epoch_ms;
  }
  if (water_status != NULL) {
    water_status_available = lm_ctrl_cloud_parse_dashboard_water_status(root, water_status);
  }
  cJSON_Delete(root);
  return (ret == ESP_OK || water_status_available) ? ESP_OK : ret;
}

esp_err_t lm_ctrl_cloud_session_fetch_heat_debug_json(
  char **json_text,
  char *error_text,
  size_t error_text_size
) {
  cJSON *root = NULL;
  cJSON *widgets;
  cJSON *widget;
  cJSON *response = NULL;
  cJSON *response_widgets;
  char *printed = NULL;
  bool heating = false;
  bool ready_start_time_present = false;
  int widget_count = 0;
  esp_err_t ret;
  lm_ctrl_cloud_machine_t selected_machine = {0};

  if (json_text == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *json_text = NULL;
  if (error_text != NULL && error_text_size > 0) {
    error_text[0] = '\0';
  }

  ret = fetch_dashboard_root(&root, NULL, error_text, error_text_size);
  if (ret != ESP_OK) {
    return ret;
  }

  widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
  if (!cJSON_IsArray(widgets)) {
    if (error_text != NULL && error_text_size > 0) {
      snprintf(error_text, error_text_size, "Dashboard response had no widgets.");
    }
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  response = cJSON_CreateObject();
  if (response == NULL) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }

  response_widgets = cJSON_AddArrayToObject(response, "widgets");
  if (response_widgets == NULL) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }

  (void)lm_ctrl_settings_get_effective_selected_machine(&selected_machine);
  if (cJSON_AddBoolToObject(response, "ok", true) == NULL ||
      cJSON_AddStringToObject(response, "serial", selected_machine.serial) == NULL) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }

  cJSON_ArrayForEach(widget, widgets) {
    cJSON *code_item;
    cJSON *output;
    cJSON *widget_json;
    cJSON *output_copy;

    code_item = cJSON_GetObjectItemCaseSensitive(widget, "code");
    if (!cJSON_IsString(code_item) ||
        code_item->valuestring == NULL ||
        !is_heat_debug_widget_code(code_item->valuestring)) {
      continue;
    }

    widget_json = cJSON_CreateObject();
    if (widget_json == NULL) {
      ret = ESP_ERR_NO_MEM;
      goto cleanup;
    }
    if (cJSON_AddStringToObject(widget_json, "code", code_item->valuestring) == NULL) {
      cJSON_Delete(widget_json);
      ret = ESP_ERR_NO_MEM;
      goto cleanup;
    }

    output = cJSON_GetObjectItemCaseSensitive(widget, "output");
    if (cJSON_IsObject(output)) {
      output_copy = cJSON_Duplicate(output, true);
      if (output_copy == NULL) {
        cJSON_Delete(widget_json);
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
      }
      cJSON_AddItemToObject(widget_json, "output", output_copy);
      heating = heating || dashboard_output_is_heating(output);
      ready_start_time_present = ready_start_time_present || dashboard_output_has_ready_start_time(output);
    }

    cJSON_AddItemToArray(response_widgets, widget_json);
    widget_count += 1;
  }

  if (cJSON_AddNumberToObject(response, "selectedWidgetCount", widget_count) == NULL ||
      cJSON_AddBoolToObject(response, "heating", heating) == NULL ||
      cJSON_AddBoolToObject(response, "readyStartTimePresent", ready_start_time_present) == NULL ||
      cJSON_AddBoolToObject(response, "etaUsable", heating && ready_start_time_present) == NULL) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }

  printed = cJSON_PrintUnformatted(response);
  if (printed == NULL) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }

  *json_text = printed;
  printed = NULL;
  ret = ESP_OK;

cleanup:
  if (ret != ESP_OK && error_text != NULL && error_text_size > 0 && error_text[0] == '\0') {
    snprintf(error_text, error_text_size, "Could not build cloud warmup debug JSON.");
  }
  cJSON_free(printed);
  cJSON_Delete(response);
  cJSON_Delete(root);
  return ret;
}

esp_err_t lm_ctrl_cloud_session_log_prebrew_dashboard_state(char *status_text, size_t status_text_size) {
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
  ret = fetch_dashboard_root(&root, NULL, error_text, sizeof(error_text));
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
    if (status_text != NULL && status_text_size > 0) {
      snprintf(status_text, status_text_size, "No prebrew widget found in dashboard.");
    }
    return ESP_ERR_NOT_FOUND;
  }

  if (status_text != NULL && status_text_size > 0) {
    copy_text(status_text, status_text_size, summary);
  }
  return ESP_OK;
}
