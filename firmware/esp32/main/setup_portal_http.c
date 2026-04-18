/**
 * Shared HTTP helpers for the local setup portal.
 */
#include "setup_portal_http.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static esp_err_t form_url_decode_segment(const char *src, size_t src_len, char *dst, size_t dst_size) {
  size_t out = 0;

  if (src == NULL || dst == NULL || dst_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  for (size_t i = 0; i < src_len && out + 1 < dst_size; ++i) {
    if (src[i] == '+') {
      dst[out++] = ' ';
      continue;
    }

    if (src[i] == '%' &&
        i + 2 < src_len &&
        isxdigit((unsigned char)src[i + 1]) &&
        isxdigit((unsigned char)src[i + 2])) {
      char hex[3] = {src[i + 1], src[i + 2], '\0'};
      dst[out++] = (char)strtol(hex, NULL, 16);
      i += 2;
      continue;
    }

    dst[out++] = src[i];
  }

  dst[out] = '\0';
  return ESP_OK;
}

bool lm_ctrl_setup_portal_parse_form_value(const char *body, const char *key, char *dst, size_t dst_size) {
  const size_t key_len = key != NULL ? strlen(key) : 0;
  const char *cursor = body;

  if (body == NULL || key == NULL || dst == NULL || dst_size == 0) {
    return false;
  }

  while (cursor != NULL && *cursor != '\0') {
    const char *next = strchr(cursor, '&');
    const char *equals = strchr(cursor, '=');
    size_t segment_len = next != NULL ? (size_t)(next - cursor) : strlen(cursor);

    if (equals != NULL && (size_t)(equals - cursor) == key_len && strncmp(cursor, key, key_len) == 0) {
      const char *value = equals + 1;
      size_t value_len = segment_len - key_len - 1;

      if (form_url_decode_segment(value, value_len, dst, dst_size) != ESP_OK) {
        return false;
      }
      return true;
    }

    cursor = next != NULL ? next + 1 : NULL;
  }

  dst[0] = '\0';
  return false;
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

esp_err_t lm_ctrl_setup_portal_send_json_result(httpd_req_t *req, const char *status, bool ok, const char *message) {
  char message_json[256];

  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  json_escape_text(message != NULL ? message : "", message_json, sizeof(message_json));
  httpd_resp_set_status(req, status != NULL ? status : "200 OK");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, ok ? "{\"ok\":true,\"message\":\"" : "{\"ok\":false,\"message\":\"");
  httpd_resp_sendstr_chunk(req, message_json);
  httpd_resp_sendstr_chunk(req, "\"}");
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}
