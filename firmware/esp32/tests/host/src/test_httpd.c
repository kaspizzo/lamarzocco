#include "test_httpd.h"

#include <stdlib.h>
#include <string.h>

static httpd_config_t s_last_httpd_config;
static bool s_has_last_httpd_config = false;

static char *dup_text(const char *text) {
  size_t len = 0;
  char *copy = NULL;

  if (text == NULL) {
    return NULL;
  }

  len = strlen(text);
  copy = malloc(len + 1U);
  if (copy == NULL) {
    return NULL;
  }

  memcpy(copy, text, len + 1U);
  return copy;
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
  size_t len = 0;

  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  while ((len + 1U) < dst_size && src[len] != '\0') {
    ++len;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static test_httpd_header_t *find_header(test_httpd_header_t *headers, size_t header_count, const char *name) {
  if (headers == NULL || name == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < header_count; ++i) {
    if (strcmp(headers[i].name, name) == 0) {
      return &headers[i];
    }
  }
  return NULL;
}

static esp_err_t set_header_value(
  test_httpd_header_t *headers,
  size_t *header_count,
  size_t header_capacity,
  const char *name,
  const char *value
) {
  test_httpd_header_t *header = NULL;

  if (headers == NULL || header_count == NULL || name == NULL || value == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  header = find_header(headers, *header_count, name);
  if (header == NULL) {
    if (*header_count >= header_capacity) {
      return ESP_ERR_NO_MEM;
    }
    header = &headers[*header_count];
    (*header_count)++;
  }

  copy_text(header->name, sizeof(header->name), name);
  copy_text(header->value, sizeof(header->value), value);
  return ESP_OK;
}

static esp_err_t append_text(httpd_req_t *req, const char *text) {
  size_t text_len;
  size_t required;
  char *new_body;

  if (req == NULL || text == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  text_len = strlen(text);
  required = req->body_len + text_len + 1U;
  if (required > req->body_capacity) {
    size_t new_capacity = req->body_capacity == 0 ? 1024U : req->body_capacity;

    while (new_capacity < required) {
      new_capacity *= 2U;
    }

    new_body = realloc(req->body, new_capacity);
    if (new_body == NULL) {
      return ESP_ERR_NO_MEM;
    }

    req->body = new_body;
    req->body_capacity = new_capacity;
  }

  memcpy(req->body + req->body_len, text, text_len);
  req->body_len += text_len;
  req->body[req->body_len] = '\0';
  return ESP_OK;
}

static esp_err_t append_bytes(httpd_req_t *req, const char *buf, size_t buf_len) {
  size_t required;
  char *new_body;

  if (req == NULL || (buf == NULL && buf_len != 0)) {
    return ESP_ERR_INVALID_ARG;
  }

  required = req->body_len + buf_len + 1U;
  if (required > req->body_capacity) {
    size_t new_capacity = req->body_capacity == 0 ? 1024U : req->body_capacity;

    while (new_capacity < required) {
      new_capacity *= 2U;
    }

    new_body = realloc(req->body, new_capacity);
    if (new_body == NULL) {
      return ESP_ERR_NO_MEM;
    }

    req->body = new_body;
    req->body_capacity = new_capacity;
  }

  if (buf_len > 0) {
    memcpy(req->body + req->body_len, buf, buf_len);
    req->body_len += buf_len;
  }
  req->body[req->body_len] = '\0';
  return ESP_OK;
}

httpd_req_t *test_httpd_request_create(void) {
  return calloc(1, sizeof(httpd_req_t));
}

void test_httpd_request_destroy(httpd_req_t *req) {
  if (req == NULL) {
    return;
  }

  free(req->request_body);
  free(req->status);
  free(req->type);
  free(req->body);
  free(req);
}

void test_httpd_request_set_uri(httpd_req_t *req, const char *uri) {
  if (req == NULL) {
    return;
  }

  req->uri = uri;
}

esp_err_t test_httpd_request_set_body(httpd_req_t *req, const char *body) {
  char *copy = NULL;
  size_t len = 0;

  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  free(req->request_body);
  req->request_body = NULL;
  req->request_body_size = 0;
  req->request_body_offset = 0;
  req->content_len = 0;
  if (body == NULL) {
    return ESP_OK;
  }

  copy = dup_text(body);
  if (copy == NULL) {
    return ESP_ERR_NO_MEM;
  }

  len = strlen(body);
  req->request_body = copy;
  req->request_body_size = len;
  req->content_len = (int)len;
  return ESP_OK;
}

esp_err_t test_httpd_request_set_header(httpd_req_t *req, const char *name, const char *value) {
  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  return set_header_value(req->request_headers, &req->request_header_count, 16, name, value);
}

esp_err_t test_httpd_request_set_cookie(httpd_req_t *req, const char *cookie_header) {
  return test_httpd_request_set_header(req, "Cookie", cookie_header != NULL ? cookie_header : "");
}

const char *test_httpd_request_body(const httpd_req_t *req) {
  return (req != NULL && req->body != NULL) ? req->body : "";
}

const char *test_httpd_request_type(const httpd_req_t *req) {
  return (req != NULL && req->type != NULL) ? req->type : "";
}

const char *test_httpd_request_status(const httpd_req_t *req) {
  return (req != NULL && req->status != NULL) ? req->status : "";
}

const char *test_httpd_response_header(const httpd_req_t *req, const char *name) {
  test_httpd_header_t *header = NULL;

  if (req == NULL || name == NULL) {
    return "";
  }

  header = find_header((test_httpd_header_t *)req->response_headers, req->response_header_count, name);
  return header != NULL ? header->value : "";
}

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status) {
  char *copy;

  if (req == NULL || status == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  copy = dup_text(status);
  if (copy == NULL) {
    return ESP_ERR_NO_MEM;
  }

  free(req->status);
  req->status = copy;
  return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type) {
  char *copy;

  if (req == NULL || type == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  copy = dup_text(type);
  if (copy == NULL) {
    return ESP_ERR_NO_MEM;
  }

  free(req->type);
  req->type = copy;
  return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value) {
  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  return set_header_value(req->response_headers, &req->response_header_count, 16, name, value);
}

esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *str) {
  if (req == NULL || str == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  req->body_len = 0;
  return append_text(req, str);
}

esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *req, const char *str) {
  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (str == NULL) {
    return ESP_OK;
  }

  return append_text(req, str);
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *buf, ssize_t buf_len) {
  if (req == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (buf == NULL || buf_len == 0) {
    return ESP_OK;
  }

  return append_bytes(req, buf, (size_t)buf_len);
}

esp_err_t httpd_resp_send_err(httpd_req_t *req, const char *status, const char *message) {
  if (req == NULL || status == NULL || message == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  req->body_len = 0;
  (void)httpd_resp_set_status(req, status);
  (void)httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return append_text(req, message);
}

esp_err_t httpd_req_get_cookie_val(httpd_req_t *req, const char *name, char *buf, size_t *buf_len) {
  const char *cookie_header = NULL;
  size_t name_len = 0;
  const char *cursor = NULL;

  if (req == NULL || name == NULL || buf == NULL || buf_len == NULL || *buf_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  cookie_header = test_httpd_response_header(req, "Cookie");
  if (cookie_header[0] == '\0') {
    cookie_header = NULL;
    for (size_t i = 0; i < req->request_header_count; ++i) {
      if (strcmp(req->request_headers[i].name, "Cookie") == 0) {
        cookie_header = req->request_headers[i].value;
        break;
      }
    }
  }
  if (cookie_header == NULL || cookie_header[0] == '\0') {
    return ESP_ERR_NOT_FOUND;
  }

  name_len = strlen(name);
  cursor = cookie_header;
  while (*cursor != '\0') {
    while (*cursor == ' ' || *cursor == ';') {
      ++cursor;
    }
    if (strncmp(cursor, name, name_len) == 0 && cursor[name_len] == '=') {
      const char *value = cursor + name_len + 1U;
      size_t value_len = 0;

      while (value[value_len] != '\0' && value[value_len] != ';') {
        ++value_len;
      }
      if (*buf_len <= value_len) {
        return ESP_ERR_INVALID_SIZE;
      }
      memcpy(buf, value, value_len);
      buf[value_len] = '\0';
      *buf_len = value_len + 1U;
      return ESP_OK;
    }
    cursor = strchr(cursor, ';');
    if (cursor == NULL) {
      break;
    }
    ++cursor;
  }

  return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *buf, size_t buf_len) {
  test_httpd_header_t *header = NULL;

  if (req == NULL || field == NULL || buf == NULL || buf_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  header = find_header(req->request_headers, req->request_header_count, field);
  if (header == NULL) {
    return ESP_ERR_NOT_FOUND;
  }
  if (strlen(header->value) + 1U > buf_len) {
    return ESP_ERR_INVALID_SIZE;
  }

  memcpy(buf, header->value, strlen(header->value) + 1U);
  return ESP_OK;
}

int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len) {
  size_t remaining = 0;
  size_t copy_len = 0;

  if (req == NULL || buf == NULL) {
    return -1;
  }

  if (req->request_body == NULL || req->request_body_offset >= req->request_body_size) {
    return 0;
  }

  remaining = req->request_body_size - req->request_body_offset;
  copy_len = remaining < buf_len ? remaining : buf_len;
  memcpy(buf, req->request_body + req->request_body_offset, copy_len);
  req->request_body_offset += copy_len;
  return (int)copy_len;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config) {
  if (handle == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (config != NULL) {
    s_last_httpd_config = *config;
    s_has_last_httpd_config = true;
  } else {
    memset(&s_last_httpd_config, 0, sizeof(s_last_httpd_config));
    s_has_last_httpd_config = false;
  }

  *handle = (httpd_handle_t)0x1;
  return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle) {
  (void)handle;
  return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler) {
  (void)handle;
  (void)uri_handler;
  return ESP_OK;
}

bool httpd_uri_match_wildcard(const char *template_uri, const char *requested_uri, size_t requested_uri_len) {
  (void)template_uri;
  (void)requested_uri;
  (void)requested_uri_len;
  return true;
}

void test_httpd_reset_server_config(void) {
  memset(&s_last_httpd_config, 0, sizeof(s_last_httpd_config));
  s_has_last_httpd_config = false;
}

const httpd_config_t *test_httpd_last_config(void) {
  if (!s_has_last_httpd_config) {
    return NULL;
  }
  return &s_last_httpd_config;
}
