#pragma once

#include "lvgl.h"

lv_img_dsc_t *lv_snapshot_take(void *obj, int color_format);
void lv_snapshot_free(lv_img_dsc_t *snapshot);
