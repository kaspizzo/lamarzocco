#pragma once

#include <stdint.h>

#define LV_USE_SNAPSHOT 1
#define LV_IMG_CF_TRUE_COLOR 0

typedef struct {
  uint8_t blue;
  uint8_t green;
  uint8_t red;
  uint8_t alpha;
} lv_color32_channels_t;

typedef union {
  uint32_t full;
  lv_color32_channels_t ch;
} lv_color32_t;

typedef struct {
  uint32_t full;
} lv_color_t;

typedef struct {
  int32_t w;
  int32_t h;
} lv_img_header_t;

typedef struct {
  lv_img_header_t header;
  const void *data;
} lv_img_dsc_t;

static inline uint32_t lv_color_to32(lv_color_t color) {
  return color.full;
}

void *lv_scr_act(void);
void lv_img_cache_invalidate_src(const void *src);
