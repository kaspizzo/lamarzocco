#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LM_SCALE_CANDIDATE_LIMIT 6
/* Stored in the controller's encrypted settings namespace; factory reset clears it. */
typedef struct {
  uint8_t version;
  uint8_t enabled;
  uint8_t manual_mode;
  uint8_t address_type;
  char driver[24];
  char address[18];
  char name[21];
} lm_scale_adapter_config_t;
typedef struct {
  char address[18];
  char name[32];
  uint8_t address_type;
  int8_t rssi;
} lm_scale_candidate_t;
typedef struct {
  lm_scale_adapter_config_t config;
  bool source_connected, source_fresh, gateway_connected, advertising, faulted;
  uint32_t source_sessions, gateway_sessions, registration_attempts;
  double grams;
  uint8_t battery;
  char status[128];
  char registration_status[128];
  lm_scale_candidate_t candidates[LM_SCALE_CANDIDATE_LIMIT];
  size_t candidate_count;
} lm_scale_adapter_info_t;

esp_err_t lm_scale_adapter_init(void);
void lm_scale_adapter_on_sync(void);
void lm_scale_adapter_on_reset(void);
void lm_scale_adapter_get_info(lm_scale_adapter_info_t *info);
bool lm_scale_adapter_source_active(void);
bool lm_scale_adapter_source_fresh(void);
esp_err_t lm_scale_adapter_configure(const lm_scale_adapter_config_t *config);
/* Called only by the machine worker, which owns its GATT command channel. */
bool lm_scale_adapter_registration_due(char *name, size_t size);
void lm_scale_adapter_registration_result(const char *status, bool attempted);
