#include "setup_portal_routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"

#include "cloud_machine_selection.h"
#include "cloud_live_updates.h"
#include "machine_link.h"
#include "setup_portal_http.h"
#include "setup_portal_presenter.h"
#include "wifi_setup_internal.h"
#include "extra/others/snapshot/lv_snapshot.h"

static const char *TAG = "lm_portal_routes";
static const char *SESSION_COOKIE_NAME = "lmctrl_session";
static const char *CSRF_HEADER_NAME = "X-CSRF-Token";
static const int LM_CTRL_SETUP_PORTAL_HTTPD_STACK_SIZE = 12288;

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

static esp_err_t base64_decode_bytes(const char *input, uint8_t *output, size_t output_size, size_t *output_len) {
  int ret;

  if (input == NULL || output == NULL || output_len == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *output_len = 0;
  ret = mbedtls_base64_decode(output, output_size, output_len, (const unsigned char *)input, strlen(input));
  return ret == 0 ? ESP_OK : ESP_FAIL;
}

static bool parse_form_value(const char *body, const char *key, char *dst, size_t dst_size) {
  return lm_ctrl_setup_portal_parse_form_value(body, key, dst, dst_size);
}

static void free_form_body(char **body, size_t *body_size) {
  if (body == NULL || *body == NULL) {
    return;
  }
  if (body_size != NULL && *body_size > 0) {
    secure_zero(*body, *body_size);
    *body_size = 0;
  }
  free(*body);
  *body = NULL;
}

static esp_err_t read_form_body_alloc(httpd_req_t *req, size_t max_body_size, char **body, size_t *body_size) {
  int received = 0;
  size_t alloc_size = 0;
  char *buffer = NULL;

  if (body != NULL) {
    *body = NULL;
  }
  if (body_size != NULL) {
    *body_size = 0;
  }
  if (req == NULL || body == NULL || max_body_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (req->content_len <= 0 || req->content_len >= (int)max_body_size) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
    return ESP_FAIL;
  }

  alloc_size = (size_t)req->content_len + 1U;
  buffer = calloc(1, alloc_size);
  if (buffer == NULL) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate form buffer");
    return ESP_ERR_NO_MEM;
  }

  while (received < req->content_len) {
    int chunk = httpd_req_recv(req, buffer + received, req->content_len - received);

    if (chunk <= 0) {
      free_form_body(&buffer, &alloc_size);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form data");
      return ESP_FAIL;
    }
    received += chunk;
  }

  buffer[received] = '\0';
  *body = buffer;
  if (body_size != NULL) {
    *body_size = alloc_size;
  }
  return ESP_OK;
}

static esp_err_t read_form_body(httpd_req_t *req, char *body, size_t body_size) {
  int received = 0;

  if (req == NULL || body == NULL || body_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (req->content_len <= 0 || req->content_len >= (int)body_size) {
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
  return ESP_OK;
}

static esp_err_t read_optional_form_body(httpd_req_t *req, char *body, size_t body_size) {
  if (req == NULL || body == NULL || body_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (req->content_len <= 0) {
    body[0] = '\0';
    return ESP_OK;
  }
  return read_form_body(req, body, body_size);
}

static void clear_web_session_locked(void) {
  s_state.web_session_token[0] = '\0';
  s_state.web_csrf_token[0] = '\0';
  s_state.web_session_valid_until_us = 0;
}

static void issue_web_session(char *session_token, size_t session_token_size, char *csrf_token, size_t csrf_token_size) {
  lock_state();
  fill_random_hex(s_state.web_session_token, sizeof(s_state.web_session_token), LM_CTRL_RANDOM_TOKEN_BYTES);
  fill_random_hex(s_state.web_csrf_token, sizeof(s_state.web_csrf_token), LM_CTRL_RANDOM_TOKEN_BYTES);
  s_state.web_session_valid_until_us = esp_timer_get_time() + LM_CTRL_WEB_SESSION_IDLE_TIMEOUT_US;
  if (session_token != NULL && session_token_size > 0) {
    copy_text(session_token, session_token_size, s_state.web_session_token);
  }
  if (csrf_token != NULL && csrf_token_size > 0) {
    copy_text(csrf_token, csrf_token_size, s_state.web_csrf_token);
  }
  unlock_state();
}

static void clear_web_session(void) {
  lock_state();
  clear_web_session_locked();
  unlock_state();
}

static void set_session_cookie(httpd_req_t *req, const char *token) {
  char cookie_header[192];

  if (req == NULL) {
    return;
  }

  if (token != NULL && token[0] != '\0') {
    snprintf(
      cookie_header,
      sizeof(cookie_header),
      "%s=%s; Path=/; HttpOnly; SameSite=Strict",
      SESSION_COOKIE_NAME,
      token
    );
  } else {
    snprintf(
      cookie_header,
      sizeof(cookie_header),
      "%s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0",
      SESSION_COOKIE_NAME
    );
  }
  httpd_resp_set_hdr(req, "Set-Cookie", cookie_header);
}

static bool copy_active_session_csrf_token(
  httpd_req_t *req,
  char *csrf_token,
  size_t csrf_token_size
) {
  char cookie_value[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  size_t cookie_value_size = sizeof(cookie_value);
  bool valid = false;
  int64_t now_us = esp_timer_get_time();

  if (req == NULL) {
    return false;
  }
  if (httpd_req_get_cookie_val(req, SESSION_COOKIE_NAME, cookie_value, &cookie_value_size) != ESP_OK) {
    return false;
  }
  if (strlen(cookie_value) != (LM_CTRL_RANDOM_TOKEN_HEX_LEN - 1U)) {
    secure_zero(cookie_value, sizeof(cookie_value));
    return false;
  }

  lock_state();
  valid = s_state.web_auth_mode == LM_CTRL_WEB_AUTH_ENABLED &&
          s_state.web_session_token[0] != '\0' &&
          s_state.web_session_valid_until_us > now_us &&
          secure_equals(
            (const uint8_t *)cookie_value,
            (const uint8_t *)s_state.web_session_token,
            LM_CTRL_RANDOM_TOKEN_HEX_LEN - 1U
          );
  if (valid) {
    s_state.web_session_valid_until_us = now_us + LM_CTRL_WEB_SESSION_IDLE_TIMEOUT_US;
    if (csrf_token != NULL && csrf_token_size > 0) {
      copy_text(csrf_token, csrf_token_size, s_state.web_csrf_token);
    }
  }
  unlock_state();
  secure_zero(cookie_value, sizeof(cookie_value));
  return valid;
}

static bool csrf_token_matches_request(httpd_req_t *req, const char *form_body, const char *expected_token) {
  char provided_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  size_t provided_size = sizeof(provided_token);

  if (expected_token == NULL || expected_token[0] == '\0') {
    return false;
  }

  if (form_body != NULL) {
    (void)parse_form_value(form_body, "csrf_token", provided_token, sizeof(provided_token));
  }
  if (provided_token[0] == '\0' &&
      httpd_req_get_hdr_value_str(req, CSRF_HEADER_NAME, provided_token, provided_size) != ESP_OK) {
    return false;
  }
  if (strlen(provided_token) != strlen(expected_token)) {
    secure_zero(provided_token, sizeof(provided_token));
    return false;
  }

  {
    const bool matches = secure_equals(
      (const uint8_t *)provided_token,
      (const uint8_t *)expected_token,
      strlen(expected_token)
    );
    secure_zero(provided_token, sizeof(provided_token));
    return matches;
  }
}

static esp_err_t send_access_setup_page(httpd_req_t *req, const char *banner) {
  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr_chunk(
    req,
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<link rel=\"icon\" href=\"data:,\">"
    "<title>Controller Access Setup</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#120d0a;color:#f4efe7;padding:24px;margin:0;}"
    "main{max-width:760px;margin:0 auto;display:grid;gap:18px;}h1{margin:0;font-size:32px;}h2{margin:0 0 8px;font-size:20px;color:#ffc685;}p{margin:0;color:#d5c1ae;line-height:1.5;}"
    ".card{background:#261a13;border:1px solid #926333;border-radius:22px;padding:18px;display:grid;gap:8px;}"
    "label{display:block;margin:10px 0 6px;color:#ffc685;font-weight:600;}input{width:100%;box-sizing:border-box;padding:12px 14px;border-radius:14px;border:1px solid #5d4128;background:#1a120d;color:#f4efe7;}"
    "button{margin-top:14px;padding:12px 18px;border:none;border-radius:14px;background:#ffc685;color:#120d0a;font-weight:700;cursor:pointer;}"
    ".secondary{background:#3a281d;color:#f4efe7;}.banner{color:#ffc685;font-weight:700;}"
    "</style></head><body><main>"
    "<h1>Home Network Access</h1>"
    "<p>The setup AP stays open as before. For the controller page inside your home network, choose whether to protect access with an admin password or leave the LAN portal open on purpose.</p>"
  );
  if (banner != NULL && banner[0] != '\0') {
    httpd_resp_sendstr_chunk(req, "<div class=\"banner\">");
    httpd_resp_sendstr_chunk(req, banner);
    httpd_resp_sendstr_chunk(req, "</div>");
  }
  httpd_resp_sendstr_chunk(
    req,
    "<form class=\"card\" method=\"post\" action=\"/access-setup\">"
    "<h2>Protect With Password</h2>"
    "<input type=\"hidden\" name=\"mode\" value=\"enabled\">"
    "<label for=\"web_password\">Admin Password</label><input id=\"web_password\" name=\"web_password\" type=\"password\" maxlength=\"63\">"
    "<label for=\"web_password_confirm\">Confirm Password</label><input id=\"web_password_confirm\" name=\"web_password_confirm\" type=\"password\" maxlength=\"63\">"
    "<p>Recommended when other laptops, phones, or IoT devices share the same network.</p>"
    "<button type=\"submit\">Set Password And Continue</button>"
    "</form>"
    "<form class=\"card\" method=\"post\" action=\"/access-setup\">"
    "<h2>Leave LAN Portal Open</h2>"
    "<input type=\"hidden\" name=\"mode\" value=\"disabled\">"
    "<p>This keeps the controller fully reconfigurable from any device in the same home network. A warning stays visible in the portal until you enable a password later.</p>"
    "<button class=\"secondary\" type=\"submit\">Continue Without Password</button>"
    "</form>"
    "</main></body></html>"
  );
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

static esp_err_t send_login_page(httpd_req_t *req, const char *banner) {
  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr_chunk(
    req,
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<link rel=\"icon\" href=\"data:,\">"
    "<title>Controller Login</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#120d0a;color:#f4efe7;padding:24px;margin:0;}"
    "main{max-width:520px;margin:0 auto;display:grid;gap:16px;}h1{margin:0;font-size:32px;}p{margin:0;color:#d5c1ae;line-height:1.5;}"
    ".card{background:#261a13;border:1px solid #926333;border-radius:22px;padding:18px;display:grid;gap:8px;}"
    "label{display:block;margin:10px 0 6px;color:#ffc685;font-weight:600;}input{width:100%;box-sizing:border-box;padding:12px 14px;border-radius:14px;border:1px solid #5d4128;background:#1a120d;color:#f4efe7;}"
    "button{margin-top:14px;padding:12px 18px;border:none;border-radius:14px;background:#ffc685;color:#120d0a;font-weight:700;cursor:pointer;}.banner{color:#ffc685;font-weight:700;}"
    "</style></head><body><main>"
    "<h1>Controller Login</h1>"
    "<p>The home-network portal is protected with an admin password. If you forgot it, clear it from the controller's on-device recovery flow.</p>"
  );
  if (banner != NULL && banner[0] != '\0') {
    httpd_resp_sendstr_chunk(req, "<div class=\"banner\">");
    httpd_resp_sendstr_chunk(req, banner);
    httpd_resp_sendstr_chunk(req, "</div>");
  }
  httpd_resp_sendstr_chunk(
    req,
    "<form class=\"card\" method=\"post\" action=\"/login\">"
    "<label for=\"password\">Admin Password</label><input id=\"password\" name=\"password\" type=\"password\" maxlength=\"63\">"
    "<button type=\"submit\">Log In</button>"
    "</form>"
    "</main></body></html>"
  );
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

static bool ensure_portal_html_access(
  httpd_req_t *req,
  const char *form_body,
  bool require_csrf,
  char *csrf_token,
  size_t csrf_token_size
) {
  bool portal_running = false;
  lm_ctrl_web_auth_mode_t auth_mode = LM_CTRL_WEB_AUTH_UNSET;

  if (csrf_token != NULL && csrf_token_size > 0) {
    csrf_token[0] = '\0';
  }

  lock_state();
  portal_running = s_state.portal_running;
  auth_mode = s_state.web_auth_mode;
  unlock_state();

  if (portal_running || auth_mode == LM_CTRL_WEB_AUTH_DISABLED) {
    return true;
  }
  if (auth_mode == LM_CTRL_WEB_AUTH_UNSET) {
    send_access_setup_page(req, NULL);
    return false;
  }
  if (!copy_active_session_csrf_token(req, csrf_token, csrf_token_size)) {
    send_login_page(req, NULL);
    return false;
  }
  if (require_csrf && !csrf_token_matches_request(req, form_body, csrf_token)) {
    send_login_page(req, "The session expired or the request token was rejected. Try again.");
    return false;
  }
  return true;
}

static bool ensure_portal_api_access(httpd_req_t *req, bool require_csrf) {
  bool portal_running = false;
  lm_ctrl_web_auth_mode_t auth_mode = LM_CTRL_WEB_AUTH_UNSET;
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};

  lock_state();
  portal_running = s_state.portal_running;
  auth_mode = s_state.web_auth_mode;
  unlock_state();

  if (portal_running || auth_mode == LM_CTRL_WEB_AUTH_DISABLED) {
    return true;
  }
  if (auth_mode == LM_CTRL_WEB_AUTH_UNSET) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Web access choice required");
    return false;
  }
  if (!copy_active_session_csrf_token(req, csrf_token, sizeof(csrf_token))) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Login required");
    return false;
  }
  if (require_csrf && !csrf_token_matches_request(req, NULL, csrf_token)) {
    secure_zero(csrf_token, sizeof(csrf_token));
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token rejected");
    return false;
  }
  secure_zero(csrf_token, sizeof(csrf_token));
  return true;
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

static esp_err_t refresh_cloud_fleet(char *banner_text, size_t banner_text_size) {
  bool selection_changed = false;
  esp_err_t ret = lm_ctrl_cloud_session_refresh_fleet(banner_text, banner_text_size, &selection_changed);

  if (ret == ESP_OK && selection_changed) {
    lm_ctrl_cloud_live_updates_stop(true);
    (void)lm_ctrl_cloud_live_updates_ensure_task();
  }
  return ret;
}

static esp_err_t save_cloud_credentials(const char *username, const char *password) {
  bool credentials_changed = false;
  esp_err_t ret = lm_ctrl_settings_save_cloud_credentials(username, password, &credentials_changed);

  if (ret == ESP_OK) {
    if (credentials_changed) {
      lm_ctrl_cloud_live_updates_stop(true);
    } else {
      (void)lm_ctrl_cloud_live_updates_ensure_task();
    }
  }

  return ret;
}

static esp_err_t save_machine_selection(const lm_ctrl_cloud_machine_t *machine) {
  esp_err_t ret = lm_ctrl_settings_save_machine_selection(machine);

  if (ret == ESP_OK) {
    lm_ctrl_cloud_live_updates_stop(true);
    (void)lm_ctrl_cloud_live_updates_ensure_task();
  }

  return ret;
}

static esp_err_t send_json_result(httpd_req_t *req, const char *status, bool ok, const char *message) {
  return lm_ctrl_setup_portal_send_json_result(req, status, ok, message);
}

static esp_err_t handle_access_setup_get(httpd_req_t *req) {
  bool portal_running = false;
  lm_ctrl_web_auth_mode_t auth_mode = LM_CTRL_WEB_AUTH_UNSET;
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};

  lock_state();
  portal_running = s_state.portal_running;
  auth_mode = s_state.web_auth_mode;
  unlock_state();

  if (portal_running || auth_mode == LM_CTRL_WEB_AUTH_DISABLED) {
    return lm_ctrl_setup_portal_send_response(req, NULL, NULL);
  }
  if (auth_mode == LM_CTRL_WEB_AUTH_ENABLED &&
      copy_active_session_csrf_token(req, csrf_token, sizeof(csrf_token))) {
    return lm_ctrl_setup_portal_send_response(req, NULL, csrf_token);
  }
  return send_access_setup_page(req, NULL);
}

static esp_err_t handle_access_setup_post(httpd_req_t *req) {
  char *body = NULL;
  size_t body_size = 0;
  char mode[16];
  char password[64];
  char password_confirm[64];
  char session_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  esp_err_t ret;

  if (read_form_body_alloc(req, 1024, &body, &body_size) != ESP_OK) {
    return ESP_FAIL;
  }

  parse_form_value(body, "mode", mode, sizeof(mode));
  parse_form_value(body, "web_password", password, sizeof(password));
  parse_form_value(body, "web_password_confirm", password_confirm, sizeof(password_confirm));

  if (strcmp(mode, "disabled") == 0) {
    ret = lm_ctrl_wifi_clear_web_admin_password();
    if (ret != ESP_OK) {
      free_form_body(&body, &body_size);
      secure_zero(password, sizeof(password));
      secure_zero(password_confirm, sizeof(password_confirm));
      return lm_ctrl_setup_portal_send_response(req, "Could not store the open LAN access choice.", NULL);
    }
    set_session_cookie(req, NULL);
    free_form_body(&body, &body_size);
    secure_zero(password, sizeof(password));
    secure_zero(password_confirm, sizeof(password_confirm));
    return lm_ctrl_setup_portal_send_response(req, "The LAN portal stays open by choice.", NULL);
  }

  if (strcmp(mode, "enabled") != 0) {
    free_form_body(&body, &body_size);
    secure_zero(password, sizeof(password));
    secure_zero(password_confirm, sizeof(password_confirm));
    return send_access_setup_page(req, "Choose how the LAN portal should be handled.");
  }
  if (password[0] == '\0' || strlen(password) < 8) {
    free_form_body(&body, &body_size);
    secure_zero(password, sizeof(password));
    secure_zero(password_confirm, sizeof(password_confirm));
    return send_access_setup_page(req, "Choose an admin password with at least 8 characters.");
  }
  if (strcmp(password, password_confirm) != 0) {
    free_form_body(&body, &body_size);
    secure_zero(password, sizeof(password));
    secure_zero(password_confirm, sizeof(password_confirm));
    return send_access_setup_page(req, "The password confirmation does not match.");
  }

  ret = lm_ctrl_wifi_save_web_admin_password(password);
  secure_zero(password, sizeof(password));
  secure_zero(password_confirm, sizeof(password_confirm));
  free_form_body(&body, &body_size);
  if (ret != ESP_OK) {
    return send_access_setup_page(req, "Could not store the admin password.");
  }

  issue_web_session(session_token, sizeof(session_token), csrf_token, sizeof(csrf_token));
  set_session_cookie(req, session_token);
  secure_zero(session_token, sizeof(session_token));
  return lm_ctrl_setup_portal_send_response(req, "LAN admin password saved.", csrf_token);
}

static esp_err_t handle_login_get(httpd_req_t *req) {
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};

  if (copy_active_session_csrf_token(req, csrf_token, sizeof(csrf_token))) {
    return lm_ctrl_setup_portal_send_response(req, NULL, csrf_token);
  }
  return send_login_page(req, NULL);
}

static esp_err_t handle_login_post(httpd_req_t *req) {
  char body[256];
  char password[64];
  char session_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};

  if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }

  parse_form_value(body, "password", password, sizeof(password));
  secure_zero(body, sizeof(body));
  if (!lm_ctrl_wifi_verify_web_admin_password(password)) {
    secure_zero(password, sizeof(password));
    return send_login_page(req, "The admin password was rejected.");
  }
  secure_zero(password, sizeof(password));

  issue_web_session(session_token, sizeof(session_token), csrf_token, sizeof(csrf_token));
  set_session_cookie(req, session_token);
  secure_zero(session_token, sizeof(session_token));
  return lm_ctrl_setup_portal_send_response(req, "Signed in.", csrf_token);
}

static esp_err_t handle_logout_get(httpd_req_t *req) {
  clear_web_session();
  set_session_cookie(req, NULL);
  return send_login_page(req, "Signed out.");
}

static esp_err_t handle_admin_password_post(httpd_req_t *req) {
  char body[512];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char mode[16];
  char password[64];
  char password_confirm[64];
  char session_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  esp_err_t ret;

  if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }

  parse_form_value(body, "mode", mode, sizeof(mode));
  parse_form_value(body, "web_password", password, sizeof(password));
  parse_form_value(body, "web_password_confirm", password_confirm, sizeof(password_confirm));

  if (strcmp(mode, "clear") == 0) {
    ret = lm_ctrl_wifi_clear_web_admin_password();
    secure_zero(body, sizeof(body));
    secure_zero(password, sizeof(password));
    secure_zero(password_confirm, sizeof(password_confirm));
    if (ret != ESP_OK) {
      return lm_ctrl_setup_portal_send_response(req, "Could not remove the LAN admin password.", NULL);
    }
    set_session_cookie(req, NULL);
    return lm_ctrl_setup_portal_send_response(req, "LAN admin password removed. The portal is now open in the home network.", NULL);
  }

  if (password[0] == '\0' || strlen(password) < 8) {
    secure_zero(body, sizeof(body));
    secure_zero(password, sizeof(password));
    secure_zero(password_confirm, sizeof(password_confirm));
    return lm_ctrl_setup_portal_send_response(req, "Choose an admin password with at least 8 characters.", csrf_token);
  }
  if (strcmp(password, password_confirm) != 0) {
    secure_zero(body, sizeof(body));
    secure_zero(password, sizeof(password));
    secure_zero(password_confirm, sizeof(password_confirm));
    return lm_ctrl_setup_portal_send_response(req, "The password confirmation does not match.", csrf_token);
  }

  ret = lm_ctrl_wifi_save_web_admin_password(password);
  secure_zero(body, sizeof(body));
  secure_zero(password, sizeof(password));
  secure_zero(password_confirm, sizeof(password_confirm));
  if (ret != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not store the LAN admin password.", csrf_token);
  }

  issue_web_session(session_token, sizeof(session_token), csrf_token, sizeof(csrf_token));
  set_session_cookie(req, session_token);
  secure_zero(session_token, sizeof(session_token));
  return lm_ctrl_setup_portal_send_response(req, "LAN admin password saved.", csrf_token);
}

static esp_err_t handle_cloud_provisioning_post(httpd_req_t *req) {
  char *body = NULL;
  size_t body_size = 0;
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char *bundle_json = NULL;
  size_t bundle_json_size = 0;
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN] = {0};
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX] = {0};
  size_t secret_len = 0;
  size_t private_key_der_len = 0;
  bool allowed = false;
  cJSON *root = NULL;
  cJSON *installation_id_item = NULL;
  cJSON *secret_item = NULL;
  cJSON *private_key_item = NULL;
  esp_err_t ret = ESP_FAIL;

  if (read_form_body_alloc(req, 3072, &body, &body_size) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    ret = ESP_OK;
    goto cleanup;
  }

  lock_state();
  allowed = s_state.portal_running || !s_state.has_cloud_provisioning;
  unlock_state();
  if (!allowed) {
    ret = lm_ctrl_setup_portal_send_response(req, "Replacing cloud provisioning is only allowed from the setup AP.", csrf_token);
    goto cleanup;
  }

  bundle_json_size = 2500U;
  bundle_json = calloc(1, bundle_json_size);
  if (bundle_json == NULL) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }

  parse_form_value(body, "bundle_json", bundle_json, bundle_json_size);
  free_form_body(&body, &body_size);
  if (bundle_json[0] == '\0') {
    ret = lm_ctrl_setup_portal_send_response(req, "Paste the provisioning bundle JSON first.", csrf_token);
    goto cleanup;
  }

  root = cJSON_Parse(bundle_json);
  free_form_body(&bundle_json, &bundle_json_size);
  if (root == NULL) {
    ret = lm_ctrl_setup_portal_send_response(req, "Provisioning bundle must be valid JSON.", csrf_token);
    goto cleanup;
  }

  installation_id_item = cJSON_GetObjectItemCaseSensitive(root, "installation_id");
  secret_item = cJSON_GetObjectItemCaseSensitive(root, "secret_b64");
  private_key_item = cJSON_GetObjectItemCaseSensitive(root, "private_key_der_b64");
  if (!cJSON_IsString(installation_id_item) ||
      installation_id_item->valuestring == NULL ||
      !cJSON_IsString(secret_item) ||
      secret_item->valuestring == NULL ||
      !cJSON_IsString(private_key_item) ||
      private_key_item->valuestring == NULL) {
    ret = lm_ctrl_setup_portal_send_response(req, "Provisioning bundle is missing required fields.", csrf_token);
    goto cleanup;
  }

  ret = base64_decode_bytes(secret_item->valuestring, secret, sizeof(secret), &secret_len);
  if (ret != ESP_OK || secret_len != LM_CTRL_CLOUD_SECRET_LEN) {
    ret = lm_ctrl_setup_portal_send_response(req, "Provisioning bundle contains an invalid cloud secret.", csrf_token);
    goto cleanup;
  }
  ret = base64_decode_bytes(private_key_item->valuestring, private_key_der, sizeof(private_key_der), &private_key_der_len);
  if (ret != ESP_OK || private_key_der_len == 0) {
    ret = lm_ctrl_setup_portal_send_response(req, "Provisioning bundle contains an invalid private key.", csrf_token);
    goto cleanup;
  }

  ret = lm_ctrl_wifi_save_cloud_provisioning(
    installation_id_item->valuestring,
    secret,
    private_key_der,
    private_key_der_len
  );
  if (ret != ESP_OK) {
    ret = lm_ctrl_setup_portal_send_response(req, "Could not store the cloud provisioning bundle.", csrf_token);
    goto cleanup;
  }

  lm_ctrl_cloud_live_updates_stop(true);
  ret = lm_ctrl_setup_portal_send_response(req, "Cloud provisioning imported.", csrf_token);

cleanup:
  if (root != NULL) {
    cJSON_Delete(root);
  }
  free_form_body(&body, &body_size);
  free_form_body(&bundle_json, &bundle_json_size);
  secure_zero(secret, sizeof(secret));
  secure_zero(private_key_der, sizeof(private_key_der));
  return ret;
}

static esp_err_t handle_root_get(httpd_req_t *req) {
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};

  if (!ensure_portal_html_access(req, NULL, false, csrf_token, sizeof(csrf_token))) {
    secure_zero(csrf_token, sizeof(csrf_token));
    return ESP_OK;
  }

  return lm_ctrl_setup_portal_send_response(req, NULL, csrf_token);
}

static esp_err_t handle_controller_post(httpd_req_t *req) {
  char body[512];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char hostname[33];
  char language_code[8];
  ctrl_language_t language = CTRL_LANGUAGE_EN;

  if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }

  parse_form_value(body, "hostname", hostname, sizeof(hostname));
  parse_form_value(body, "language", language_code, sizeof(language_code));
  if (hostname[0] == '\0') {
    copy_text(hostname, sizeof(hostname), LM_CTRL_WIFI_DEFAULT_HOSTNAME);
  }
  language = ctrl_language_from_code(language_code);
  secure_zero(body, sizeof(body));

  if (lm_ctrl_wifi_save_controller_preferences(hostname, language) != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not store controller settings.", csrf_token);
  }

  return lm_ctrl_setup_portal_send_response(req, "Controller settings saved.", csrf_token);
}

static esp_err_t handle_controller_advanced_post(httpd_req_t *req) {
  char body[256];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char error_text[128];
  lm_ctrl_setup_portal_advanced_form_t form = {0};
  ctrl_state_t current_state;

  if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }
  if (!lm_ctrl_setup_portal_parse_advanced_form(body, &form)) {
    secure_zero(body, sizeof(body));
    return lm_ctrl_setup_portal_send_response(req, "Advanced settings form is invalid.", csrf_token);
  }
  secure_zero(body, sizeof(body));

  ctrl_state_init(&current_state);
  if (ctrl_state_load(&current_state) != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not load the current controller settings.", csrf_token);
  }
  if (!lm_ctrl_setup_portal_validate_advanced_form(&form, &current_state, error_text, sizeof(error_text))) {
    return lm_ctrl_setup_portal_send_response(req, error_text, csrf_token);
  }
  if (ctrl_state_update_advanced_settings(form.preset_count, form.temperature_step_c, form.time_step_s) != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not store the advanced controller settings.", csrf_token);
  }
  return lm_ctrl_setup_portal_send_response(req, "Advanced controller settings saved.", csrf_token);
}

static esp_err_t handle_debug_screenshot_toggle_post(httpd_req_t *req) {
  char body[64];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char enabled_text[8] = {0};
  bool enabled = false;

  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (read_optional_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }

  (void)parse_form_value(body, "enabled", enabled_text, sizeof(enabled_text));
  enabled = strcmp(enabled_text, "1") == 0;
  secure_zero(body, sizeof(body));
  secure_zero(enabled_text, sizeof(enabled_text));

  if (lm_ctrl_wifi_set_debug_screenshot_enabled(enabled) != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not store the remote screenshot setting.", csrf_token);
  }
  return lm_ctrl_setup_portal_send_response(
    req,
    enabled ? "Remote screenshot enabled." : "Remote screenshot disabled.",
    csrf_token
  );
}

static esp_err_t handle_heat_display_toggle_post(httpd_req_t *req) {
  char body[64];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char enabled_text[8] = {0};
  bool enabled = false;

  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (read_optional_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }

  (void)parse_form_value(body, "enabled", enabled_text, sizeof(enabled_text));
  enabled = strcmp(enabled_text, "1") == 0;
  secure_zero(body, sizeof(body));
  secure_zero(enabled_text, sizeof(enabled_text));

  if (lm_ctrl_wifi_set_heat_display_enabled(enabled) != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not store the heating display setting.", csrf_token);
  }
  return lm_ctrl_setup_portal_send_response(
    req,
    enabled ? "Heating display enabled." : "Heating display disabled.",
    csrf_token
  );
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
  if (!ensure_portal_api_access(req, true)) {
    return ESP_OK;
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
  secure_zero(body, (size_t)received + 1U);
  secure_zero(logo_blob, LM_CTRL_CUSTOM_LOGO_BLOB_SIZE);
  free(body);
  free(logo_blob);
  if (ret != ESP_OK) {
    return send_json_result(req, "500 Internal Server Error", false, "Could not store the custom controller logo.");
  }

  return send_json_result(req, "200 OK", true, "Custom controller logo saved. The on-device header has been updated.");
}

static esp_err_t handle_controller_logo_clear_post(httpd_req_t *req) {
  char body[128];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};

  if (read_optional_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }
  secure_zero(body, sizeof(body));
  if (lm_ctrl_wifi_clear_controller_logo() != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not remove the custom controller logo.", csrf_token);
  }

  return lm_ctrl_setup_portal_send_response(req, "Custom controller logo removed. The header now uses the default text again.", csrf_token);
}

static esp_err_t handle_wifi_post(httpd_req_t *req) {
  char *body = NULL;
  size_t body_size = 0;
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char ssid[33];
  char password[65];
  char hostname[33];
  char current_ssid[33];
  char current_password[65];
  char current_hostname[33];
  ctrl_language_t language;

  if (read_form_body_alloc(req, 768, &body, &body_size) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    free_form_body(&body, &body_size);
    return ESP_OK;
  }

  parse_form_value(body, "ssid", ssid, sizeof(ssid));
  parse_form_value(body, "password", password, sizeof(password));

  if (ssid[0] == '\0') {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, "SSID is required.", csrf_token);
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
  free_form_body(&body, &body_size);

  if (lm_ctrl_wifi_store_credentials(ssid, password, hostname, language) != ESP_OK) {
    secure_zero(password, sizeof(password));
    return lm_ctrl_setup_portal_send_response(req, "Could not store Wi-Fi credentials.", csrf_token);
  }

  if (lm_ctrl_wifi_apply_station_credentials() != ESP_OK) {
    secure_zero(password, sizeof(password));
    return lm_ctrl_setup_portal_send_response(req, "Credentials saved, but station connect did not start.", csrf_token);
  }

  ESP_LOGI(TAG, "Stored Wi-Fi credentials for SSID '%s'", ssid);
  secure_zero(password, sizeof(password));
  return lm_ctrl_setup_portal_send_response(req, "Saved. The controller is now trying to join the configured Wi-Fi.", csrf_token);
}

static esp_err_t handle_reboot_post(httpd_req_t *req) {
  char body[128];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};

  if (read_optional_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }
  secure_zero(body, sizeof(body));
  if (lm_ctrl_wifi_schedule_reboot() != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not schedule a reboot.", csrf_token);
  }

  return lm_ctrl_setup_portal_send_response(req, "Controller reboot scheduled.", csrf_token);
}

static esp_err_t handle_reset_network_post(httpd_req_t *req) {
  char body[128];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};

  if (read_optional_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }
  secure_zero(body, sizeof(body));
  if (lm_ctrl_wifi_reset_network() != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not reset network settings.", csrf_token);
  }

  set_session_cookie(req, NULL);
  return lm_ctrl_setup_portal_send_response(req, "Network settings cleared. The controller will reboot into setup mode.", NULL);
}

static esp_err_t handle_factory_reset_post(httpd_req_t *req) {
  char body[128];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};

  if (read_optional_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }
  secure_zero(body, sizeof(body));
  if (lm_ctrl_wifi_factory_reset() != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not reset the controller.", csrf_token);
  }

  set_session_cookie(req, NULL);
  return lm_ctrl_setup_portal_send_response(req, "Factory reset scheduled. The controller will reboot into setup mode.", NULL);
}

static esp_err_t handle_cloud_post(httpd_req_t *req) {
  char *body = NULL;
  size_t body_size = 0;
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char cloud_username[96];
  char cloud_password[128];
  char banner[160];
  bool has_cloud_provisioning = false;

  if (read_form_body_alloc(req, 768, &body, &body_size) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    free_form_body(&body, &body_size);
    return ESP_OK;
  }

  parse_form_value(body, "cloud_username", cloud_username, sizeof(cloud_username));
  parse_form_value(body, "cloud_password", cloud_password, sizeof(cloud_password));
  lock_state();
  has_cloud_provisioning = s_state.has_cloud_provisioning;
  unlock_state();

  if (cloud_username[0] == '\0') {
    free_form_body(&body, &body_size);
    secure_zero(cloud_password, sizeof(cloud_password));
    return lm_ctrl_setup_portal_send_response(req, "Cloud e-mail is required.", csrf_token);
  }
  if (cloud_password[0] == '\0') {
    free_form_body(&body, &body_size);
    secure_zero(cloud_password, sizeof(cloud_password));
    return lm_ctrl_setup_portal_send_response(req, "Cloud password is required.", csrf_token);
  }
  if (!has_cloud_provisioning) {
    free_form_body(&body, &body_size);
    secure_zero(cloud_password, sizeof(cloud_password));
    return lm_ctrl_setup_portal_send_response(req, "Import the cloud provisioning bundle first.", csrf_token);
  }

  free_form_body(&body, &body_size);
  if (save_cloud_credentials(cloud_username, cloud_password) != ESP_OK) {
    secure_zero(cloud_password, sizeof(cloud_password));
    return lm_ctrl_setup_portal_send_response(req, "Could not store cloud credentials.", csrf_token);
  }

  ESP_LOGI(TAG, "Stored cloud credentials for '%s'", cloud_username);
  if (refresh_cloud_fleet(banner, sizeof(banner)) != ESP_OK) {
    secure_zero(cloud_password, sizeof(cloud_password));
    return lm_ctrl_setup_portal_send_response(req, banner[0] != '\0' ? banner : "Cloud account stored, but machine lookup failed.", csrf_token);
  }
  secure_zero(cloud_password, sizeof(cloud_password));
  return lm_ctrl_setup_portal_send_response(req, banner, csrf_token);
}

static esp_err_t handle_cloud_refresh_post(httpd_req_t *req) {
  char body[128];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char banner[160];

  if (read_optional_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }
  secure_zero(body, sizeof(body));
  if (refresh_cloud_fleet(banner, sizeof(banner)) != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, banner[0] != '\0' ? banner : "Machine lookup failed.", csrf_token);
  }

  return lm_ctrl_setup_portal_send_response(req, banner, csrf_token);
}

static esp_err_t handle_cloud_machine_post(httpd_req_t *req) {
  char body[512];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char machine_serial[32];
  lm_ctrl_cloud_machine_t selected_machine = {0};
  bool found = false;

  if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }

  parse_form_value(body, "machine_serial", machine_serial, sizeof(machine_serial));
  if (machine_serial[0] == '\0') {
    if (!lm_ctrl_settings_get_effective_selected_machine(&selected_machine)) {
      secure_zero(body, sizeof(body));
      return lm_ctrl_setup_portal_send_response(req, "Select a machine first.", csrf_token);
    }
    secure_zero(body, sizeof(body));
    if (save_machine_selection(&selected_machine) != ESP_OK) {
      return lm_ctrl_setup_portal_send_response(req, "Could not store the selected machine.", csrf_token);
    }
    (void)lm_ctrl_machine_link_request_sync();
    return lm_ctrl_setup_portal_send_response(req, "Machine selection saved on the controller.", csrf_token);
  }

  lock_state();
  found = lm_ctrl_cloud_find_machine_by_serial(machine_serial, s_state.fleet, s_state.fleet_count, &selected_machine);
  unlock_state();
  secure_zero(body, sizeof(body));

  if (!found) {
    char banner[160];

    if (refresh_cloud_fleet(banner, sizeof(banner)) != ESP_OK) {
      return lm_ctrl_setup_portal_send_response(req, banner[0] != '\0' ? banner : "Machine list is stale. Reload it first.", csrf_token);
    }

    lock_state();
    found = lm_ctrl_cloud_find_machine_by_serial(machine_serial, s_state.fleet, s_state.fleet_count, &selected_machine);
    unlock_state();
  }

  if (!found) {
    return lm_ctrl_setup_portal_send_response(req, "Selected machine was not found in the current cloud account.", csrf_token);
  }

  if (save_machine_selection(&selected_machine) != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not store the selected machine.", csrf_token);
  }

  (void)lm_ctrl_machine_link_request_sync();
  ESP_LOGI(TAG, "Stored machine '%s' (%s)", selected_machine.name, selected_machine.serial);
  return lm_ctrl_setup_portal_send_response(req, "Machine selection saved on the controller.", csrf_token);
}

static esp_err_t handle_bbw_post(httpd_req_t *req) {
  char body[512];
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char bbw_mode_code[24];
  char bbw_dose_1_text[24];
  char bbw_dose_2_text[24];
  char payload[128];
  char status_text[192];
  char *endptr = NULL;
  float dose_1 = 0.0f;
  float dose_2 = 0.0f;

  if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    secure_zero(body, sizeof(body));
    return ESP_OK;
  }

  parse_form_value(body, "bbw_mode", bbw_mode_code, sizeof(bbw_mode_code));
  parse_form_value(body, "bbw_dose_1", bbw_dose_1_text, sizeof(bbw_dose_1_text));
  parse_form_value(body, "bbw_dose_2", bbw_dose_2_text, sizeof(bbw_dose_2_text));
  secure_zero(body, sizeof(body));

  if (bbw_mode_code[0] == '\0' || bbw_dose_1_text[0] == '\0' || bbw_dose_2_text[0] == '\0') {
    return lm_ctrl_setup_portal_send_response(req, "Brew by weight mode and both dose targets are required.", csrf_token);
  }

  dose_1 = strtof(bbw_dose_1_text, &endptr);
  if (endptr == bbw_dose_1_text || *endptr != '\0') {
    return lm_ctrl_setup_portal_send_response(req, "Dose 1 must be a valid number.", csrf_token);
  }
  dose_2 = strtof(bbw_dose_2_text, &endptr);
  if (endptr == bbw_dose_2_text || *endptr != '\0') {
    return lm_ctrl_setup_portal_send_response(req, "Dose 2 must be a valid number.", csrf_token);
  }

  if (dose_1 < 5.0f || dose_1 > 100.0f || dose_2 < 5.0f || dose_2 > 100.0f) {
    return lm_ctrl_setup_portal_send_response(req, "Brew by weight doses must stay between 5.0 g and 100.0 g.", csrf_token);
  }

  snprintf(payload, sizeof(payload), "{\"mode\":\"%s\"}", bbw_mode_code);
  status_text[0] = '\0';
  if (lm_ctrl_wifi_execute_machine_command("CoffeeMachineBrewByWeightChangeMode", payload, NULL, status_text, sizeof(status_text)) != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, status_text[0] != '\0' ? status_text : "Could not update the brew by weight mode.", csrf_token);
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
    return lm_ctrl_setup_portal_send_response(req, status_text[0] != '\0' ? status_text : "Could not update the brew by weight doses.", csrf_token);
  }

  (void)lm_ctrl_machine_link_request_sync();
  return lm_ctrl_setup_portal_send_response(req, "Brew by weight settings saved.", csrf_token);
}

static esp_err_t handle_preset_post(httpd_req_t *req) {
  char *body = NULL;
  size_t body_size = 0;
  char csrf_token[LM_CTRL_RANDOM_TOKEN_HEX_LEN] = {0};
  char slot_text[8];
  char preset_name[CTRL_PRESET_NAME_LEN];
  char temperature_text[24];
  char infuse_text[24];
  char pause_text[24];
  char bbw_mode_code[24];
  char bbw_dose_1_text[24];
  char bbw_dose_2_text[24];
  char *endptr = NULL;
  ctrl_state_t controller_state;
  ctrl_preset_t preset = {0};
  ctrl_values_t machine_values = {0};
  uint32_t loaded_mask = 0;
  uint32_t feature_mask = 0;
  float parsed_value = 0.0f;
  int preset_index = -1;
  bool bbw_available = false;
  char banner[96];

  if (read_form_body_alloc(req, 768, &body, &body_size) != ESP_OK) {
    return ESP_FAIL;
  }
  if (!ensure_portal_html_access(req, body, true, csrf_token, sizeof(csrf_token))) {
    free_form_body(&body, &body_size);
    return ESP_OK;
  }

  parse_form_value(body, "preset_slot", slot_text, sizeof(slot_text));
  parse_form_value(body, "preset_name", preset_name, sizeof(preset_name));
  parse_form_value(body, "temperature_c", temperature_text, sizeof(temperature_text));
  parse_form_value(body, "infuse_s", infuse_text, sizeof(infuse_text));
  parse_form_value(body, "pause_s", pause_text, sizeof(pause_text));

  if (slot_text[0] == '\0') {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, "Preset slot is required.", csrf_token);
  }

  preset_index = (int)strtol(slot_text, &endptr, 10);
  if (endptr == slot_text || *endptr != '\0' || preset_index < 0 || preset_index >= CTRL_PRESET_MAX_COUNT) {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, "Preset slot is invalid.", csrf_token);
  }

  ctrl_state_init(&controller_state);
  if (ctrl_state_load(&controller_state) != ESP_OK) {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, "Could not load the controller presets.", csrf_token);
  }
  if (preset_index >= controller_state.preset_count) {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, "Preset slot is not active.", csrf_token);
  }
  preset = controller_state.presets[preset_index];

  parsed_value = strtof(temperature_text, &endptr);
  if (temperature_text[0] == '\0' || endptr == temperature_text || *endptr != '\0' || parsed_value < 80.0f || parsed_value > 103.0f) {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, "Temperature must stay between 80.0 C and 103.0 C.", csrf_token);
  }
  if (!ctrl_state_value_matches_step(parsed_value, 80.0f, 103.0f, controller_state.temperature_step_c)) {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, controller_state.temperature_step_c < 0.3f ? "Temperature must follow 0.1 C steps." : "Temperature must follow 0.5 C steps.", csrf_token);
  }
  preset.values.temperature_c = parsed_value;

  parsed_value = strtof(infuse_text, &endptr);
  if (infuse_text[0] == '\0' || endptr == infuse_text || *endptr != '\0' || parsed_value < 0.0f || parsed_value > 9.0f) {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, "Prebrewing In must stay between 0.0 s and 9.0 s.", csrf_token);
  }
  if (!ctrl_state_value_matches_step(parsed_value, 0.0f, 9.0f, controller_state.time_step_s)) {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, controller_state.time_step_s < 0.3f ? "Prebrewing In must follow 0.1 s steps." : "Prebrewing In must follow 0.5 s steps.", csrf_token);
  }
  preset.values.infuse_s = parsed_value;

  parsed_value = strtof(pause_text, &endptr);
  if (pause_text[0] == '\0' || endptr == pause_text || *endptr != '\0' || parsed_value < 0.0f || parsed_value > 9.0f) {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, "Prebrewing Out must stay between 0.0 s and 9.0 s.", csrf_token);
  }
  if (!ctrl_state_value_matches_step(parsed_value, 0.0f, 9.0f, controller_state.time_step_s)) {
    free_form_body(&body, &body_size);
    return lm_ctrl_setup_portal_send_response(req, controller_state.time_step_s < 0.3f ? "Prebrewing Out must follow 0.1 s steps." : "Prebrewing Out must follow 0.5 s steps.", csrf_token);
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
      free_form_body(&body, &body_size);
      return lm_ctrl_setup_portal_send_response(req, "BBW mode and both dose targets are required when BBW is available.", csrf_token);
    }

    preset.values.bbw_mode = ctrl_bbw_mode_from_cloud_code(bbw_mode_code);

    parsed_value = strtof(bbw_dose_1_text, &endptr);
    if (endptr == bbw_dose_1_text || *endptr != '\0' || parsed_value < 5.0f || parsed_value > 100.0f) {
      free_form_body(&body, &body_size);
      return lm_ctrl_setup_portal_send_response(req, "BBW Dose 1 must stay between 5.0 g and 100.0 g.", csrf_token);
    }
    preset.values.bbw_dose_1_g = parsed_value;

    parsed_value = strtof(bbw_dose_2_text, &endptr);
    if (endptr == bbw_dose_2_text || *endptr != '\0' || parsed_value < 5.0f || parsed_value > 100.0f) {
      free_form_body(&body, &body_size);
      return lm_ctrl_setup_portal_send_response(req, "BBW Dose 2 must stay between 5.0 g and 100.0 g.", csrf_token);
    }
    preset.values.bbw_dose_2_g = parsed_value;
  }
  free_form_body(&body, &body_size);

  if (ctrl_state_store_preset_slot(preset_index, &preset) != ESP_OK) {
    return lm_ctrl_setup_portal_send_response(req, "Could not store the preset.", csrf_token);
  }

  snprintf(banner, sizeof(banner), "Preset %d saved.", preset_index + 1);
  return lm_ctrl_setup_portal_send_response(req, banner, csrf_token);
}

static esp_err_t handle_wifi_scan_get(httpd_req_t *req) {
  wifi_scan_config_t scan_config = {0};
  wifi_ap_record_t *ap_records = NULL;
  uint16_t ap_count = 0;
  bool first = true;
  char escaped_ssid[128];
  esp_err_t ret;

  if (!ensure_portal_api_access(req, false)) {
    return ESP_OK;
  }

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

static esp_err_t handle_cloud_heat_debug_get(httpd_req_t *req) {
  char error_text[192];
  char *json_text = NULL;
  esp_err_t ret;
  esp_err_t send_ret;

  if (!ensure_portal_api_access(req, false)) {
    return ESP_OK;
  }

  error_text[0] = '\0';
  ret = lm_ctrl_cloud_session_fetch_heat_debug_json(&json_text, error_text, sizeof(error_text));
  if (ret != ESP_OK || json_text == NULL) {
    return send_json_result(
      req,
      "500 Internal Server Error",
      false,
      error_text[0] != '\0' ? error_text : "Could not fetch cloud warmup debug data."
    );
  }

  httpd_resp_set_type(req, "application/json");
  send_ret = httpd_resp_sendstr(req, json_text);
  cJSON_free(json_text);
  return send_ret;
}

static esp_err_t handle_debug_screenshot_get(httpd_req_t *req) {
  bool enabled = false;

  lock_state();
  enabled = s_state.debug_screenshot_enabled;
  unlock_state();
  if (!enabled) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Debug screenshot route is disabled");
    return ESP_OK;
  }
  if (!ensure_portal_api_access(req, false)) {
    return ESP_OK;
  }
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

esp_err_t lm_ctrl_setup_portal_start_http_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = handle_root_get,
  };
  httpd_uri_t access_setup_get_uri = {
    .uri = "/access-setup",
    .method = HTTP_GET,
    .handler = handle_access_setup_get,
  };
  httpd_uri_t access_setup_post_uri = {
    .uri = "/access-setup",
    .method = HTTP_POST,
    .handler = handle_access_setup_post,
  };
  httpd_uri_t login_get_uri = {
    .uri = "/login",
    .method = HTTP_GET,
    .handler = handle_login_get,
  };
  httpd_uri_t login_post_uri = {
    .uri = "/login",
    .method = HTTP_POST,
    .handler = handle_login_post,
  };
  httpd_uri_t logout_get_uri = {
    .uri = "/logout",
    .method = HTTP_GET,
    .handler = handle_logout_get,
  };
  httpd_uri_t controller_uri = {
    .uri = "/controller",
    .method = HTTP_POST,
    .handler = handle_controller_post,
  };
  httpd_uri_t controller_advanced_uri = {
    .uri = "/controller-advanced",
    .method = HTTP_POST,
    .handler = handle_controller_advanced_post,
  };
  httpd_uri_t debug_screenshot_toggle_uri = {
    .uri = "/debug-screenshot-toggle",
    .method = HTTP_POST,
    .handler = handle_debug_screenshot_toggle_post,
  };
  httpd_uri_t heat_display_toggle_uri = {
    .uri = "/heat-display-toggle",
    .method = HTTP_POST,
    .handler = handle_heat_display_toggle_post,
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
  httpd_uri_t cloud_provisioning_uri = {
    .uri = "/cloud-provisioning",
    .method = HTTP_POST,
    .handler = handle_cloud_provisioning_post,
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
  httpd_uri_t admin_password_uri = {
    .uri = "/admin-password",
    .method = HTTP_POST,
    .handler = handle_admin_password_post,
  };
  httpd_uri_t scan_uri = {
    .uri = "/wifi-scan",
    .method = HTTP_GET,
    .handler = handle_wifi_scan_get,
  };
  httpd_uri_t cloud_heat_debug_uri = {
    .uri = "/debug/cloud-heat.json",
    .method = HTTP_GET,
    .handler = handle_cloud_heat_debug_get,
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

  config.max_uri_handlers = 35;
  config.stack_size = LM_CTRL_SETUP_PORTAL_HTTPD_STACK_SIZE;
  config.uri_match_fn = httpd_uri_match_wildcard;
  ESP_RETURN_ON_ERROR(httpd_start(&s_state.http_server, &config), TAG, "Failed to start setup web server");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &root_uri), TAG, "Failed to register root handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &access_setup_get_uri), TAG, "Failed to register access setup GET handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &access_setup_post_uri), TAG, "Failed to register access setup POST handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &login_get_uri), TAG, "Failed to register login GET handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &login_post_uri), TAG, "Failed to register login POST handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &logout_get_uri), TAG, "Failed to register logout GET handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &controller_uri), TAG, "Failed to register controller handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &controller_advanced_uri), TAG, "Failed to register advanced controller handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &heat_display_toggle_uri), TAG, "Failed to register heat display toggle handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &debug_screenshot_toggle_uri), TAG, "Failed to register screenshot toggle handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &controller_logo_uri), TAG, "Failed to register controller logo handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &controller_logo_clear_uri), TAG, "Failed to register controller logo clear handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &wifi_uri), TAG, "Failed to register Wi-Fi handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &cloud_uri), TAG, "Failed to register cloud handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &cloud_provisioning_uri), TAG, "Failed to register cloud provisioning handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &cloud_refresh_uri), TAG, "Failed to register cloud refresh handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &cloud_machine_uri), TAG, "Failed to register cloud machine handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &bbw_uri), TAG, "Failed to register brew by weight handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &preset_uri), TAG, "Failed to register preset handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &admin_password_uri), TAG, "Failed to register admin-password handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &scan_uri), TAG, "Failed to register scan handler");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.http_server, &cloud_heat_debug_uri), TAG, "Failed to register cloud heat debug handler");
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
