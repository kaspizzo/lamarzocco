#include "board_display.h"

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"

#include "board_config.h"

static const char *TAG = "lm_display";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_disp_t *s_display = NULL;

static const st77916_lcd_init_cmd_t s_init_cmds[] = {
  {0xF0, (uint8_t[]){0x28}, 1, 0},
  {0xF2, (uint8_t[]){0x28}, 1, 0},
  {0x73, (uint8_t[]){0xF0}, 1, 0},
  {0x7C, (uint8_t[]){0xD1}, 1, 0},
  {0x83, (uint8_t[]){0xE0}, 1, 0},
  {0x84, (uint8_t[]){0x61}, 1, 0},
  {0xF2, (uint8_t[]){0x82}, 1, 0},
  {0xF0, (uint8_t[]){0x00}, 1, 0},
  {0xF0, (uint8_t[]){0x01}, 1, 0},
  {0xF1, (uint8_t[]){0x01}, 1, 0},
  {0xB0, (uint8_t[]){0x56}, 1, 0},
  {0xB1, (uint8_t[]){0x4D}, 1, 0},
  {0xB2, (uint8_t[]){0x24}, 1, 0},
  {0xB4, (uint8_t[]){0x87}, 1, 0},
  {0xB5, (uint8_t[]){0x44}, 1, 0},
  {0xB6, (uint8_t[]){0x8B}, 1, 0},
  {0xB7, (uint8_t[]){0x40}, 1, 0},
  {0xB8, (uint8_t[]){0x86}, 1, 0},
  {0xBA, (uint8_t[]){0x00}, 1, 0},
  {0xBB, (uint8_t[]){0x08}, 1, 0},
  {0xBC, (uint8_t[]){0x08}, 1, 0},
  {0xBD, (uint8_t[]){0x00}, 1, 0},
  {0xC0, (uint8_t[]){0x80}, 1, 0},
  {0xC1, (uint8_t[]){0x10}, 1, 0},
  {0xC2, (uint8_t[]){0x37}, 1, 0},
  {0xC3, (uint8_t[]){0x80}, 1, 0},
  {0xC4, (uint8_t[]){0x10}, 1, 0},
  {0xC5, (uint8_t[]){0x37}, 1, 0},
  {0xC6, (uint8_t[]){0xA9}, 1, 0},
  {0xC7, (uint8_t[]){0x41}, 1, 0},
  {0xC8, (uint8_t[]){0x01}, 1, 0},
  {0xC9, (uint8_t[]){0xA9}, 1, 0},
  {0xCA, (uint8_t[]){0x41}, 1, 0},
  {0xCB, (uint8_t[]){0x01}, 1, 0},
  {0xD0, (uint8_t[]){0x91}, 1, 0},
  {0xD1, (uint8_t[]){0x68}, 1, 0},
  {0xD2, (uint8_t[]){0x68}, 1, 0},
  {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
  {0xDD, (uint8_t[]){0x4F}, 1, 0},
  {0xDE, (uint8_t[]){0x4F}, 1, 0},
  {0xF1, (uint8_t[]){0x10}, 1, 0},
  {0xF0, (uint8_t[]){0x00}, 1, 0},
  {0xF0, (uint8_t[]){0x02}, 1, 0},
  {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
  {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
  {0xF0, (uint8_t[]){0x10}, 1, 0},
  {0xF3, (uint8_t[]){0x10}, 1, 0},
  {0xE0, (uint8_t[]){0x07}, 1, 0},
  {0xE1, (uint8_t[]){0x00}, 1, 0},
  {0xE2, (uint8_t[]){0x00}, 1, 0},
  {0xE3, (uint8_t[]){0x00}, 1, 0},
  {0xE4, (uint8_t[]){0xE0}, 1, 0},
  {0xE5, (uint8_t[]){0x06}, 1, 0},
  {0xE6, (uint8_t[]){0x21}, 1, 0},
  {0xE7, (uint8_t[]){0x01}, 1, 0},
  {0xE8, (uint8_t[]){0x05}, 1, 0},
  {0xE9, (uint8_t[]){0x02}, 1, 0},
  {0xEA, (uint8_t[]){0xDA}, 1, 0},
  {0xEB, (uint8_t[]){0x00}, 1, 0},
  {0xEC, (uint8_t[]){0x00}, 1, 0},
  {0xED, (uint8_t[]){0x0F}, 1, 0},
  {0xEE, (uint8_t[]){0x00}, 1, 0},
  {0xEF, (uint8_t[]){0x00}, 1, 0},
  {0xF8, (uint8_t[]){0x00}, 1, 0},
  {0xF9, (uint8_t[]){0x00}, 1, 0},
  {0xFA, (uint8_t[]){0x00}, 1, 0},
  {0xFB, (uint8_t[]){0x00}, 1, 0},
  {0xFC, (uint8_t[]){0x00}, 1, 0},
  {0xFD, (uint8_t[]){0x00}, 1, 0},
  {0xFE, (uint8_t[]){0x00}, 1, 0},
  {0xFF, (uint8_t[]){0x00}, 1, 0},
  {0x60, (uint8_t[]){0x40}, 1, 0},
  {0x61, (uint8_t[]){0x04}, 1, 0},
  {0x62, (uint8_t[]){0x00}, 1, 0},
  {0x63, (uint8_t[]){0x42}, 1, 0},
  {0x64, (uint8_t[]){0xD9}, 1, 0},
  {0x65, (uint8_t[]){0x00}, 1, 0},
  {0x66, (uint8_t[]){0x00}, 1, 0},
  {0x67, (uint8_t[]){0x00}, 1, 0},
  {0x68, (uint8_t[]){0x00}, 1, 0},
  {0x69, (uint8_t[]){0x00}, 1, 0},
  {0x6A, (uint8_t[]){0x00}, 1, 0},
  {0x6B, (uint8_t[]){0x00}, 1, 0},
  {0x70, (uint8_t[]){0x40}, 1, 0},
  {0x71, (uint8_t[]){0x03}, 1, 0},
  {0x72, (uint8_t[]){0x00}, 1, 0},
  {0x73, (uint8_t[]){0x42}, 1, 0},
  {0x74, (uint8_t[]){0xD8}, 1, 0},
  {0x75, (uint8_t[]){0x00}, 1, 0},
  {0x76, (uint8_t[]){0x00}, 1, 0},
  {0x77, (uint8_t[]){0x00}, 1, 0},
  {0x78, (uint8_t[]){0x00}, 1, 0},
  {0x79, (uint8_t[]){0x00}, 1, 0},
  {0x7A, (uint8_t[]){0x00}, 1, 0},
  {0x7B, (uint8_t[]){0x00}, 1, 0},
  {0x80, (uint8_t[]){0x48}, 1, 0},
  {0x81, (uint8_t[]){0x00}, 1, 0},
  {0x82, (uint8_t[]){0x06}, 1, 0},
  {0x83, (uint8_t[]){0x02}, 1, 0},
  {0x84, (uint8_t[]){0xD6}, 1, 0},
  {0x85, (uint8_t[]){0x04}, 1, 0},
  {0x86, (uint8_t[]){0x00}, 1, 0},
  {0x87, (uint8_t[]){0x00}, 1, 0},
  {0x88, (uint8_t[]){0x48}, 1, 0},
  {0x89, (uint8_t[]){0x00}, 1, 0},
  {0x8A, (uint8_t[]){0x08}, 1, 0},
  {0x8B, (uint8_t[]){0x02}, 1, 0},
  {0x8C, (uint8_t[]){0xD8}, 1, 0},
  {0x8D, (uint8_t[]){0x04}, 1, 0},
  {0x8E, (uint8_t[]){0x00}, 1, 0},
  {0x8F, (uint8_t[]){0x00}, 1, 0},
  {0x90, (uint8_t[]){0x48}, 1, 0},
  {0x91, (uint8_t[]){0x00}, 1, 0},
  {0x92, (uint8_t[]){0x0A}, 1, 0},
  {0x93, (uint8_t[]){0x02}, 1, 0},
  {0x94, (uint8_t[]){0xDA}, 1, 0},
  {0x95, (uint8_t[]){0x04}, 1, 0},
  {0x96, (uint8_t[]){0x00}, 1, 0},
  {0x97, (uint8_t[]){0x00}, 1, 0},
  {0x98, (uint8_t[]){0x48}, 1, 0},
  {0x99, (uint8_t[]){0x00}, 1, 0},
  {0x9A, (uint8_t[]){0x0C}, 1, 0},
  {0x9B, (uint8_t[]){0x02}, 1, 0},
  {0x9C, (uint8_t[]){0xDC}, 1, 0},
  {0x9D, (uint8_t[]){0x04}, 1, 0},
  {0x9E, (uint8_t[]){0x00}, 1, 0},
  {0x9F, (uint8_t[]){0x00}, 1, 0},
  {0xA0, (uint8_t[]){0x48}, 1, 0},
  {0xA1, (uint8_t[]){0x00}, 1, 0},
  {0xA2, (uint8_t[]){0x05}, 1, 0},
  {0xA3, (uint8_t[]){0x02}, 1, 0},
  {0xA4, (uint8_t[]){0xD5}, 1, 0},
  {0xA5, (uint8_t[]){0x04}, 1, 0},
  {0xA6, (uint8_t[]){0x00}, 1, 0},
  {0xA7, (uint8_t[]){0x00}, 1, 0},
  {0xA8, (uint8_t[]){0x48}, 1, 0},
  {0xA9, (uint8_t[]){0x00}, 1, 0},
  {0xAA, (uint8_t[]){0x07}, 1, 0},
  {0xAB, (uint8_t[]){0x02}, 1, 0},
  {0xAC, (uint8_t[]){0xD7}, 1, 0},
  {0xAD, (uint8_t[]){0x04}, 1, 0},
  {0xAE, (uint8_t[]){0x00}, 1, 0},
  {0xAF, (uint8_t[]){0x00}, 1, 0},
  {0xB0, (uint8_t[]){0x48}, 1, 0},
  {0xB1, (uint8_t[]){0x00}, 1, 0},
  {0xB2, (uint8_t[]){0x09}, 1, 0},
  {0xB3, (uint8_t[]){0x02}, 1, 0},
  {0xB4, (uint8_t[]){0xD9}, 1, 0},
  {0xB5, (uint8_t[]){0x04}, 1, 0},
  {0xB6, (uint8_t[]){0x00}, 1, 0},
  {0xB7, (uint8_t[]){0x00}, 1, 0},
  {0xB8, (uint8_t[]){0x48}, 1, 0},
  {0xB9, (uint8_t[]){0x00}, 1, 0},
  {0xBA, (uint8_t[]){0x0B}, 1, 0},
  {0xBB, (uint8_t[]){0x02}, 1, 0},
  {0xBC, (uint8_t[]){0xDB}, 1, 0},
  {0xBD, (uint8_t[]){0x04}, 1, 0},
  {0xBE, (uint8_t[]){0x00}, 1, 0},
  {0xBF, (uint8_t[]){0x00}, 1, 0},
  {0xC0, (uint8_t[]){0x10}, 1, 0},
  {0xC1, (uint8_t[]){0x47}, 1, 0},
  {0xC2, (uint8_t[]){0x56}, 1, 0},
  {0xC3, (uint8_t[]){0x65}, 1, 0},
  {0xC4, (uint8_t[]){0x74}, 1, 0},
  {0xC5, (uint8_t[]){0x88}, 1, 0},
  {0xC6, (uint8_t[]){0x99}, 1, 0},
  {0xC7, (uint8_t[]){0x01}, 1, 0},
  {0xC8, (uint8_t[]){0xBB}, 1, 0},
  {0xC9, (uint8_t[]){0xAA}, 1, 0},
  {0xD0, (uint8_t[]){0x10}, 1, 0},
  {0xD1, (uint8_t[]){0x47}, 1, 0},
  {0xD2, (uint8_t[]){0x56}, 1, 0},
  {0xD3, (uint8_t[]){0x65}, 1, 0},
  {0xD4, (uint8_t[]){0x74}, 1, 0},
  {0xD5, (uint8_t[]){0x88}, 1, 0},
  {0xD6, (uint8_t[]){0x99}, 1, 0},
  {0xD7, (uint8_t[]){0x01}, 1, 0},
  {0xD8, (uint8_t[]){0xBB}, 1, 0},
  {0xD9, (uint8_t[]){0xAA}, 1, 0},
  {0xF3, (uint8_t[]){0x01}, 1, 0},
  {0xF0, (uint8_t[]){0x00}, 1, 0},
  {0x21, (uint8_t[]){0x00}, 1, 0},
  {0x11, (uint8_t[]){0x00}, 1, 120},
  {0x29, (uint8_t[]){0x00}, 1, 0},
};

esp_err_t lm_ctrl_display_init(lv_disp_t **out_display) {
  ESP_LOGI(TAG, "Initialize LCD SPI bus");
  spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
    LM_CTRL_LCD_SCL,
    LM_CTRL_LCD_D0,
    LM_CTRL_LCD_D1,
    LM_CTRL_LCD_D2,
    LM_CTRL_LCD_D3,
    LM_CTRL_LCD_SPI_MAX_TRANSFER_SZ
  );
  ESP_RETURN_ON_ERROR(spi_bus_initialize(LM_CTRL_LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init failed");

  esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(LM_CTRL_LCD_CS, NULL, NULL);
  io_config.trans_queue_depth = 1;
  ESP_RETURN_ON_ERROR(
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LM_CTRL_LCD_HOST, &io_config, &s_panel_io),
    TAG,
    "Panel IO init failed"
  );

  st77916_vendor_config_t vendor_config = {
    .init_cmds = s_init_cmds,
    .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
    .flags = {
      .use_qspi_interface = 1,
    },
  };

  esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = LM_CTRL_LCD_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = LM_CTRL_LCD_BPP,
    .vendor_config = &vendor_config,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(s_panel_io, &panel_config, &s_panel), TAG, "Panel driver init failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "Panel reset failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Panel init sequence failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, false), TAG, "Panel mirror failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "Panel enable failed");

  i2c_master_bus_config_t i2c_config = {
    .i2c_port = LM_CTRL_TOUCH_HOST,
    .sda_io_num = LM_CTRL_TOUCH_SDA,
    .scl_io_num = LM_CTRL_TOUCH_SCL,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .intr_priority = 0,
    .trans_queue_depth = 0,
    .flags = {
      .enable_internal_pullup = 1,
    },
  };
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_config, &s_i2c_bus), TAG, "I2C bus init failed");

  esp_lcd_touch_config_t touch_config = {
    .x_max = LM_CTRL_LCD_H_RES,
    .y_max = LM_CTRL_LCD_V_RES,
    .rst_gpio_num = LM_CTRL_TOUCH_RST,
    .int_gpio_num = LM_CTRL_TOUCH_INT,
    .levels = {
      .reset = 0,
      .interrupt = 0,
    },
    .flags = {
      .swap_xy = 0,
      .mirror_x = 0,
      .mirror_y = 0,
    },
  };
  esp_lcd_panel_io_handle_t touch_io = NULL;
  esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &touch_io_config, &touch_io), TAG, "Touch IO init failed");
  ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(touch_io, &touch_config, &s_touch), TAG, "Touch controller init failed");

  esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
  ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter_config), TAG, "LVGL adapter init failed");

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
  /*
   * ESP-IDF 5.5 SPI LCD IO does not mark PSRAM color buffers for PSRAM DMA.
   * Keep LVGL flushes in a small internal buffer and cap SPI transfers to avoid
   * transient internal DMA bounce-buffer/descriptor allocation failures after
   * Wi-Fi/BLE/UI startup.
   */
  esp_lv_adapter_display_config_t display_config = {
    .panel = s_panel,
    .panel_io = s_panel_io,
    .profile = {
      .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
      .rotation = ESP_LV_ADAPTER_ROTATE_0,
      .hor_res = LM_CTRL_LCD_H_RES,
      .ver_res = LM_CTRL_LCD_V_RES,
      .buffer_height = LM_CTRL_LCD_DRAW_BUF_ROWS,
      .use_psram = false,
      .enable_ppa_accel = false,
      .require_double_buffer = false,
      .mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
    },
    .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT,
    .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
  };
#else
  esp_lv_adapter_display_config_t display_config =
    ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
      s_panel,
      s_panel_io,
      LM_CTRL_LCD_H_RES,
      LM_CTRL_LCD_V_RES,
      ESP_LV_ADAPTER_ROTATE_0
    );
#endif
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
  ESP_LOGI(
    TAG,
    "Using %d-row internal LVGL draw buffer for ESP-IDF 5.x SPI DMA compatibility",
    LM_CTRL_LCD_DRAW_BUF_ROWS
  );
#endif
  s_display = esp_lv_adapter_register_display(&display_config);
  if (s_display == NULL) {
    ESP_LOGE(TAG, "LVGL display registration failed");
    return ESP_FAIL;
  }

  esp_lv_adapter_touch_config_t lv_touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_display, s_touch);
  esp_lv_adapter_register_touch(&lv_touch_config);

  ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "LVGL adapter start failed");

  if (out_display != NULL) {
    *out_display = s_display;
  }
  return ESP_OK;
}

i2c_master_bus_handle_t lm_ctrl_display_i2c_bus(void) {
  return s_i2c_bus;
}
