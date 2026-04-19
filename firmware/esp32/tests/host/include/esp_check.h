#pragma once

#include "esp_err.h"

#define ESP_RETURN_ON_ERROR(expr, tag, message) \
  do { \
    esp_err_t err_rc_ = (expr); \
    (void)(tag); \
    (void)(message); \
    if (err_rc_ != ESP_OK) { \
      return err_rc_; \
    } \
  } while (0)

#define ESP_GOTO_ON_ERROR(expr, goto_label, tag, message) \
  do { \
    ret = (expr); \
    (void)(tag); \
    (void)(message); \
    if (ret != ESP_OK) { \
      goto goto_label; \
    } \
  } while (0)
