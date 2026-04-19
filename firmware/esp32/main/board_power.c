#include "board_power.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "board_config.h"

static const char *TAG = "lm_power";
static const int64_t LM_CTRL_POWER_SAMPLE_INTERVAL_US =
  (int64_t)LM_CTRL_BATTERY_SAMPLE_INTERVAL_MS * 1000LL;

typedef struct {
  bool initialized;
  bool available;
  bool charging;
  bool low;
  bool usb_connected;
  bool calibration_ready;
  uint8_t level_percent;
  uint32_t status_version;
  adc_oneshot_unit_handle_t adc_handle;
  adc_cali_handle_t cali_handle;
  adc_unit_t adc_unit;
  adc_channel_t adc_channel;
  esp_timer_handle_t sample_timer;
} lm_ctrl_power_state_t;

static lm_ctrl_power_state_t s_power = {0};
static portMUX_TYPE s_power_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_gpio_isr_service_installed = false;

static bool has_battery_adc(void) {
  return LM_CTRL_BATTERY_ADC_GPIO != GPIO_NUM_NC;
}

static bool has_charging_gpio(void) {
  return LM_CTRL_BATTERY_CHARGE_GPIO != GPIO_NUM_NC;
}

static bool battery_state_changed(bool available, bool charging, bool low, bool usb_connected, uint8_t level_percent) {
  return s_power.available != available ||
         s_power.charging != charging ||
         s_power.low != low ||
         s_power.usb_connected != usb_connected ||
         s_power.level_percent != level_percent;
}

static uint8_t interpolate_level(int battery_mv, int lower_mv, int upper_mv, uint8_t lower_pct, uint8_t upper_pct) {
  const int span_mv = upper_mv - lower_mv;
  const int delta_mv = battery_mv - lower_mv;

  if (span_mv <= 0) {
    return lower_pct;
  }

  return (uint8_t)(lower_pct + ((delta_mv * (int)(upper_pct - lower_pct)) / span_mv));
}

static uint8_t battery_level_from_mv(int battery_mv) {
  if (battery_mv <= 3300) {
    return 0;
  }
  if (battery_mv < 3520) {
    return interpolate_level(battery_mv, 3300, 3520, 0, 20);
  }
  if (battery_mv < 3640) {
    return interpolate_level(battery_mv, 3520, 3640, 20, 40);
  }
  if (battery_mv < 3760) {
    return interpolate_level(battery_mv, 3640, 3760, 40, 60);
  }
  if (battery_mv < 3880) {
    return interpolate_level(battery_mv, 3760, 3880, 60, 80);
  }
  if (battery_mv < 4000) {
    return interpolate_level(battery_mv, 3880, 4000, 80, 100);
  }
  return 100;
}

static bool read_charging_state(void) {
  if (!has_charging_gpio()) {
    return false;
  }

  return gpio_get_level(LM_CTRL_BATTERY_CHARGE_GPIO) == LM_CTRL_BATTERY_CHARGE_ACTIVE_LEVEL;
}

static bool read_usb_connected_state(void) {
  return usb_serial_jtag_is_connected();
}

static void update_power_state(bool available, uint8_t level_percent, bool charging, bool usb_connected) {
  const bool low = available && level_percent < LM_CTRL_BATTERY_LOW_PERCENT;

  portENTER_CRITICAL(&s_power_lock);
  if (battery_state_changed(available, charging, low, usb_connected, level_percent)) {
    s_power.status_version++;
  }
  s_power.available = available;
  s_power.charging = charging;
  s_power.low = low;
  s_power.usb_connected = usb_connected;
  s_power.level_percent = level_percent;
  portEXIT_CRITICAL(&s_power_lock);
}

static void sync_live_power_signals(void) {
  bool available;
  bool charging;
  bool usb_connected;
  bool low;
  uint8_t level_percent;

  charging = read_charging_state();
  usb_connected = read_usb_connected_state();

  portENTER_CRITICAL(&s_power_lock);
  available = s_power.available;
  level_percent = s_power.level_percent;
  low = available && level_percent < LM_CTRL_BATTERY_LOW_PERCENT;
  if (battery_state_changed(available, charging, low, usb_connected, level_percent)) {
    s_power.status_version++;
  }
  s_power.charging = charging;
  s_power.low = low;
  s_power.usb_connected = usb_connected;
  portEXIT_CRITICAL(&s_power_lock);
}

static esp_err_t read_battery_voltage_mv(int *battery_mv) {
  int raw = 0;
  int sensed_mv = 0;
  esp_err_t ret;

  if (battery_mv == NULL || s_power.adc_handle == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  ret = adc_oneshot_read(s_power.adc_handle, s_power.adc_channel, &raw);
  if (ret != ESP_OK) {
    return ret;
  }

  if (s_power.calibration_ready && s_power.cali_handle != NULL) {
    ret = adc_cali_raw_to_voltage(s_power.cali_handle, raw, &sensed_mv);
    if (ret != ESP_OK) {
      return ret;
    }
  } else {
    sensed_mv = (raw * 3300) / 4095;
  }

  *battery_mv = (int)((float)sensed_mv * LM_CTRL_BATTERY_VOLTAGE_DIVIDER);
  return ESP_OK;
}

static void sample_power_state(void) {
  int battery_mv = 0;

  if (read_battery_voltage_mv(&battery_mv) != ESP_OK) {
    ESP_LOGW(TAG, "Battery ADC read failed");
    return;
  }

  update_power_state(true, battery_level_from_mv(battery_mv), read_charging_state(), read_usb_connected_state());
}

static void sample_power_timer_cb(void *arg) {
  (void)arg;
  sample_power_state();
}

static void charging_gpio_isr_cb(void *arg) {
  bool available;
  bool charging;
  bool usb_connected;
  bool low;
  uint8_t level_percent;

  (void)arg;
  if (!has_charging_gpio()) {
    return;
  }

  charging = read_charging_state();
  usb_connected = read_usb_connected_state();
  portENTER_CRITICAL_ISR(&s_power_lock);
  available = s_power.available;
  level_percent = s_power.level_percent;
  low = available && level_percent < LM_CTRL_BATTERY_LOW_PERCENT;
  if (battery_state_changed(available, charging, low, usb_connected, level_percent)) {
    s_power.status_version++;
  }
  s_power.charging = charging;
  s_power.low = low;
  s_power.usb_connected = usb_connected;
  portEXIT_CRITICAL_ISR(&s_power_lock);
}

static esp_err_t init_charging_gpio(void) {
  gpio_config_t config = {0};
  unsigned charge_gpio = 0;
  esp_err_t ret;

  if (!has_charging_gpio()) {
    ESP_LOGI(TAG, "Battery charge detect GPIO not configured");
    return ESP_OK;
  }

  charge_gpio = (unsigned)(int)LM_CTRL_BATTERY_CHARGE_GPIO;
  config.pin_bit_mask = 1ULL << charge_gpio;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_ANYEDGE;

  ret = gpio_config(&config);
  if (ret != ESP_OK) {
    return ret;
  }

  if (!s_gpio_isr_service_installed) {
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
      return ret;
    }
    s_gpio_isr_service_installed = true;
  }

  ret = gpio_isr_handler_add(LM_CTRL_BATTERY_CHARGE_GPIO, charging_gpio_isr_cb, NULL);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_LOGI(TAG, "Battery charge detect configured on GPIO %d", LM_CTRL_BATTERY_CHARGE_GPIO);
  return ESP_OK;
}

static void init_calibration_if_available(adc_channel_t channel) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t cali_config = {
    .unit_id = s_power.adc_unit,
    .chan = channel,
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
  };

  if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_power.cali_handle) == ESP_OK) {
    s_power.calibration_ready = true;
  } else {
    ESP_LOGW(TAG, "ADC calibration unavailable, falling back to raw battery estimates");
  }
#else
  (void)channel;
#endif
}

esp_err_t lm_ctrl_power_init(void) {
  adc_oneshot_unit_init_cfg_t unit_config = {0};
  adc_oneshot_chan_cfg_t channel_config = {
    .bitwidth = ADC_BITWIDTH_DEFAULT,
    .atten = ADC_ATTEN_DB_12,
  };
  esp_timer_create_args_t timer_args = {
    .callback = sample_power_timer_cb,
    .arg = NULL,
    .name = "lm_battery",
  };
  adc_unit_t adc_unit = ADC_UNIT_1;
  adc_channel_t adc_channel = ADC_CHANNEL_0;
  esp_err_t ret;

  if (s_power.initialized) {
    return ESP_OK;
  }

  if (!has_battery_adc()) {
    s_power.initialized = true;
    return ESP_OK;
  }

  ret = init_charging_gpio();
  if (ret != ESP_OK) {
    return ret;
  }

  ret = adc_oneshot_io_to_channel((int)LM_CTRL_BATTERY_ADC_GPIO, &adc_unit, &adc_channel);
  if (ret != ESP_OK) {
    return ret;
  }

  unit_config.unit_id = adc_unit;
  unit_config.ulp_mode = ADC_ULP_MODE_DISABLE;
  ret = adc_oneshot_new_unit(&unit_config, &s_power.adc_handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = adc_oneshot_config_channel(s_power.adc_handle, adc_channel, &channel_config);
  if (ret != ESP_OK) {
    return ret;
  }

  s_power.adc_unit = adc_unit;
  s_power.adc_channel = adc_channel;
  init_calibration_if_available(adc_channel);

  ret = esp_timer_create(&timer_args, &s_power.sample_timer);
  if (ret != ESP_OK) {
    return ret;
  }

  sample_power_state();
  ret = esp_timer_start_periodic(s_power.sample_timer, LM_CTRL_POWER_SAMPLE_INTERVAL_US);
  if (ret != ESP_OK) {
    return ret;
  }

  s_power.initialized = true;
  ESP_LOGI(
    TAG,
    "Battery monitor initialized on GPIO %d with %lu ms sampling",
    LM_CTRL_BATTERY_ADC_GPIO,
    (unsigned long)LM_CTRL_BATTERY_SAMPLE_INTERVAL_MS
  );
  return ESP_OK;
}

void lm_ctrl_power_get_info(lm_ctrl_power_info_t *info) {
  if (info == NULL) {
    return;
  }

  sync_live_power_signals();
  memset(info, 0, sizeof(*info));
  portENTER_CRITICAL(&s_power_lock);
  info->available = s_power.available;
  info->charging = s_power.charging;
  info->low = s_power.low;
  info->usb_connected = s_power.usb_connected;
  info->level_percent = s_power.level_percent;
  portEXIT_CRITICAL(&s_power_lock);
}

uint32_t lm_ctrl_power_status_version(void) {
  uint32_t version;

  sync_live_power_signals();
  portENTER_CRITICAL(&s_power_lock);
  version = s_power.status_version;
  portEXIT_CRITICAL(&s_power_lock);

  return version;
}
