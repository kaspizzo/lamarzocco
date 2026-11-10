/*******************************************************************************
 * Size: 10 px
 * Bpp: 4
 * Opts: --no-compress --no-prefilter --bpp 4 --size 10 --font firmware/esp32/managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf -r 0x31-0x32 --font firmware/esp32/managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff -r 0xF534 --format lvgl --lv-font-name lm_ctrl_font_scale_mode_10 --no-kerning --lv-include lvgl.h -o firmware/esp32/main/fonts/lm_ctrl_font_scale_mode_10.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef LM_CTRL_FONT_SCALE_MODE_10
#define LM_CTRL_FONT_SCALE_MODE_10 1
#endif

#if LM_CTRL_FONT_SCALE_MODE_10

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0031 "1" */
    0xbe, 0xa0, 0x5a, 0x5, 0xa0, 0x5a, 0x5, 0xa0,
    0x5a, 0x5, 0xa0,

    /* U+0032 "2" */
    0x4c, 0xdd, 0x50, 0x42, 0x1, 0xf0, 0x0, 0x0,
    0xf0, 0x0, 0xa, 0x80, 0x0, 0xa9, 0x0, 0xb,
    0x80, 0x0, 0x8f, 0xdd, 0xd5,

    /* U+F534 "" */
    0x4, 0xbd, 0xa2, 0x4, 0xbd, 0xa1, 0x6, 0xff,
    0xff, 0xe9, 0xff, 0xff, 0xe1, 0xdf, 0x40, 0x7f,
    0xfe, 0x40, 0x9f, 0x8e, 0xf1, 0x3, 0xff, 0xc0,
    0x6, 0xf8, 0x9f, 0xec, 0xff, 0xdf, 0xec, 0xff,
    0x30, 0x9f, 0xfe, 0x50, 0x9f, 0xfe, 0x50, 0x0,
    0x1, 0x0, 0x0, 0x1, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 59, .box_w = 3, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 11, .adv_w = 92, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 32, .adv_w = 200, .box_w = 13, .box_h = 7, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x1, 0xf503
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 49, .range_length = 62724, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 3, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t lm_ctrl_font_scale_mode_10 = {
#else
lv_font_t lm_ctrl_font_scale_mode_10 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 7,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if LM_CTRL_FONT_SCALE_MODE_10*/

