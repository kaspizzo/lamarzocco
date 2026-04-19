#include "setup_portal_routes.h"
#include "test_httpd.h"
#include "test_support.h"

static char s_stub_web_password[64];

#include "../../../main/setup_portal_routes.c"

lm_ctrl_wifi_state_t s_state;
lv_img_dsc_t s_custom_logo_dsc;

static void reset_route_test_state(void) {
  memset(&s_state, 0, sizeof(s_state));
  memset(s_stub_web_password, 0, sizeof(s_stub_web_password));
  s_state.web_auth_mode = LM_CTRL_WEB_AUTH_UNSET;
  strcpy(s_state.hostname, LM_CTRL_WIFI_DEFAULT_HOSTNAME);
}

static void extract_csrf_token(const char *body, char *token, size_t token_size) {
  const char *start = NULL;
  const char *end = NULL;
  size_t len = 0;

  if (token == NULL || token_size == 0) {
    return;
  }

  token[0] = '\0';
  if (body == NULL) {
    return;
  }

  start = strstr(body, "value=\"");
  if (start == NULL) {
    return;
  }
  start += strlen("value=\"");
  end = strchr(start, '"');
  if (end == NULL) {
    return;
  }

  len = (size_t)(end - start);
  if (len + 1U > token_size) {
    len = token_size - 1U;
  }
  memcpy(token, start, len);
  token[len] = '\0';
}

void lock_state(void) {
}

void unlock_state(void) {
}

void mark_status_dirty_locked(void) {
}

int64_t current_epoch_ms(void) {
  return 1700000000000LL;
}

esp_err_t lm_ctrl_cloud_session_refresh_fleet(char *banner_text, size_t banner_text_size, bool *selection_changed) {
  if (banner_text != NULL && banner_text_size > 0) {
    banner_text[0] = '\0';
  }
  if (selection_changed != NULL) {
    *selection_changed = false;
  }
  return ESP_OK;
}

void lm_ctrl_cloud_live_updates_stop(bool wait_for_shutdown) {
  (void)wait_for_shutdown;
}

esp_err_t lm_ctrl_cloud_live_updates_ensure_task(void) {
  return ESP_OK;
}

esp_err_t lm_ctrl_settings_save_cloud_credentials(const char *username, const char *password, bool *credentials_changed) {
  (void)username;
  (void)password;
  if (credentials_changed != NULL) {
    *credentials_changed = true;
  }
  return ESP_OK;
}

esp_err_t lm_ctrl_settings_save_machine_selection(const lm_ctrl_cloud_machine_t *machine) {
  (void)machine;
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_save_cloud_provisioning(
  const char *installation_id,
  const uint8_t *secret,
  const uint8_t *private_key_der,
  size_t private_key_der_len
) {
  (void)installation_id;
  (void)secret;
  (void)private_key_der;
  (void)private_key_der_len;
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_save_controller_preferences(const char *hostname, ctrl_language_t language) {
  (void)language;
  copy_text(s_state.hostname, sizeof(s_state.hostname), hostname);
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_set_debug_screenshot_enabled(bool enabled) {
  s_state.debug_screenshot_enabled = enabled;
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_save_controller_logo(uint8_t schema_version, const uint8_t *logo_data, size_t logo_size) {
  (void)schema_version;
  (void)logo_data;
  (void)logo_size;
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_clear_controller_logo(void) {
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_store_credentials(const char *ssid, const char *password, const char *hostname, ctrl_language_t language) {
  (void)ssid;
  (void)password;
  (void)hostname;
  (void)language;
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_apply_station_credentials(void) {
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_schedule_reboot(void) {
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_reset_network(void) {
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_factory_reset(void) {
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_save_web_admin_password(const char *password) {
  if (password == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  copy_text(s_stub_web_password, sizeof(s_stub_web_password), password);
  s_state.web_auth_mode = LM_CTRL_WEB_AUTH_ENABLED;
  clear_web_session_locked();
  return ESP_OK;
}

esp_err_t lm_ctrl_wifi_clear_web_admin_password(void) {
  s_stub_web_password[0] = '\0';
  s_state.web_auth_mode = LM_CTRL_WEB_AUTH_DISABLED;
  clear_web_session_locked();
  return ESP_OK;
}

bool lm_ctrl_wifi_verify_web_admin_password(const char *password) {
  return password != NULL && strcmp(password, s_stub_web_password) == 0;
}

esp_err_t lm_ctrl_wifi_execute_machine_command(
  const char *command,
  const char *json_body,
  lm_ctrl_cloud_command_result_t *result,
  char *status_text,
  size_t status_text_size
) {
  (void)command;
  (void)json_body;
  (void)result;
  if (status_text != NULL && status_text_size > 0) {
    copy_text(status_text, status_text_size, "ok");
  }
  return ESP_OK;
}

void lm_ctrl_wifi_get_info(lm_ctrl_wifi_info_t *info) {
  if (info == NULL) {
    return;
  }
  memset(info, 0, sizeof(*info));
  info->web_auth_mode = s_state.web_auth_mode;
  info->debug_screenshot_enabled = s_state.debug_screenshot_enabled;
  info->portal_running = s_state.portal_running;
  copy_text(info->hostname, sizeof(info->hostname), s_state.hostname);
}

bool lm_ctrl_machine_link_get_values(ctrl_values_t *values, uint32_t *loaded_mask, uint32_t *feature_mask) {
  if (values != NULL) {
    memset(values, 0, sizeof(*values));
  }
  if (loaded_mask != NULL) {
    *loaded_mask = 0;
  }
  if (feature_mask != NULL) {
    *feature_mask = 0;
  }
  return false;
}

esp_err_t lm_ctrl_machine_link_request_sync(void) {
  return ESP_OK;
}

esp_err_t lm_ctrl_setup_portal_send_response(httpd_req_t *req, const char *banner, const char *csrf_token) {
  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr_chunk(req, "<html><body><h1>Controller Setup</h1>");
  if (banner != NULL && banner[0] != '\0') {
    httpd_resp_sendstr_chunk(req, "<div class=\"banner\">");
    httpd_resp_sendstr_chunk(req, banner);
    httpd_resp_sendstr_chunk(req, "</div>");
  }
  if (csrf_token != NULL && csrf_token[0] != '\0') {
    httpd_resp_sendstr_chunk(req, "<input type=\"hidden\" name=\"csrf_token\" value=\"");
    httpd_resp_sendstr_chunk(req, csrf_token);
    httpd_resp_sendstr_chunk(req, "\">");
  }
  httpd_resp_sendstr_chunk(req, "</body></html>");
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block) {
  (void)config;
  (void)block;
  return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number) {
  if (number != NULL) {
    *number = 0;
  }
  return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *ap_records) {
  (void)number;
  (void)ap_records;
  return ESP_OK;
}

esp_err_t esp_lv_adapter_lock(int timeout_ms) {
  (void)timeout_ms;
  return ESP_OK;
}

void esp_lv_adapter_unlock(void) {
}

void *lv_scr_act(void) {
  return NULL;
}

lv_img_dsc_t *lv_snapshot_take(void *obj, int color_format) {
  (void)obj;
  (void)color_format;
  return NULL;
}

void lv_snapshot_free(lv_img_dsc_t *snapshot) {
  (void)snapshot;
}

static int test_parse_advanced_form_reads_supported_values(void) {
  lm_ctrl_setup_portal_advanced_form_t form = {0};

  ASSERT_TRUE(
    lm_ctrl_setup_portal_parse_advanced_form(
      "preset_count=6&temperature_step_c=0.5&time_step_s=0.1&preset_reduce_confirm=1",
      &form
    )
  );
  ASSERT_EQ_INT(6, form.preset_count);
  ASSERT_FLOAT_EQ(0.5f, form.temperature_step_c, 0.0001f);
  ASSERT_FLOAT_EQ(0.1f, form.time_step_s, 0.0001f);
  ASSERT_TRUE(form.preset_reduce_confirmed);

  ASSERT_FALSE(lm_ctrl_setup_portal_parse_advanced_form("preset_count=abc&temperature_step_c=0.5&time_step_s=0.5", &form));
  ASSERT_FALSE(lm_ctrl_setup_portal_parse_advanced_form("preset_count=4&temperature_step_c=0.5", &form));
  ASSERT_TRUE(lm_ctrl_setup_portal_parse_advanced_form("preset_count=4&temperature_step_c=0.5&time_step_s=0.5", &form));
  return 0;
}

static int test_validate_advanced_form_rejects_invalid_values_and_missing_confirm(void) {
  ctrl_state_t state;
  lm_ctrl_setup_portal_advanced_form_t form = {0};
  char error_text[128];

  ctrl_state_init(&state);
  state.preset_count = 6;

  form = (lm_ctrl_setup_portal_advanced_form_t){
    .preset_count = 1,
    .temperature_step_c = 0.1f,
    .time_step_s = 0.1f,
  };
  ASSERT_FALSE(lm_ctrl_setup_portal_validate_advanced_form(&form, &state, error_text, sizeof(error_text)));
  ASSERT_CONTAINS(error_text, "Preset count");

  form = (lm_ctrl_setup_portal_advanced_form_t){
    .preset_count = 6,
    .temperature_step_c = 0.2f,
    .time_step_s = 0.1f,
  };
  ASSERT_FALSE(lm_ctrl_setup_portal_validate_advanced_form(&form, &state, error_text, sizeof(error_text)));
  ASSERT_CONTAINS(error_text, "Temperature step");

  form = (lm_ctrl_setup_portal_advanced_form_t){
    .preset_count = 4,
    .temperature_step_c = 0.5f,
    .time_step_s = 0.5f,
    .preset_reduce_confirmed = false,
  };
  ASSERT_FALSE(lm_ctrl_setup_portal_validate_advanced_form(&form, &state, error_text, sizeof(error_text)));
  ASSERT_CONTAINS(error_text, "Confirm the preset deletion");

  return 0;
}

static int test_validate_advanced_form_accepts_confirmed_reduction_and_regular_updates(void) {
  ctrl_state_t state;
  lm_ctrl_setup_portal_advanced_form_t form = {0};
  char error_text[128];

  ctrl_state_init(&state);
  state.preset_count = 4;

  form = (lm_ctrl_setup_portal_advanced_form_t){
    .preset_count = 6,
    .temperature_step_c = 0.5f,
    .time_step_s = 0.5f,
    .preset_reduce_confirmed = false,
  };
  ASSERT_TRUE(lm_ctrl_setup_portal_validate_advanced_form(&form, &state, error_text, sizeof(error_text)));
  ASSERT_STREQ("", error_text);

  state.preset_count = 6;
  form = (lm_ctrl_setup_portal_advanced_form_t){
    .preset_count = 4,
    .temperature_step_c = 0.5f,
    .time_step_s = 0.5f,
    .preset_reduce_confirmed = true,
  };
  ASSERT_TRUE(lm_ctrl_setup_portal_validate_advanced_form(&form, &state, error_text, sizeof(error_text)));
  ASSERT_STREQ("", error_text);

  return 0;
}

static int test_access_setup_post_with_password_renders_portal_and_sets_cookie(void) {
  httpd_req_t *req = test_httpd_request_create();
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};

  ASSERT_TRUE(req != NULL);
  reset_route_test_state();
  test_httpd_request_set_uri(req, "/access-setup");
  ASSERT_EQ_INT(ESP_OK, test_httpd_request_set_body(req, "mode=enabled&web_password=secret123&web_password_confirm=secret123"));

  ASSERT_EQ_INT(ESP_OK, handle_access_setup_post(req));
  ASSERT_EQ_INT(LM_CTRL_WEB_AUTH_ENABLED, s_state.web_auth_mode);
  ASSERT_STREQ("text/html; charset=utf-8", test_httpd_request_type(req));
  ASSERT_CONTAINS(test_httpd_request_body(req), "Controller Setup");
  ASSERT_CONTAINS(test_httpd_request_body(req), "LAN admin password saved.");
  ASSERT_CONTAINS(test_httpd_response_header(req, "Set-Cookie"), "lmctrl_session=");
  ASSERT_STREQ("", test_httpd_response_header(req, "Location"));
  extract_csrf_token(test_httpd_request_body(req), csrf_token, sizeof(csrf_token));
  ASSERT_TRUE(csrf_token[0] != '\0');

  test_httpd_request_destroy(req);
  return 0;
}

static int test_login_post_creates_usable_session_for_protected_post(void) {
  httpd_req_t *login_req = test_httpd_request_create();
  httpd_req_t *toggle_req = test_httpd_request_create();
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char body[128];

  ASSERT_TRUE(login_req != NULL);
  ASSERT_TRUE(toggle_req != NULL);
  reset_route_test_state();
  copy_text(s_stub_web_password, sizeof(s_stub_web_password), "secret123");
  s_state.web_auth_mode = LM_CTRL_WEB_AUTH_ENABLED;
  copy_text(s_state.hostname, sizeof(s_state.hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);

  test_httpd_request_set_uri(login_req, "/login");
  ASSERT_EQ_INT(ESP_OK, test_httpd_request_set_body(login_req, "password=secret123"));
  ASSERT_EQ_INT(ESP_OK, handle_login_post(login_req));
  ASSERT_STREQ("text/html; charset=utf-8", test_httpd_request_type(login_req));
  ASSERT_CONTAINS(test_httpd_request_body(login_req), "Signed in.");
  ASSERT_CONTAINS(test_httpd_response_header(login_req, "Set-Cookie"), "lmctrl_session=");
  ASSERT_STREQ("", test_httpd_response_header(login_req, "Location"));

  extract_csrf_token(test_httpd_request_body(login_req), csrf_token, sizeof(csrf_token));
  ASSERT_TRUE(csrf_token[0] != '\0');

  test_httpd_request_set_uri(toggle_req, "/debug-screenshot-toggle");
  ASSERT_EQ_INT(ESP_OK, test_httpd_request_set_cookie(toggle_req, test_httpd_response_header(login_req, "Set-Cookie")));
  snprintf(body, sizeof(body), "enabled=1&csrf_token=%s", csrf_token);
  ASSERT_EQ_INT(ESP_OK, test_httpd_request_set_body(toggle_req, body));

  ASSERT_EQ_INT(ESP_OK, handle_debug_screenshot_toggle_post(toggle_req));
  ASSERT_TRUE(s_state.debug_screenshot_enabled);
  ASSERT_STREQ("text/html; charset=utf-8", test_httpd_request_type(toggle_req));
  ASSERT_CONTAINS(test_httpd_request_body(toggle_req), "Remote screenshot enabled.");

  test_httpd_request_destroy(login_req);
  test_httpd_request_destroy(toggle_req);
  return 0;
}

static int test_clearing_password_returns_portal_to_open_mode(void) {
  httpd_req_t *login_req = test_httpd_request_create();
  httpd_req_t *clear_req = test_httpd_request_create();
  httpd_req_t *root_req = test_httpd_request_create();
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char body[128];

  ASSERT_TRUE(login_req != NULL);
  ASSERT_TRUE(clear_req != NULL);
  ASSERT_TRUE(root_req != NULL);
  reset_route_test_state();
  copy_text(s_stub_web_password, sizeof(s_stub_web_password), "secret123");
  s_state.web_auth_mode = LM_CTRL_WEB_AUTH_ENABLED;
  copy_text(s_state.hostname, sizeof(s_state.hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);

  test_httpd_request_set_uri(login_req, "/login");
  ASSERT_EQ_INT(ESP_OK, test_httpd_request_set_body(login_req, "password=secret123"));
  ASSERT_EQ_INT(ESP_OK, handle_login_post(login_req));
  extract_csrf_token(test_httpd_request_body(login_req), csrf_token, sizeof(csrf_token));
  ASSERT_TRUE(csrf_token[0] != '\0');

  test_httpd_request_set_uri(clear_req, "/admin-password");
  ASSERT_EQ_INT(ESP_OK, test_httpd_request_set_cookie(clear_req, test_httpd_response_header(login_req, "Set-Cookie")));
  snprintf(body, sizeof(body), "mode=clear&csrf_token=%s", csrf_token);
  ASSERT_EQ_INT(ESP_OK, test_httpd_request_set_body(clear_req, body));
  ASSERT_EQ_INT(ESP_OK, handle_admin_password_post(clear_req));
  ASSERT_EQ_INT(LM_CTRL_WEB_AUTH_DISABLED, s_state.web_auth_mode);
  ASSERT_CONTAINS(test_httpd_request_body(clear_req), "LAN admin password removed.");
  ASSERT_CONTAINS(test_httpd_response_header(clear_req, "Set-Cookie"), "Max-Age=0");

  test_httpd_request_set_uri(root_req, "/");
  ASSERT_EQ_INT(ESP_OK, handle_root_get(root_req));
  ASSERT_CONTAINS(test_httpd_request_body(root_req), "Controller Setup");

  test_httpd_request_destroy(login_req);
  test_httpd_request_destroy(clear_req);
  test_httpd_request_destroy(root_req);
  return 0;
}

int run_setup_portal_route_tests(void) {
  RUN_TEST(test_parse_advanced_form_reads_supported_values);
  RUN_TEST(test_validate_advanced_form_rejects_invalid_values_and_missing_confirm);
  RUN_TEST(test_validate_advanced_form_accepts_confirmed_reduction_and_regular_updates);
  RUN_TEST(test_access_setup_post_with_password_renders_portal_and_sets_cookie);
  RUN_TEST(test_login_post_creates_usable_session_for_protected_post);
  RUN_TEST(test_clearing_password_returns_portal_to_open_mode);
  return 0;
}
