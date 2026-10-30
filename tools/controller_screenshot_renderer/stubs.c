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
