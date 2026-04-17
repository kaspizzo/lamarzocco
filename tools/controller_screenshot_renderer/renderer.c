#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "controller_ui.h"
#include "stubs.h"

static void dummy_action_cb(lm_ctrl_ui_action_t action, ctrl_focus_t focus, void *user_data) {
  (void)action;
  (void)focus;
  (void)user_data;
}

static void dummy_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  (void)area;
  (void)color_p;
  lv_disp_flush_ready(disp_drv);
}

static bool ensure_directory(const char *path) {
  if (mkdir(path, 0755) == 0) {
    return true;
  }

  return errno == EEXIST;
}

static bool write_bmp(const char *path, const lv_img_dsc_t *image) {
  FILE *file = NULL;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_stride = 0;
  uint32_t pixel_bytes = 0;
  uint32_t file_size = 0;
  uint8_t file_header[14] = {0};
  uint8_t info_header[40] = {0};

  if (path == NULL || image == NULL || image->data == NULL) {
    return false;
  }

  width = image->header.w;
  height = image->header.h;
  row_stride = (width * 3U + 3U) & ~3U;
  pixel_bytes = row_stride * height;
  file_size = 14U + 40U + pixel_bytes;

  file = fopen(path, "wb");
  if (file == NULL) {
    return false;
  }

  file_header[0] = 'B';
  file_header[1] = 'M';
  file_header[2] = (uint8_t)(file_size & 0xFFU);
  file_header[3] = (uint8_t)((file_size >> 8) & 0xFFU);
  file_header[4] = (uint8_t)((file_size >> 16) & 0xFFU);
  file_header[5] = (uint8_t)((file_size >> 24) & 0xFFU);
  file_header[10] = 54U;

  info_header[0] = 40U;
  info_header[4] = (uint8_t)(width & 0xFFU);
  info_header[5] = (uint8_t)((width >> 8) & 0xFFU);
  info_header[6] = (uint8_t)((width >> 16) & 0xFFU);
  info_header[7] = (uint8_t)((width >> 24) & 0xFFU);
  info_header[8] = (uint8_t)(height & 0xFFU);
  info_header[9] = (uint8_t)((height >> 8) & 0xFFU);
  info_header[10] = (uint8_t)((height >> 16) & 0xFFU);
  info_header[11] = (uint8_t)((height >> 24) & 0xFFU);
  info_header[12] = 1U;
  info_header[14] = 24U;
  info_header[20] = (uint8_t)(pixel_bytes & 0xFFU);
  info_header[21] = (uint8_t)((pixel_bytes >> 8) & 0xFFU);
  info_header[22] = (uint8_t)((pixel_bytes >> 16) & 0xFFU);
  info_header[23] = (uint8_t)((pixel_bytes >> 24) & 0xFFU);

  if (fwrite(file_header, sizeof(file_header), 1, file) != 1 ||
      fwrite(info_header, sizeof(info_header), 1, file) != 1) {
    fclose(file);
    return false;
  }

  for (int y = (int)height - 1; y >= 0; --y) {
    const lv_color32_t *row = (const lv_color32_t *)(image->data + ((size_t)y * width * sizeof(lv_color32_t)));
    uint32_t written = 0;

    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t pixel[3] = {
        row[x].ch.blue,
        row[x].ch.green,
        row[x].ch.red,
      };

      if (fwrite(pixel, sizeof(pixel), 1, file) != 1) {
        fclose(file);
        return false;
      }
      written += 3U;
    }

    while (written < row_stride) {
      const uint8_t pad = 0;
      if (fwrite(&pad, sizeof(pad), 1, file) != 1) {
        fclose(file);
        return false;
      }
      written++;
    }
  }

  fclose(file);
  return true;
}

static bool render_to_bmp(const char *path, const ctrl_state_t *state, const host_stub_env_t *env, const char *status_text) {
  lm_ctrl_ui_t ui = {0};
  lv_img_dsc_t *snapshot = NULL;
  bool ok = false;

  if (path == NULL || state == NULL || env == NULL) {
    return false;
  }

  host_stub_set_env(env);
  if (lm_ctrl_ui_init(&ui, state, status_text, dummy_action_cb, NULL) != ESP_OK) {
    return false;
  }

  lv_timer_handler();
  snapshot = lv_snapshot_take(ui.screen, LV_IMG_CF_TRUE_COLOR);
  if (snapshot != NULL) {
    ok = write_bmp(path, snapshot);
    lv_snapshot_free(snapshot);
  }

  if (ui.screen != NULL) {
    lv_obj_t *placeholder = lv_obj_create(NULL);
    lv_obj_remove_style_all(placeholder);
    lv_obj_set_size(placeholder, 360, 360);
    lv_obj_set_style_bg_opa(placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(placeholder, lv_color_black(), 0);
    lv_scr_load(placeholder);
    lv_timer_handler();
    lv_obj_del(ui.screen);
    lv_timer_handler();
  }

  return ok;
}

static host_stub_env_t base_env(void) {
  host_stub_env_t env = {0};

  env.wifi_info.language = CTRL_LANGUAGE_EN;
  env.wifi_info.sta_connected = true;
  env.wifi_info.has_credentials = true;
  env.wifi_info.has_cloud_credentials = true;
  env.wifi_info.has_machine_selection = true;
  env.machine_info.connected = true;
  env.machine_info.authenticated = true;
  return env;
}

static ctrl_state_t base_state(void) {
  ctrl_state_t state = {0};

  state.screen = CTRL_SCREEN_MAIN;
  state.focus = CTRL_FOCUS_TEMPERATURE;
  state.values.temperature_c = 94.0f;
  state.values.infuse_s = 2.0f;
  state.values.pause_s = 9.0f;
  state.values.steam_on = true;
  state.values.standby_on = false;
  state.loaded_mask =
    LM_CTRL_MACHINE_FIELD_TEMPERATURE |
    LM_CTRL_MACHINE_FIELD_INFUSE |
    LM_CTRL_MACHINE_FIELD_PAUSE |
    LM_CTRL_MACHINE_FIELD_STEAM |
    LM_CTRL_MACHINE_FIELD_STANDBY;

  snprintf(state.presets[0].name, sizeof(state.presets[0].name), "Fire Finch");
  state.presets[0].values = state.values;
  state.presets[1].values.temperature_c = 93.0f;
  state.presets[1].values.infuse_s = 1.5f;
  state.presets[1].values.pause_s = 2.0f;
  state.presets[2].values.temperature_c = 91.5f;
  state.presets[2].values.infuse_s = 0.8f;
  state.presets[2].values.pause_s = 1.8f;
  state.presets[3].values.temperature_c = 96.0f;
  state.presets[3].values.infuse_s = 2.0f;
  state.presets[3].values.pause_s = 3.0f;
  return state;
}

static int generate_main_screens(const char *output_dir) {
  host_stub_env_t env = base_env();
  ctrl_state_t state = base_state();
  char path[512];

  snprintf(path, sizeof(path), "%s/boiler.bmp", output_dir);
  if (!render_to_bmp(path, &state, &env, NULL)) {
    return 1;
  }

  state.focus = CTRL_FOCUS_INFUSE;
  snprintf(path, sizeof(path), "%s/prebrew-on-time.bmp", output_dir);
  if (!render_to_bmp(path, &state, &env, NULL)) {
    return 1;
  }

  state.focus = CTRL_FOCUS_PAUSE;
  snprintf(path, sizeof(path), "%s/prebrew-off-time.bmp", output_dir);
  if (!render_to_bmp(path, &state, &env, NULL)) {
    return 1;
  }

  state.focus = CTRL_FOCUS_STEAM;
  snprintf(path, sizeof(path), "%s/steam-boiler.bmp", output_dir);
  if (!render_to_bmp(path, &state, &env, NULL)) {
    return 1;
  }

  state.focus = CTRL_FOCUS_STANDBY;
  snprintf(path, sizeof(path), "%s/standby.bmp", output_dir);
  if (!render_to_bmp(path, &state, &env, NULL)) {
    return 1;
  }

  state.screen = CTRL_SCREEN_PRESETS;
  state.preset_index = 0;
  snprintf(path, sizeof(path), "%s/preset-view.bmp", output_dir);
  if (!render_to_bmp(path, &state, &env, NULL)) {
    return 1;
  }

  return 0;
}

static int generate_setup_screens(const char *output_dir) {
  host_stub_env_t env = {0};
  ctrl_state_t state = base_state();
  char path[512];

  state.screen = CTRL_SCREEN_SETUP;
  env.wifi_info.language = CTRL_LANGUAGE_EN;
  snprintf(env.qr_payload, sizeof(env.qr_payload), "WIFI:T:WPA;S:LM-CTRL-76C1;P:LMCTRL-04EF76C1;;");

  snprintf(path, sizeof(path), "%s/first-setup.bmp", output_dir);
  if (!render_to_bmp(
        path,
        &state,
        &env,
        "AP: LM-CTRL-76C1\nPassword: LMCTRL-04EF76C1\nIP: 192.168.4.1"
      )) {
    return 1;
  }

  env.wifi_info.sta_connected = true;
  env.wifi_info.has_credentials = true;
  env.machine_info.connected = true;
  env.machine_info.authenticated = true;
  snprintf(env.qr_payload, sizeof(env.qr_payload), "https://lamarzocco.local/");

  snprintf(path, sizeof(path), "%s/setup-with-wlan-connected.bmp", output_dir);
  if (!render_to_bmp(path, &state, &env, "IP: 192.168.178.137")) {
    return 1;
  }

  return 0;
}

int main(void) {
  static lv_color_t display_buffer[360 * 360];
  static lv_disp_draw_buf_t draw_buf;
  static lv_disp_drv_t disp_drv;
  const char *output_dir = "out";

  if (!ensure_directory(output_dir)) {
    fprintf(stderr, "Could not create output directory: %s\n", output_dir);
    return 1;
  }

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, display_buffer, NULL, 360 * 360);
  lv_disp_drv_init(&disp_drv);
  disp_drv.draw_buf = &draw_buf;
  disp_drv.flush_cb = dummy_flush_cb;
  disp_drv.hor_res = 360;
  disp_drv.ver_res = 360;
  lv_disp_drv_register(&disp_drv);
  {
    lv_obj_t *boot_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(boot_screen);
    lv_obj_set_size(boot_screen, 360, 360);
    lv_obj_set_style_bg_opa(boot_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(boot_screen, lv_color_black(), 0);
    lv_scr_load(boot_screen);
    lv_timer_handler();
  }

  if (generate_main_screens(output_dir) != 0 || generate_setup_screens(output_dir) != 0) {
    fprintf(stderr, "Failed to render controller screenshots.\n");
    return 1;
  }

  return 0;
}
