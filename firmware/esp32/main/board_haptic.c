#include "board_haptic.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "lm_haptic";

#define LM_CTRL_HAPTIC_ADDR 0x5A
#define LM_CTRL_HAPTIC_TIMEOUT_MS 20

#define DRV2605_REG_STATUS 0x00
#define DRV2605_REG_MODE 0x01
#define DRV2605_REG_LIBRARY 0x03
#define DRV2605_REG_WAVESEQ1 0x04
#define DRV2605_REG_WAVESEQ2 0x05
#define DRV2605_REG_GO 0x0C
#define DRV2605_REG_FEEDBACK 0x1A

#define DRV2605_MODE_INTERNAL_TRIGGER 0x00
#define DRV2605_LIBRARY_LRA 0x06
#define DRV2605_LRA_FEEDBACK 0xB6
#define DRV2605_EFFECT_STRONG_CLICK 0x01

static i2c_master_dev_handle_t s_haptic = NULL;

static esp_err_t haptic_write_reg(uint8_t reg, uint8_t value) {
  const uint8_t payload[2] = {reg, value};
  return i2c_master_transmit(s_haptic, payload, sizeof(payload), LM_CTRL_HAPTIC_TIMEOUT_MS);
}

static esp_err_t haptic_read_reg(uint8_t reg, uint8_t *value) {
  return i2c_master_transmit_receive(s_haptic, &reg, 1, value, 1, LM_CTRL_HAPTIC_TIMEOUT_MS);
}

bool lm_ctrl_haptic_available(void) {
  return s_haptic != NULL;
}

esp_err_t lm_ctrl_haptic_init(i2c_master_bus_handle_t bus) {
  if (bus == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_haptic != NULL) {
    return ESP_OK;
  }

  i2c_device_config_t dev_config = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = LM_CTRL_HAPTIC_ADDR,
    .scl_speed_hz = 400000,
  };
  esp_err_t err = i2c_master_bus_add_device(bus, &dev_config, &s_haptic);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to add DRV2605L on I2C bus: %s", esp_err_to_name(err));
    s_haptic = NULL;
    return err;
  }

  vTaskDelay(pdMS_TO_TICKS(1));

  uint8_t status = 0;
  err = haptic_read_reg(DRV2605_REG_STATUS, &status);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Could not find DRV2605L: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "DRV2605L status=0x%02X", status);
  ESP_RETURN_ON_ERROR(haptic_write_reg(DRV2605_REG_MODE, DRV2605_MODE_INTERNAL_TRIGGER), TAG, "MODE write failed");
  ESP_RETURN_ON_ERROR(haptic_write_reg(DRV2605_REG_LIBRARY, DRV2605_LIBRARY_LRA), TAG, "LIBRARY write failed");
  ESP_RETURN_ON_ERROR(haptic_write_reg(DRV2605_REG_FEEDBACK, DRV2605_LRA_FEEDBACK), TAG, "FEEDBACK write failed");
  ESP_RETURN_ON_ERROR(haptic_write_reg(DRV2605_REG_WAVESEQ1, 0), TAG, "Waveform clear failed");
  ESP_RETURN_ON_ERROR(haptic_write_reg(DRV2605_REG_WAVESEQ2, 0), TAG, "Waveform clear failed");
  return ESP_OK;
}

esp_err_t lm_ctrl_haptic_click(void) {
  if (s_haptic == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = haptic_write_reg(DRV2605_REG_MODE, DRV2605_MODE_INTERNAL_TRIGGER);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "I2C busy, skipped effect");
    return err;
  }
  ESP_RETURN_ON_ERROR(haptic_write_reg(DRV2605_REG_LIBRARY, DRV2605_LIBRARY_LRA), TAG, "LIBRARY write failed");
  ESP_RETURN_ON_ERROR(haptic_write_reg(DRV2605_REG_WAVESEQ1, DRV2605_EFFECT_STRONG_CLICK), TAG, "WAVESEQ1 write failed");
  ESP_RETURN_ON_ERROR(haptic_write_reg(DRV2605_REG_WAVESEQ2, 0), TAG, "WAVESEQ2 write failed");
  return haptic_write_reg(DRV2605_REG_GO, 1);
}
