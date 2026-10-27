#pragma once

#include "machine_link.h"
#include "wifi_setup.h"

typedef struct {
  lm_ctrl_wifi_info_t wifi_info;
  lm_ctrl_machine_link_info_t machine_info;
  bool scale_source_fresh;
  bool all_status_icons;
  const lv_img_dsc_t *custom_logo;
  char qr_payload[192];
} host_stub_env_t;

void host_stub_set_env(const host_stub_env_t *env);
