#include "test_httpd.h"

#include <stdlib.h>
#include <string.h>

struct httpd_req {
  char *type;
  char *body;
  size_t body_len;
  size_t body_capacity;
};

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

httpd_req_t *test_httpd_request_create(void) {
  return calloc(1, sizeof(httpd_req_t));
}

void test_httpd_request_destroy(httpd_req_t *req) {
  if (req == NULL) {
    return;
  }

  free(req->type);
  free(req->body);
  free(req);
}

const char *test_httpd_request_body(const httpd_req_t *req) {
  return (req != NULL && req->body != NULL) ? req->body : "";
}

const char *test_httpd_request_type(const httpd_req_t *req) {
  return (req != NULL && req->type != NULL) ? req->type : "";
}

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type) {
  char *copy;

  if (req == NULL || type == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  copy = strdup(type);
  if (copy == NULL) {
    return ESP_ERR_NO_MEM;
  }

  free(req->type);
  req->type = copy;
  return ESP_OK;
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
