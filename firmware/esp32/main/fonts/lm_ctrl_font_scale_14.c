/*******************************************************************************
 * Size: 14 px
 * Bpp: 4
 * Opts: --no-compress --no-prefilter --bpp 4 --size 14 --font firmware/esp32/managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff -r 0xF24E --format lvgl -o firmware/esp32/main/fonts/lm_ctrl_font_scale_14.c --no-kerning --lv-include lvgl.h
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef LM_CTRL_FONT_SCALE_14
#define LM_CTRL_FONT_SCALE_14 1
#endif

#if LM_CTRL_FONT_SCALE_14

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F24E "" */
    0x0, 0x0, 0x0, 0x0, 0x31, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x29, 0x99, 0x9e, 0xff, 0x99, 0x99,
    0x50, 0x0, 0x0, 0x5f, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xa0, 0x0, 0x0, 0x2, 0x22, 0x5f, 0xff,
    0x92, 0x22, 0x10, 0x0, 0x0, 0x4a, 0x40, 0xa,
    0xfd, 0x10, 0x1a, 0x70, 0x0, 0x0, 0xee, 0xe0,
    0x2, 0xf7, 0x0, 0xae, 0xf3, 0x0, 0x7, 0xf2,
    0xf7, 0x2, 0xf7, 0x2, 0xf4, 0xcb, 0x0, 0xe,
    0x80, 0x8e, 0x2, 0xf7, 0xa, 0xc0, 0x4f, 0x30,
    0x7f, 0x10, 0x1f, 0x72, 0xf7, 0x2f, 0x40, 0xc,
    0xb0, 0xed, 0x88, 0x8d, 0xe2, 0xf7, 0x9f, 0x88,
    0x8b, 0xf2, 0xdf, 0xff, 0xff, 0xd2, 0xf7, 0x9f,
    0xff, 0xff, 0xf2, 0x2b, 0xff, 0xfa, 0x22, 0xf7,
    0x8, 0xff, 0xfd, 0x40, 0x0, 0x1, 0x0, 0x2,
    0xf7, 0x0, 0x1, 0x10, 0x0, 0x0, 0x5f, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x90, 0x0, 0x0, 0x3b,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x70, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 280, .box_w = 18, .box_h = 15, .ofs_x = 0, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 62030, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
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
const lv_font_t lm_ctrl_font_scale_14 = {
#else
lv_font_t lm_ctrl_font_scale_14 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 15,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -5,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if LM_CTRL_FONT_SCALE_14*/
