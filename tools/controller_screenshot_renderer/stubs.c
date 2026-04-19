#include "stubs.h"

#include <stdio.h>
#include <string.h>

static host_stub_env_t s_env;

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  snprintf(dst, dst_size, "%s", src);
}

void host_stub_set_env(const host_stub_env_t *env) {
  if (env == NULL) {
    memset(&s_env, 0, sizeof(s_env));
    return;
  }

  s_env = *env;
}

void lm_ctrl_machine_link_get_info(lm_ctrl_machine_link_info_t *info) {
  if (info == NULL) {
    return;
  }

  *info = s_env.machine_info;
}

void lm_ctrl_wifi_get_info(lm_ctrl_wifi_info_t *info) {
  if (info == NULL) {
    return;
  }

  *info = s_env.wifi_info;
}

const lv_img_dsc_t *lm_ctrl_wifi_get_custom_logo(void) {
  return s_env.custom_logo;
}

void lm_ctrl_wifi_get_setup_qr_payload(char *buffer, size_t buffer_size) {
  copy_text(buffer, buffer_size, s_env.qr_payload);
}

const char *ctrl_bbw_mode_name(ctrl_bbw_mode_t mode, ctrl_language_t language) {
  switch (mode) {
    case CTRL_BBW_MODE_DOSE_1:
      return language == CTRL_LANGUAGE_DE ? "Dosis 1" : "Dose 1";
    case CTRL_BBW_MODE_DOSE_2:
      return language == CTRL_LANGUAGE_DE ? "Dosis 2" : "Dose 2";
    case CTRL_BBW_MODE_CONTINUOUS:
      return language == CTRL_LANGUAGE_DE ? "Kontinuierlich" : "Continuous";
    default:
      return language == CTRL_LANGUAGE_DE ? "Unbekannt" : "Unknown";
  }
}

const char *ctrl_steam_level_label(ctrl_steam_level_t level) {
  switch (level) {
    case CTRL_STEAM_LEVEL_1:
      return "1";
    case CTRL_STEAM_LEVEL_2:
      return "2";
    case CTRL_STEAM_LEVEL_3:
      return "3";
    case CTRL_STEAM_LEVEL_OFF:
    default:
      return "Off";
  }
}

void ctrl_preset_display_name(const ctrl_preset_t *preset, int preset_index, char *buffer, size_t buffer_size) {
  if (buffer == NULL || buffer_size == 0) {
    return;
  }

  if (preset != NULL && preset->name[0] != '\0') {
    copy_text(buffer, buffer_size, preset->name);
    return;
  }

  snprintf(buffer, buffer_size, "Fire Finch");
  if (preset_index > 0) {
    snprintf(buffer, buffer_size, "Preset %d", preset_index + 1);
  }
}
