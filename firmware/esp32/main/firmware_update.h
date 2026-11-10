#pragma once
#include "esp_http_server.h"

/* Call only after the portal's normal API authentication/CSRF checks. */
esp_err_t lm_ctrl_firmware_upload(httpd_req_t *req);
esp_err_t lm_ctrl_firmware_status(httpd_req_t *req);
/* Arm a startup deadline for a pending OTA image before hardware init. */
esp_err_t lm_ctrl_firmware_boot_begin(void);
/* Start the health window after mandatory hardware, storage, UI and input init.
 * Tick from the live main event loop; network/cloud availability is optional. */
void lm_ctrl_firmware_boot_ready(void);
void lm_ctrl_firmware_boot_tick(void);
