/**
 * Controller state machine and recipe persistence helpers.
 *
 * The state layer stays intentionally small: it owns editable values, overlay
 * navigation, presets, and the NVS-backed recipe snapshot used across reboots.
 */
#include "controller_state.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "ctrl_state";
static const char *CTRL_STATE_NAMESPACE = "ctrl_state";
static const char *CTRL_STATE_KEY_SCHEMA = "schema";
static const char *CTRL_STATE_KEY_CURRENT = "current";
static const char *CTRL_STATE_KEY_SETTINGS = "settings";
static const char *CTRL_STATE_KEY_PERSISTED = "persisted";
static const uint8_t CTRL_STATE_SCHEMA_VERSION = 4;
static const uint8_t CTRL_RESET_ARM_STEPS = 24;
static const float CTRL_STEAM_LEVEL_1_TEMPERATURE_C = 126.0f;
static const float CTRL_STEAM_LEVEL_2_TEMPERATURE_C = 128.0f;
static const float CTRL_STEAM_LEVEL_3_TEMPERATURE_C = 131.0f;
static const float CTRL_EDIT_STEP_FINE = 0.1f;
static const float CTRL_EDIT_STEP_COARSE = 0.5f;
static _Atomic uint32_t s_preset_version = 1;

typedef struct {
  float temperature_c;
  float infuse_s;
  float pause_s;
} ctrl_recipe_values_v1_t;

typedef struct {
  float temperature_c;
  float infuse_s;
  float pause_s;
  uint8_t bbw_mode;
  float bbw_dose_1_g;
  float bbw_dose_2_g;
} ctrl_recipe_values_t;

typedef struct {
  ctrl_recipe_values_t values;
  ctrl_recipe_values_t presets[CTRL_PRESET_DEFAULT_COUNT];
} ctrl_persisted_state_t;

typedef struct {
  char name[CTRL_PRESET_NAME_LEN];
  ctrl_recipe_values_t values;
} ctrl_persisted_preset_t;

typedef struct {
  float temperature_step_c;
  float time_step_s;
  uint8_t preset_count;
  uint8_t reserved[3];
} ctrl_persisted_settings_t;

static void copy_text(char *dst, size_t dst_size, const char *src) {
  size_t len;

  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  len = strnlen(src, dst_size - 1);
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static uint32_t bump_preset_version(void) {
  return atomic_fetch_add_explicit(&s_preset_version, 1U, memory_order_relaxed) + 1U;
}

static void format_preset_key(int preset_index, char *buffer, size_t buffer_size) {
  if (buffer == NULL || buffer_size == 0) {
    return;
  }

  snprintf(buffer, buffer_size, "preset%d", preset_index);
}

void ctrl_preset_default_name(int preset_index, char *buffer, size_t buffer_size) {
  if (buffer == NULL || buffer_size == 0) {
    return;
  }

  snprintf(buffer, buffer_size, "Preset %d", preset_index + 1);
}

void ctrl_preset_display_name(const ctrl_preset_t *preset, int preset_index, char *buffer, size_t buffer_size) {
  if (buffer == NULL || buffer_size == 0) {
    return;
  }

  if (preset != NULL && preset->name[0] != '\0') {
    copy_text(buffer, buffer_size, preset->name);
    return;
  }

  ctrl_preset_default_name(preset_index, buffer, buffer_size);
}

static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static bool approx_equal(float a, float b, float epsilon);

static bool supported_edit_step(float step) {
  return approx_equal(step, CTRL_EDIT_STEP_FINE, 0.0001f) ||
         approx_equal(step, CTRL_EDIT_STEP_COARSE, 0.0001f);
}

static float normalize_edit_step(float step, float fallback) {
  return supported_edit_step(step) ? step : fallback;
}

static uint8_t normalize_preset_count(int preset_count) {
  if (preset_count < 2) {
    return 2;
  }
  if (preset_count > CTRL_PRESET_MAX_COUNT) {
    return CTRL_PRESET_MAX_COUNT;
  }
  return (uint8_t)preset_count;
}

static bool approx_equal(float a, float b, float epsilon) {
  float delta = a - b;

  if (delta < 0.0f) {
    delta = -delta;
  }
  return delta < epsilon;
}

static float snap_to_step(float value, float min_value, float max_value, float step) {
  float clamped = clampf(value, min_value, max_value);
  float scaled;
  int whole_steps;

  if (!supported_edit_step(step)) {
    return clamped;
  }

  scaled = (clamped - min_value) / step;
  whole_steps = scaled >= 0.0f ? (int)(scaled + 0.5f) : (int)(scaled - 0.5f);
  return clampf(min_value + ((float)whole_steps * step), min_value, max_value);
}

static int wrap_index(int value, int count) {
  if (count <= 0) {
    return 0;
  }

  int wrapped = value % count;
  if (wrapped < 0) {
    wrapped += count;
  }
  return wrapped;
}

static ctrl_steam_level_t clamp_steam_level_index(int value) {
  if (value <= (int)CTRL_STEAM_LEVEL_OFF) {
    return CTRL_STEAM_LEVEL_OFF;
  }
  if (value >= (int)CTRL_STEAM_LEVEL_3) {
    return CTRL_STEAM_LEVEL_3;
  }
  return (ctrl_steam_level_t)value;
}

static ctrl_recovery_action_t clamp_recovery_action_index(int value) {
  if (value <= (int)CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD) {
    return CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD;
  }
  if (value >= (int)CTRL_RECOVERY_ACTION_RESET_NETWORK) {
    return CTRL_RECOVERY_ACTION_RESET_NETWORK;
  }
  return (ctrl_recovery_action_t)value;
}

static void copy_values(ctrl_values_t *dst, const ctrl_values_t *src) {
  if (dst == NULL || src == NULL) {
    return;
  }

  dst->temperature_c = src->temperature_c;
  dst->infuse_s = src->infuse_s;
  dst->pause_s = src->pause_s;
  dst->bbw_mode = src->bbw_mode;
  dst->bbw_dose_1_g = src->bbw_dose_1_g;
  dst->bbw_dose_2_g = src->bbw_dose_2_g;
}

static void copy_preset(ctrl_preset_t *dst, const ctrl_preset_t *src) {
  if (dst == NULL || src == NULL) {
    return;
  }

  copy_text(dst->name, sizeof(dst->name), src->name);
  copy_values(&dst->values, &src->values);
}

static ctrl_bbw_mode_t normalize_bbw_mode(ctrl_bbw_mode_t mode) {
  switch (mode) {
    case CTRL_BBW_MODE_DOSE_1:
    case CTRL_BBW_MODE_DOSE_2:
    case CTRL_BBW_MODE_CONTINUOUS:
      return mode;
    default:
      return CTRL_BBW_MODE_DOSE_1;
  }
}

ctrl_steam_level_t ctrl_steam_level_normalize(ctrl_steam_level_t level) {
  switch (level) {
    case CTRL_STEAM_LEVEL_OFF:
    case CTRL_STEAM_LEVEL_1:
    case CTRL_STEAM_LEVEL_2:
    case CTRL_STEAM_LEVEL_3:
      return level;
    default:
      return CTRL_STEAM_LEVEL_OFF;
  }
}

bool ctrl_steam_level_enabled(ctrl_steam_level_t level) {
  return ctrl_steam_level_normalize(level) != CTRL_STEAM_LEVEL_OFF;
}

float ctrl_steam_level_target_temperature_c(ctrl_steam_level_t level) {
  switch (ctrl_steam_level_normalize(level)) {
    case CTRL_STEAM_LEVEL_1:
      return CTRL_STEAM_LEVEL_1_TEMPERATURE_C;
    case CTRL_STEAM_LEVEL_2:
      return CTRL_STEAM_LEVEL_2_TEMPERATURE_C;
    case CTRL_STEAM_LEVEL_3:
      return CTRL_STEAM_LEVEL_3_TEMPERATURE_C;
    case CTRL_STEAM_LEVEL_OFF:
    default:
      return 0.0f;
  }
}

bool ctrl_steam_level_from_temperature(float temperature_c, ctrl_steam_level_t *level) {
  ctrl_steam_level_t parsed_level = CTRL_STEAM_LEVEL_OFF;

  if (level == NULL) {
    return false;
  }

  if (approx_equal(temperature_c, CTRL_STEAM_LEVEL_1_TEMPERATURE_C, 0.25f)) {
    parsed_level = CTRL_STEAM_LEVEL_1;
  } else if (approx_equal(temperature_c, CTRL_STEAM_LEVEL_2_TEMPERATURE_C, 0.25f)) {
    parsed_level = CTRL_STEAM_LEVEL_2;
  } else if (approx_equal(temperature_c, CTRL_STEAM_LEVEL_3_TEMPERATURE_C, 0.25f)) {
    parsed_level = CTRL_STEAM_LEVEL_3;
  } else {
    return false;
  }

  *level = parsed_level;
  return true;
}

bool ctrl_steam_level_from_cloud_code(const char *code, ctrl_steam_level_t *level) {
  ctrl_steam_level_t parsed_level = CTRL_STEAM_LEVEL_OFF;

  if (code == NULL || level == NULL) {
    return false;
  }

  if (strcmp(code, "Level1") == 0) {
    parsed_level = CTRL_STEAM_LEVEL_1;
  } else if (strcmp(code, "Level2") == 0) {
    parsed_level = CTRL_STEAM_LEVEL_2;
  } else if (strcmp(code, "Level3") == 0) {
    parsed_level = CTRL_STEAM_LEVEL_3;
  } else {
    return false;
  }

  *level = parsed_level;
  return true;
}

const char *ctrl_steam_level_label(ctrl_steam_level_t level) {
  switch (ctrl_steam_level_normalize(level)) {
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

static void copy_recipe_values_from_persisted_v1(ctrl_values_t *dst, const ctrl_recipe_values_v1_t *src) {
  if (dst == NULL || src == NULL) {
    return;
  }

  dst->temperature_c = src->temperature_c;
  dst->infuse_s = src->infuse_s;
  dst->pause_s = src->pause_s;
}

static void copy_recipe_values_from_persisted(ctrl_values_t *dst, const ctrl_recipe_values_t *src) {
  if (dst == NULL || src == NULL) {
    return;
  }

  copy_recipe_values_from_persisted_v1(dst, (const ctrl_recipe_values_v1_t *)src);
  dst->bbw_mode = normalize_bbw_mode((ctrl_bbw_mode_t)src->bbw_mode);
  dst->bbw_dose_1_g = src->bbw_dose_1_g;
  dst->bbw_dose_2_g = src->bbw_dose_2_g;
}

static void copy_persisted_from_values(ctrl_recipe_values_t *dst, const ctrl_values_t *src) {
  if (dst == NULL || src == NULL) {
    return;
  }

  dst->temperature_c = src->temperature_c;
  dst->infuse_s = src->infuse_s;
  dst->pause_s = src->pause_s;
  dst->bbw_mode = (uint8_t)normalize_bbw_mode(src->bbw_mode);
  dst->bbw_dose_1_g = src->bbw_dose_1_g;
  dst->bbw_dose_2_g = src->bbw_dose_2_g;
}

static void copy_persisted_preset_from_preset(ctrl_persisted_preset_t *dst, const ctrl_preset_t *src) {
  if (dst == NULL || src == NULL) {
    return;
  }

  memset(dst, 0, sizeof(*dst));
  copy_text(dst->name, sizeof(dst->name), src->name);
  copy_persisted_from_values(&dst->values, &src->values);
}

static void normalize_recipe_values(ctrl_values_t *values) {
  if (values == NULL) {
    return;
  }

  values->temperature_c = clampf(values->temperature_c, 80.0f, 103.0f);
  values->infuse_s = clampf(values->infuse_s, 0.0f, 9.0f);
  values->pause_s = clampf(values->pause_s, 0.0f, 9.0f);
  values->steam_level = ctrl_steam_level_normalize(values->steam_level);
  values->bbw_mode = normalize_bbw_mode(values->bbw_mode);
  values->bbw_dose_1_g = clampf(values->bbw_dose_1_g, 5.0f, 100.0f);
  values->bbw_dose_2_g = clampf(values->bbw_dose_2_g, 5.0f, 100.0f);
}

static void snap_recipe_values_to_steps(ctrl_values_t *values, float temperature_step_c, float time_step_s) {
  if (values == NULL) {
    return;
  }

  values->temperature_c = snap_to_step(values->temperature_c, 80.0f, 103.0f, temperature_step_c);
  values->infuse_s = snap_to_step(values->infuse_s, 0.0f, 9.0f, time_step_s);
  values->pause_s = snap_to_step(values->pause_s, 0.0f, 9.0f, time_step_s);
}

static void normalize_recipe_values_with_steps(ctrl_values_t *values, float temperature_step_c, float time_step_s) {
  normalize_recipe_values(values);
  snap_recipe_values_to_steps(values, temperature_step_c, time_step_s);
}

static void normalize_preset(ctrl_preset_t *preset, float temperature_step_c, float time_step_s) {
  if (preset == NULL) {
    return;
  }

  normalize_recipe_values_with_steps(&preset->values, temperature_step_c, time_step_s);
  preset->name[CTRL_PRESET_NAME_LEN - 1] = '\0';
}

static void set_default_preset(ctrl_preset_t *preset, int preset_index, const ctrl_values_t *default_values) {
  if (preset == NULL || default_values == NULL) {
    return;
  }

  memset(preset, 0, sizeof(*preset));
  copy_values(&preset->values, default_values);
  preset->values.steam_level = default_values->steam_level;
  preset->values.standby_on = default_values->standby_on;
  ctrl_preset_default_name(preset_index, preset->name, sizeof(preset->name));
}

static void reset_inactive_preset_slots(ctrl_state_t *state, uint8_t first_inactive_index) {
  ctrl_values_t default_values = {
    .temperature_c = 93.0f,
    .infuse_s = 1.0f,
    .pause_s = 2.0f,
    .steam_level = CTRL_STEAM_LEVEL_2,
    .standby_on = false,
    .bbw_mode = CTRL_BBW_MODE_DOSE_1,
    .bbw_dose_1_g = 32.0f,
    .bbw_dose_2_g = 34.0f,
  };

  if (state == NULL || first_inactive_index >= CTRL_PRESET_MAX_COUNT) {
    return;
  }

  for (uint8_t preset_index = first_inactive_index; preset_index < CTRL_PRESET_MAX_COUNT; ++preset_index) {
    set_default_preset(&state->presets[preset_index], (int)preset_index, &default_values);
    normalize_preset(&state->presets[preset_index], state->temperature_step_c, state->time_step_s);
  }
}

static void normalize_state_settings(ctrl_state_t *state) {
  if (state == NULL) {
    return;
  }

  state->preset_count = normalize_preset_count(state->preset_count);
  state->temperature_step_c = normalize_edit_step(state->temperature_step_c, CTRL_TEMPERATURE_STEP_DEFAULT_C);
  state->time_step_s = normalize_edit_step(state->time_step_s, CTRL_TIME_STEP_DEFAULT_S);
  if (state->preset_index >= state->preset_count) {
    state->preset_index = (uint8_t)(state->preset_count - 1U);
  }
}

/*
 * The step configuration changes both rotary input and portal form validation,
 * so the state layer snaps persisted values immediately whenever the settings change.
 */
static void apply_advanced_settings(ctrl_state_t *state, uint8_t preset_count, float temperature_step_c, float time_step_s, bool delete_hidden_presets) {
  const uint8_t previous_preset_count = state != NULL ? state->preset_count : 0;

  if (state == NULL) {
    return;
  }

  state->preset_count = preset_count;
  state->temperature_step_c = temperature_step_c;
  state->time_step_s = time_step_s;
  normalize_state_settings(state);
  normalize_recipe_values_with_steps(&state->values, state->temperature_step_c, state->time_step_s);

  for (int preset_index = 0; preset_index < CTRL_PRESET_MAX_COUNT; ++preset_index) {
    normalize_preset(&state->presets[preset_index], state->temperature_step_c, state->time_step_s);
  }

  if (delete_hidden_presets && previous_preset_count > state->preset_count) {
    reset_inactive_preset_slots(state, state->preset_count);
  }
}

static esp_err_t load_recipe_blob(nvs_handle_t handle, const char *key, ctrl_values_t *dst, uint8_t schema_version) {
  ctrl_recipe_values_t stored = {0};
  ctrl_recipe_values_v1_t stored_v1 = {0};
  size_t size = schema_version >= 2 ? sizeof(stored) : sizeof(stored_v1);
  esp_err_t ret;

  if (key == NULL || dst == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (schema_version >= 2) {
    ret = nvs_get_blob(handle, key, &stored, &size);
  } else {
    ret = nvs_get_blob(handle, key, &stored_v1, &size);
  }
  if (ret != ESP_OK) {
    return ret;
  }
  if ((schema_version >= 2 && size != sizeof(stored)) ||
      (schema_version < 2 && size != sizeof(stored_v1))) {
    return ESP_ERR_INVALID_SIZE;
  }

  if (schema_version >= 2) {
    copy_recipe_values_from_persisted(dst, &stored);
  } else {
    copy_recipe_values_from_persisted_v1(dst, &stored_v1);
  }
  normalize_recipe_values(dst);
  return ESP_OK;
}

static esp_err_t store_recipe_blob(nvs_handle_t handle, const char *key, const ctrl_values_t *src) {
  ctrl_recipe_values_t stored = {0};

  if (key == NULL || src == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  copy_persisted_from_values(&stored, src);
  return nvs_set_blob(handle, key, &stored, sizeof(stored));
}

static esp_err_t load_preset_blob(nvs_handle_t handle, const char *key, ctrl_preset_t *dst, uint8_t schema_version, int preset_index) {
  ctrl_persisted_preset_t stored = {0};
  size_t size = sizeof(stored);
  esp_err_t ret;

  if (key == NULL || dst == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (schema_version >= 3) {
    ret = nvs_get_blob(handle, key, &stored, &size);
    if (ret != ESP_OK) {
      return ret;
    }
    if (size != sizeof(stored)) {
      return ESP_ERR_INVALID_SIZE;
    }

    copy_text(dst->name, sizeof(dst->name), stored.name);
    copy_recipe_values_from_persisted(&dst->values, &stored.values);
    normalize_preset(dst, CTRL_TEMPERATURE_STEP_DEFAULT_C, CTRL_TIME_STEP_DEFAULT_S);
    return ESP_OK;
  }

  ret = load_recipe_blob(handle, key, &dst->values, schema_version);
  if (ret != ESP_OK) {
    return ret;
  }

  ctrl_preset_default_name(preset_index, dst->name, sizeof(dst->name));
  normalize_preset(dst, CTRL_TEMPERATURE_STEP_DEFAULT_C, CTRL_TIME_STEP_DEFAULT_S);
  return ESP_OK;
}

static esp_err_t store_preset_blob(nvs_handle_t handle, const char *key, const ctrl_preset_t *src) {
  ctrl_persisted_preset_t stored = {0};

  if (key == NULL || src == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  copy_persisted_preset_from_preset(&stored, src);
  return nvs_set_blob(handle, key, &stored, sizeof(stored));
}

static esp_err_t load_settings_blob(nvs_handle_t handle, ctrl_state_t *state, uint8_t schema_version) {
  ctrl_persisted_settings_t stored = {0};
  size_t size = sizeof(stored);
  esp_err_t ret;

  if (state == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (schema_version < CTRL_STATE_SCHEMA_VERSION) {
    return ESP_ERR_NVS_NOT_FOUND;
  }

  ret = nvs_get_blob(handle, CTRL_STATE_KEY_SETTINGS, &stored, &size);
  if (ret != ESP_OK) {
    return ret;
  }
  if (size != sizeof(stored)) {
    return ESP_ERR_INVALID_SIZE;
  }

  state->preset_count = stored.preset_count;
  state->temperature_step_c = stored.temperature_step_c;
  state->time_step_s = stored.time_step_s;
  return ESP_OK;
}

static esp_err_t store_settings_blob(nvs_handle_t handle, const ctrl_state_t *state) {
  ctrl_persisted_settings_t stored = {0};

  if (state == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  stored.preset_count = state->preset_count;
  stored.temperature_step_c = state->temperature_step_c;
  stored.time_step_s = state->time_step_s;
  return nvs_set_blob(handle, CTRL_STATE_KEY_SETTINGS, &stored, sizeof(stored));
}

static esp_err_t load_versioned_state(
  nvs_handle_t handle,
  ctrl_state_t *state,
  bool *used_versioned_format,
  uint8_t *loaded_schema_version
) {
  uint8_t schema = 0;
  bool loaded_any = false;
  esp_err_t ret;
  int preset_index;

  if (state == NULL || used_versioned_format == NULL || loaded_schema_version == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *used_versioned_format = false;
  *loaded_schema_version = 0;

  ret = nvs_get_u8(handle, CTRL_STATE_KEY_SCHEMA, &schema);
  if (ret != ESP_OK) {
    return ret;
  }
  if (schema < 1 || schema > CTRL_STATE_SCHEMA_VERSION) {
    return ESP_ERR_INVALID_VERSION;
  }

  *used_versioned_format = true;
  *loaded_schema_version = schema;

  ret = load_recipe_blob(handle, CTRL_STATE_KEY_CURRENT, &state->values, schema);
  if (ret == ESP_OK) {
    loaded_any = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "Could not load current recipe: %s", esp_err_to_name(ret));
  }

  for (preset_index = 0; preset_index < CTRL_PRESET_MAX_COUNT; ++preset_index) {
    char preset_key[16];

    format_preset_key(preset_index, preset_key, sizeof(preset_key));
    ret = load_preset_blob(handle, preset_key, &state->presets[preset_index], schema, preset_index);
    if (ret == ESP_OK) {
      loaded_any = true;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(TAG, "Could not load %s: %s", preset_key, esp_err_to_name(ret));
    }
  }

  ret = load_settings_blob(handle, state, schema);
  if (ret == ESP_OK) {
    loaded_any = true;
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "Could not load settings: %s", esp_err_to_name(ret));
  }

  return loaded_any ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

static esp_err_t load_legacy_blob_state(nvs_handle_t handle, ctrl_state_t *state, bool *used_legacy_format) {
  ctrl_persisted_state_t persisted = {0};
  size_t size = sizeof(persisted);
  esp_err_t ret;
  int preset_index;

  if (state == NULL || used_legacy_format == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *used_legacy_format = false;

  ret = nvs_get_blob(handle, CTRL_STATE_KEY_PERSISTED, &persisted, &size);
  if (ret != ESP_OK) {
    return ret;
  }
  if (size != sizeof(persisted)) {
    return ESP_ERR_INVALID_SIZE;
  }

  *used_legacy_format = true;
  copy_recipe_values_from_persisted(&state->values, &persisted.values);
  normalize_recipe_values(&state->values);
  for (preset_index = 0; preset_index < CTRL_PRESET_DEFAULT_COUNT; ++preset_index) {
    copy_recipe_values_from_persisted(&state->presets[preset_index].values, &persisted.presets[preset_index]);
    ctrl_preset_default_name(preset_index, state->presets[preset_index].name, sizeof(state->presets[preset_index].name));
    normalize_preset(&state->presets[preset_index], state->temperature_step_c, state->time_step_s);
  }

  return ESP_OK;
}

static bool *focus_bool_ptr(ctrl_state_t *state, ctrl_focus_t focus) {
  if (state == NULL) {
    return NULL;
  }

  switch (focus) {
    case CTRL_FOCUS_STANDBY:
      return &state->values.standby_on;
    default:
      return NULL;
  }
}

static ctrl_bbw_mode_t cycle_bbw_mode(ctrl_bbw_mode_t mode, int delta_steps) {
  int mode_index = (int)normalize_bbw_mode(mode);

  if (delta_steps == 0) {
    return normalize_bbw_mode(mode);
  }

  mode_index = wrap_index(mode_index + delta_steps, 3);
  return (ctrl_bbw_mode_t)mode_index;
}

static ctrl_action_t make_action(ctrl_action_type_t type, ctrl_focus_t focus, int preset_slot) {
  return (ctrl_action_t){
    .type = type,
    .applied_focus = focus,
    .preset_slot = preset_slot,
  };
}

void ctrl_state_init(ctrl_state_t *state) {
  if (state == NULL) {
    return;
  }

  *state = (ctrl_state_t){0};
  state->values.temperature_c = 93.0f;
  state->values.infuse_s = 1.0f;
  state->values.pause_s = 2.0f;
  state->values.steam_level = CTRL_STEAM_LEVEL_2;
  state->values.standby_on = false;
  state->values.bbw_mode = CTRL_BBW_MODE_DOSE_1;
  state->values.bbw_dose_1_g = 32.0f;
  state->values.bbw_dose_2_g = 34.0f;
  state->loaded_mask = 0;
  state->feature_mask = 0;
  state->focus = CTRL_FOCUS_TEMPERATURE;
  state->screen = CTRL_SCREEN_MAIN;
  state->preset_count = CTRL_PRESET_DEFAULT_COUNT;
  state->preset_index = 0;
  state->temperature_step_c = CTRL_TEMPERATURE_STEP_DEFAULT_C;
  state->time_step_s = CTRL_TIME_STEP_DEFAULT_S;
  state->reset_progress = 0;
  state->recovery_action = CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD;

  for (int preset_index = 0; preset_index < CTRL_PRESET_MAX_COUNT; ++preset_index) {
    set_default_preset(&state->presets[preset_index], preset_index, &state->values);
  }

  state->presets[1] = (ctrl_preset_t){
    .name = "Preset 2",
    .values = {
      .temperature_c = 94.0f,
      .infuse_s = 1.5f,
      .pause_s = 2.5f,
      .bbw_mode = CTRL_BBW_MODE_DOSE_1,
      .bbw_dose_1_g = 34.0f,
      .bbw_dose_2_g = 36.0f,
      .steam_level = CTRL_STEAM_LEVEL_2,
      .standby_on = false,
    },
  };
  state->presets[2] = (ctrl_preset_t){
    .name = "Preset 3",
    .values = {
      .temperature_c = 91.5f,
      .infuse_s = 0.5f,
      .pause_s = 1.0f,
      .bbw_mode = CTRL_BBW_MODE_DOSE_2,
      .bbw_dose_1_g = 28.0f,
      .bbw_dose_2_g = 32.0f,
      .steam_level = CTRL_STEAM_LEVEL_OFF,
      .standby_on = false,
    },
  };
  state->presets[3] = (ctrl_preset_t){
    .name = "Preset 4",
    .values = {
      .temperature_c = 96.0f,
      .infuse_s = 2.5f,
      .pause_s = 3.0f,
      .bbw_mode = CTRL_BBW_MODE_CONTINUOUS,
      .bbw_dose_1_g = 30.0f,
      .bbw_dose_2_g = 38.0f,
      .steam_level = CTRL_STEAM_LEVEL_2,
      .standby_on = true,
    },
  };
  apply_advanced_settings(state, state->preset_count, state->temperature_step_c, state->time_step_s, false);
}

esp_err_t ctrl_state_load(ctrl_state_t *state) {
  nvs_handle_t handle;
  esp_err_t ret;
  bool used_versioned_format = false;
  bool used_legacy_format = false;
  uint8_t loaded_schema_version = 0;

  if (state == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  ret = nvs_open(CTRL_STATE_NAMESPACE, NVS_READONLY, &handle);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Could not open persisted controller state: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = load_versioned_state(handle, state, &used_versioned_format, &loaded_schema_version);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    ret = load_legacy_blob_state(handle, state, &used_legacy_format);
  }
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "Could not load persisted controller state: %s", esp_err_to_name(ret));
  }

  nvs_close(handle);

  if (ret == ESP_OK) {
    apply_advanced_settings(state, state->preset_count, state->temperature_step_c, state->time_step_s, false);
  }

  if (ret == ESP_OK && (used_legacy_format || (used_versioned_format && loaded_schema_version != CTRL_STATE_SCHEMA_VERSION))) {
    esp_err_t migrate_ret = ctrl_state_persist(state);
    if (migrate_ret != ESP_OK) {
      ESP_LOGW(TAG, "Could not migrate persisted controller state: %s", esp_err_to_name(migrate_ret));
    }
  }

  return (ret == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : ret;
}

esp_err_t ctrl_state_persist(const ctrl_state_t *state) {
  ctrl_state_t state_copy;
  nvs_handle_t handle;
  esp_err_t ret;
  int preset_index;

  if (state == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  state_copy = *state;
  apply_advanced_settings(&state_copy, state_copy.preset_count, state_copy.temperature_step_c, state_copy.time_step_s, false);

  ret = nvs_open(CTRL_STATE_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Could not open persisted controller state for writing: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = nvs_set_u8(handle, CTRL_STATE_KEY_SCHEMA, CTRL_STATE_SCHEMA_VERSION);
  if (ret == ESP_OK) {
    ret = store_recipe_blob(handle, CTRL_STATE_KEY_CURRENT, &state_copy.values);
  }
  if (ret == ESP_OK) {
    ret = store_settings_blob(handle, &state_copy);
  }
  for (preset_index = 0; ret == ESP_OK && preset_index < CTRL_PRESET_MAX_COUNT; ++preset_index) {
    char preset_key[16];

    format_preset_key(preset_index, preset_key, sizeof(preset_key));
    ret = store_preset_blob(handle, preset_key, &state_copy.presets[preset_index]);
  }
  if (ret == ESP_OK) {
    ret = nvs_erase_key(handle, CTRL_STATE_KEY_PERSISTED);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
      ret = ESP_OK;
    }
  }
  if (ret == ESP_OK) {
    ret = nvs_commit(handle);
  }
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Could not persist controller state: %s", esp_err_to_name(ret));
  }

  nvs_close(handle);
  return ret;
}

void ctrl_rotate(ctrl_state_t *state, int delta_steps) {
  if (state == NULL || delta_steps == 0) {
    return;
  }

  if (state->screen == CTRL_SCREEN_PRESETS) {
    state->preset_index = (uint8_t)wrap_index((int)state->preset_index + delta_steps, state->preset_count);
    return;
  }

  if (state->screen == CTRL_SCREEN_SETUP_RESET_ARM) {
    int progress = (int)state->reset_progress + delta_steps;

    if (progress < 0) {
      progress = 0;
    }
    if (progress >= CTRL_RESET_ARM_STEPS) {
      state->reset_progress = CTRL_RESET_ARM_STEPS;
      state->recovery_action = CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD;
      state->screen = CTRL_SCREEN_SETUP_RESET_CONFIRM;
      return;
    }

    state->reset_progress = (uint8_t)progress;
    return;
  }

  if (state->screen == CTRL_SCREEN_SETUP_RESET_CONFIRM) {
    state->recovery_action = clamp_recovery_action_index((int)state->recovery_action + delta_steps);
    return;
  }

  if (state->screen != CTRL_SCREEN_MAIN) {
    return;
  }

  switch (state->focus) {
    case CTRL_FOCUS_TEMPERATURE:
      state->values.temperature_c = clampf(
        state->values.temperature_c + (state->temperature_step_c * (float)delta_steps),
        80.0f,
        103.0f
      );
      break;
    case CTRL_FOCUS_INFUSE:
      state->values.infuse_s = clampf(
        state->values.infuse_s + (state->time_step_s * (float)delta_steps),
        0.0f,
        9.0f
      );
      break;
    case CTRL_FOCUS_PAUSE:
      state->values.pause_s = clampf(
        state->values.pause_s + (state->time_step_s * (float)delta_steps),
        0.0f,
        9.0f
      );
      break;
    case CTRL_FOCUS_STEAM:
      state->values.steam_level = clamp_steam_level_index((int)state->values.steam_level + delta_steps);
      break;
    case CTRL_FOCUS_STANDBY:
      if (delta_steps > 0) state->values.standby_on = false;
      if (delta_steps < 0) state->values.standby_on = true;
      break;
    case CTRL_FOCUS_BBW_MODE:
      state->values.bbw_mode = cycle_bbw_mode(state->values.bbw_mode, delta_steps);
      break;
    case CTRL_FOCUS_BBW_DOSE_1:
      state->values.bbw_dose_1_g = clampf(state->values.bbw_dose_1_g + (0.1f * delta_steps), 5.0f, 100.0f);
      break;
    case CTRL_FOCUS_BBW_DOSE_2:
      state->values.bbw_dose_2_g = clampf(state->values.bbw_dose_2_g + (0.1f * delta_steps), 5.0f, 100.0f);
      break;
    default:
      break;
  }
}

void ctrl_set_focus(ctrl_state_t *state, ctrl_focus_t focus) {
  if (state == NULL) {
    return;
  }

  if (focus < CTRL_FOCUS_TEMPERATURE || focus >= CTRL_FOCUS_COUNT) {
    return;
  }

  if ((focus == CTRL_FOCUS_BBW_MODE ||
       focus == CTRL_FOCUS_BBW_DOSE_1 ||
       focus == CTRL_FOCUS_BBW_DOSE_2) &&
      (state->feature_mask & CTRL_FEATURE_BBW) == 0) {
    return;
  }

  state->focus = focus;
  state->screen = CTRL_SCREEN_MAIN;
}

void ctrl_toggle_focus(ctrl_state_t *state, ctrl_focus_t focus) {
  bool *value = focus_bool_ptr(state, focus);
  if (state == NULL) {
    return;
  }

  state->focus = focus;
  state->screen = CTRL_SCREEN_MAIN;

  if (focus == CTRL_FOCUS_STEAM) {
    state->values.steam_level = ctrl_steam_level_enabled(state->values.steam_level)
      ? CTRL_STEAM_LEVEL_OFF
      : CTRL_STEAM_LEVEL_2;
    return;
  }

  if (value == NULL) {
    return;
  }

  *value = !(*value);
}

void ctrl_open_presets(ctrl_state_t *state) {
  if (state == NULL) {
    return;
  }

  if (state->preset_index >= state->preset_count) {
    state->preset_index = (uint8_t)(state->preset_count - 1U);
  }
  state->screen = CTRL_SCREEN_PRESETS;
}

void ctrl_open_setup(ctrl_state_t *state) {
  if (state == NULL) {
    return;
  }

  state->reset_progress = 0;
  state->recovery_action = CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD;
  state->screen = CTRL_SCREEN_SETUP;
}

void ctrl_open_setup_reset(ctrl_state_t *state) {
  if (state == NULL) {
    return;
  }

  state->reset_progress = 0;
  state->recovery_action = CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD;
  state->screen = CTRL_SCREEN_SETUP_RESET_ARM;
}

void ctrl_cancel_setup_reset(ctrl_state_t *state) {
  if (state == NULL) {
    return;
  }

  state->reset_progress = 0;
  state->recovery_action = CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD;
  state->screen = CTRL_SCREEN_SETUP;
}

void ctrl_close_overlay(ctrl_state_t *state) {
  if (state == NULL) {
    return;
  }

  state->reset_progress = 0;
  state->recovery_action = CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD;
  state->screen = CTRL_SCREEN_MAIN;
}

ctrl_action_t ctrl_confirm_setup_reset(ctrl_state_t *state) {
  if (state == NULL || state->screen != CTRL_SCREEN_SETUP_RESET_CONFIRM) {
    return make_action(CTRL_ACTION_NONE, CTRL_FOCUS_TEMPERATURE, -1);
  }

  state->reset_progress = 0;
  {
    const ctrl_recovery_action_t recovery_action = state->recovery_action;
    state->recovery_action = CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD;
    state->screen = CTRL_SCREEN_SETUP;

    switch (recovery_action) {
      case CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD:
        return make_action(CTRL_ACTION_CLEAR_WEB_PASSWORD, CTRL_FOCUS_TEMPERATURE, -1);
      case CTRL_RECOVERY_ACTION_RESET_NETWORK:
        return make_action(CTRL_ACTION_RESET_NETWORK, CTRL_FOCUS_TEMPERATURE, -1);
      case CTRL_RECOVERY_ACTION_COUNT:
      default:
        return make_action(CTRL_ACTION_NONE, CTRL_FOCUS_TEMPERATURE, -1);
    }
  }
}

ctrl_action_t ctrl_load_preset(ctrl_state_t *state) {
  ctrl_action_t action = make_action(CTRL_ACTION_NONE, CTRL_FOCUS_TEMPERATURE, -1);

  if (state == NULL || state->screen != CTRL_SCREEN_PRESETS) {
    return action;
  }
  if (state->preset_index >= state->preset_count) {
    return action;
  }

  copy_values(&state->values, &state->presets[state->preset_index].values);
  normalize_recipe_values_with_steps(&state->values, state->temperature_step_c, state->time_step_s);
  state->screen = CTRL_SCREEN_MAIN;
  return make_action(CTRL_ACTION_LOAD_PRESET, state->focus, (int)state->preset_index);
}

ctrl_action_t ctrl_save_preset(ctrl_state_t *state) {
  ctrl_action_t action = make_action(CTRL_ACTION_NONE, CTRL_FOCUS_TEMPERATURE, -1);

  if (state == NULL || state->screen != CTRL_SCREEN_PRESETS) {
    return action;
  }
  if (state->preset_index >= state->preset_count) {
    return action;
  }

  copy_values(&state->presets[state->preset_index].values, &state->values);
  normalize_preset(&state->presets[state->preset_index], state->temperature_step_c, state->time_step_s);
  bump_preset_version();
  return make_action(CTRL_ACTION_SAVE_PRESET, state->focus, (int)state->preset_index);
}

const char *ctrl_focus_name(ctrl_focus_t focus) {
  return ctrl_focus_name_for_language(focus, CTRL_LANGUAGE_EN);
}

typedef struct {
  const char *en;
  const char *de;
} ctrl_text_entry_t;

static const ctrl_text_entry_t s_ctrl_text[CTRL_TEXT_COUNT] = {
  [CTRL_TEXT_FOCUS_TEMPERATURE] = { "Coffee Boiler", "Kaffeeboiler" },
  [CTRL_TEXT_FOCUS_INFUSE] = { "Prebrewing On-Time", "Prebrewing An-Zeit" },
  [CTRL_TEXT_FOCUS_PAUSE] = { "Prebrewing Off-Time", "Prebrewing Aus-Zeit" },
  [CTRL_TEXT_FOCUS_STEAM] = { "Steam Boiler", "Dampfboiler" },
  [CTRL_TEXT_STATUS] = { "Status", "Status" },
  [CTRL_TEXT_FOCUS_BBW] = { "Brew by Weight", "Brew by Weight" },
  [CTRL_TEXT_FOCUS_BBW_DOSE_1] = { "BBW Dose 1", "BBW Dosis 1" },
  [CTRL_TEXT_FOCUS_BBW_DOSE_2] = { "BBW Dose 2", "BBW Dosis 2" },
  [CTRL_TEXT_PREBREWING] = { "Prebrewing", "Prebrewing" },
  [CTRL_TEXT_SETTING] = { "Setting", "Einstellung" },
  [CTRL_TEXT_BBW_MODE_DOSE_1] = { "Dose 1", "Dosis 1" },
  [CTRL_TEXT_BBW_MODE_DOSE_2] = { "Dose 2", "Dosis 2" },
  [CTRL_TEXT_BBW_MODE_CONTINUOUS] = { "Continuous", "Kontinuierlich" },
  [CTRL_TEXT_STATUS_FIELD_UPDATED_FMT] = { "%s updated.", "%s aktualisiert." },
  [CTRL_TEXT_STATUS_PRESET_LOADED_FMT] = { "Preset %d loaded.", "Preset %d aktiviert." },
  [CTRL_TEXT_STATUS_PRESET_SAVED_FMT] = {
    "Preset %d saved from the current values.",
    "Preset %d aus aktuellen Werten gesichert.",
  },
  [CTRL_TEXT_STATUS_SETUP_LOADING] = { "Controller setup is loading.", "Controller-Setup wird geladen." },
  [CTRL_TEXT_STATUS_CLEAR_WEB_PASSWORD] = {
    "Web password is being cleared.",
    "Web-Passwort wird gelöscht.",
  },
  [CTRL_TEXT_STATUS_RESET_NETWORK] = { "Network reset in progress.", "Netzwerk-Reset wird ausgeführt." },
  [CTRL_TEXT_LOADING_CLOUD_VALUES] = {
    "Values load once\ncloud sync is ready.",
    "Werte werden geladen,\nsobald die Cloud bereit ist.",
  },
  [CTRL_TEXT_LOADING_MACHINE_VALUES] = {
    "Values load once\nthe machine is connected.",
    "Werte werden geladen,\nsobald die Maschine verbunden ist.",
  },
  [CTRL_TEXT_MACHINE_OFFLINE_CLOUD_VALUES] = {
    "Machine offline.\nCloud values unavailable.",
    "Maschine offline.\nCloud-Werte nicht verfügbar.",
  },
  [CTRL_TEXT_MACHINE_UNREACHABLE] = {
    "Machine unreachable.\nConnect via BLE or cloud.",
    "Maschine nicht erreichbar.\nBLE oder Cloud verbinden.",
  },
  [CTRL_TEXT_NO_WATER_HINT] = {
    "No water detected.\nCheck and refill tank.",
    "Kein Wasser erkannt.\nTank prüfen und füllen.",
  },
  [CTRL_TEXT_HINT_TEMPERATURE] = {
    "The coffee boiler setting controls the brewing water temperature.",
    "Die Kaffeeboiler-Einstellung regelt die Wassertemperatur für die Espresso-Zubereitung.",
  },
  [CTRL_TEXT_HINT_INFUSE] = {
    "On-time: pump pulse duration during prebrewing.",
    "An-Zeit: Dauer des Pumpenimpulses während Prebrewing.",
  },
  [CTRL_TEXT_HINT_PAUSE] = {
    "Off-time: pause between prebrewing pump pulses.",
    "Aus-Zeit: Pause zwischen zwei Pumpenimpulsen.",
  },
  [CTRL_TEXT_HINT_HEATING_READY_FMT] = {
    "Heating up.\nReady in %s.",
    "Aufheizen läuft.\nBereit in %s.",
  },
  [CTRL_TEXT_HINT_STEAM_DIRECT] = {
    "Adjust the steam level\ndirectly on the controller.",
    "Dampflevel direkt\nam Controller einstellen.",
  },
  [CTRL_TEXT_ON] = { "On", "An" },
  [CTRL_TEXT_HINT_STATUS_DIRECT] = {
    "Adjust machine status\ndirectly on the controller.",
    "Maschinenstatus direkt\nam Controller einstellen.",
  },
  [CTRL_TEXT_HINT_BBW_MODE] = {
    "Select the active\nbrew-by-weight mode.",
    "Aktiven Brew-by-Weight\nModus wählen.",
  },
  [CTRL_TEXT_HINT_BBW_DOSE_1] = {
    "Target weight for\nBBW dose 1.",
    "Zielgewicht für\nBBW Dosis 1.",
  },
  [CTRL_TEXT_HINT_BBW_DOSE_2] = {
    "Target weight for\nBBW dose 2.",
    "Zielgewicht für\nBBW Dosis 2.",
  },
  [CTRL_TEXT_PRESET_BODY_WITH_BBW_FMT] = {
    "Coffee Boiler %.1f C\nOn-Time %.1f s\nOff-Time %.1f s\nBBW %s\nDose 1 %.1f g\nDose 2 %.1f g",
    "Kaffeeboiler %.1f C\nAn-Zeit %.1f s\nAus-Zeit %.1f s\nBBW %s\nDosis 1 %.1f g\nDosis 2 %.1f g",
  },
  [CTRL_TEXT_PRESET_BODY_BASIC_FMT] = {
    "Coffee Boiler %.1f C\nOn-Time %.1f s\nOff-Time %.1f s",
    "Kaffeeboiler %.1f C\nAn-Zeit %.1f s\nAus-Zeit %.1f s",
  },
  [CTRL_TEXT_SHOT_TIMER_TITLE] = { "Shot Timer", "Shot-Timer" },
  [CTRL_TEXT_LOAD] = { "Load", "Laden" },
  [CTRL_TEXT_SAVE] = { "Save", "Sichern" },
  [CTRL_TEXT_SETUP_PORTAL_STARTING] = { "Setup portal is starting.", "Setup-Portal wird gestartet." },
  [CTRL_TEXT_RESET] = { "Reset", "Zurücksetzen" },
  [CTRL_TEXT_RECOVERY_ARM_OPEN] = {
    "Rotate clockwise once to open recovery.",
    "Einmal im Uhrzeigersinn drehen, um die Wiederherstellung zu öffnen.",
  },
  [CTRL_TEXT_RECOVERY_ARM_CANCEL] = { "Swipe down to cancel.", "Nach unten wischen zum Abbrechen." },
  [CTRL_TEXT_RECOVERY] = { "Recovery", "Wiederherstellung" },
  [CTRL_TEXT_CLEAR_WEB_PASSWORD_ACTION] = { "Clear web password", "Web-Passwort löschen" },
  [CTRL_TEXT_RESET_NETWORK_ACTION] = { "Reset network", "Netzwerk zurücksetzen" },
  [CTRL_TEXT_RECOVERY_PICK_ACTION] = {
    "Rotate to choose the recovery action.",
    "Aktion mit dem Drehknopf wählen.",
  },
  [CTRL_TEXT_BACK] = { "Back", "Zurück" },
  [CTRL_TEXT_RUN] = { "Run", "Start" },
};

const char *ctrl_text(ctrl_text_key_t key, ctrl_language_t language) {
  if (key < 0 || key >= CTRL_TEXT_COUNT) {
    return "";
  }

  return language == CTRL_LANGUAGE_DE ? s_ctrl_text[key].de : s_ctrl_text[key].en;
}

const char *ctrl_focus_name_for_language(ctrl_focus_t focus, ctrl_language_t language) {
  switch (focus) {
    case CTRL_FOCUS_TEMPERATURE:
      return ctrl_text(CTRL_TEXT_FOCUS_TEMPERATURE, language);
    case CTRL_FOCUS_INFUSE:
      return ctrl_text(CTRL_TEXT_FOCUS_INFUSE, language);
    case CTRL_FOCUS_PAUSE:
      return ctrl_text(CTRL_TEXT_FOCUS_PAUSE, language);
    case CTRL_FOCUS_STEAM:
      return ctrl_text(CTRL_TEXT_FOCUS_STEAM, language);
    case CTRL_FOCUS_STANDBY:
      return ctrl_text(CTRL_TEXT_STATUS, language);
    case CTRL_FOCUS_BBW_MODE:
      return ctrl_text(CTRL_TEXT_FOCUS_BBW, language);
    case CTRL_FOCUS_BBW_DOSE_1:
      return ctrl_text(CTRL_TEXT_FOCUS_BBW_DOSE_1, language);
    case CTRL_FOCUS_BBW_DOSE_2:
      return ctrl_text(CTRL_TEXT_FOCUS_BBW_DOSE_2, language);
    default:
      return "Unknown";
  }
}

const char *ctrl_focus_page_title(ctrl_focus_t focus, ctrl_language_t language) {
  switch (focus) {
    case CTRL_FOCUS_INFUSE:
    case CTRL_FOCUS_PAUSE:
      return ctrl_text(CTRL_TEXT_PREBREWING, language);
    case CTRL_FOCUS_STANDBY:
      return ctrl_text(CTRL_TEXT_STATUS, language);
    case CTRL_FOCUS_BBW_MODE:
      return ctrl_text(CTRL_TEXT_FOCUS_BBW, language);
    case CTRL_FOCUS_TEMPERATURE:
    case CTRL_FOCUS_STEAM:
    case CTRL_FOCUS_BBW_DOSE_1:
    case CTRL_FOCUS_BBW_DOSE_2:
      return ctrl_focus_name_for_language(focus, language);
    default:
      return ctrl_text(CTRL_TEXT_SETTING, language);
  }
}

const char *ctrl_bbw_mode_name(ctrl_bbw_mode_t mode, ctrl_language_t language) {
  switch (normalize_bbw_mode(mode)) {
    case CTRL_BBW_MODE_DOSE_1:
      return ctrl_text(CTRL_TEXT_BBW_MODE_DOSE_1, language);
    case CTRL_BBW_MODE_DOSE_2:
      return ctrl_text(CTRL_TEXT_BBW_MODE_DOSE_2, language);
    case CTRL_BBW_MODE_CONTINUOUS:
    default:
      return ctrl_text(CTRL_TEXT_BBW_MODE_CONTINUOUS, language);
  }
}

const char *ctrl_recovery_action_name(ctrl_recovery_action_t action, ctrl_language_t language) {
  switch (action) {
    case CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD:
      return ctrl_text(CTRL_TEXT_CLEAR_WEB_PASSWORD_ACTION, language);
    case CTRL_RECOVERY_ACTION_RESET_NETWORK:
      return ctrl_text(CTRL_TEXT_RESET_NETWORK_ACTION, language);
    default:
      return "";
  }
}

const char *ctrl_bbw_mode_cloud_code(ctrl_bbw_mode_t mode) {
  switch (normalize_bbw_mode(mode)) {
    case CTRL_BBW_MODE_DOSE_2:
      return "Dose2";
    case CTRL_BBW_MODE_CONTINUOUS:
      return "Continuous";
    case CTRL_BBW_MODE_DOSE_1:
    default:
      return "Dose1";
  }
}

ctrl_bbw_mode_t ctrl_bbw_mode_from_cloud_code(const char *code) {
  if (code == NULL) {
    return CTRL_BBW_MODE_DOSE_1;
  }
  if (strcmp(code, "Dose2") == 0) {
    return CTRL_BBW_MODE_DOSE_2;
  }
  if (strcmp(code, "Continuous") == 0) {
    return CTRL_BBW_MODE_CONTINUOUS;
  }
  return CTRL_BBW_MODE_DOSE_1;
}

const char *ctrl_screen_name(ctrl_screen_t screen) {
  switch (screen) {
    case CTRL_SCREEN_MAIN:
      return "Main";
    case CTRL_SCREEN_PRESETS:
      return "Presets";
    case CTRL_SCREEN_SETUP:
      return "Setup";
    case CTRL_SCREEN_SETUP_RESET_ARM:
      return "Setup Reset Arm";
    case CTRL_SCREEN_SETUP_RESET_CONFIRM:
      return "Setup Recovery";
    default:
      return "Unknown";
  }
}

esp_err_t ctrl_state_reset_persisted(void) {
  nvs_handle_t handle;
  esp_err_t ret = nvs_open(CTRL_STATE_NAMESPACE, NVS_READWRITE, &handle);

  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Could not open persisted controller state for reset: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = nvs_erase_all(handle);
  if (ret == ESP_OK) {
    ret = nvs_commit(handle);
  }
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Could not clear persisted controller state: %s", esp_err_to_name(ret));
  }

  nvs_close(handle);
  if (ret == ESP_OK) {
    bump_preset_version();
  }
  return ret;
}

esp_err_t ctrl_state_refresh_presets(ctrl_state_t *state) {
  ctrl_state_t persisted;
  esp_err_t ret;

  if (state == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  ctrl_state_init(&persisted);
  ret = ctrl_state_load(&persisted);
  if (ret != ESP_OK) {
    return ret;
  }

  copy_values(&state->values, &persisted.values);
  state->preset_count = persisted.preset_count;
  state->temperature_step_c = persisted.temperature_step_c;
  state->time_step_s = persisted.time_step_s;
  if (state->preset_index >= state->preset_count) {
    state->preset_index = (uint8_t)(state->preset_count - 1U);
  }
  for (int preset_index = 0; preset_index < CTRL_PRESET_MAX_COUNT; ++preset_index) {
    copy_preset(&state->presets[preset_index], &persisted.presets[preset_index]);
  }

  return ESP_OK;
}

esp_err_t ctrl_state_load_preset_slot(int preset_index, ctrl_preset_t *preset) {
  ctrl_state_t state;
  esp_err_t ret;

  if (preset == NULL || preset_index < 0 || preset_index >= CTRL_PRESET_MAX_COUNT) {
    return ESP_ERR_INVALID_ARG;
  }

  ctrl_state_init(&state);
  ret = ctrl_state_load(&state);
  if (ret != ESP_OK) {
    return ret;
  }
  if (preset_index >= state.preset_count) {
    return ESP_ERR_INVALID_ARG;
  }

  copy_preset(preset, &state.presets[preset_index]);
  return ESP_OK;
}

esp_err_t ctrl_state_store_preset_slot(int preset_index, const ctrl_preset_t *preset) {
  ctrl_state_t state;
  ctrl_preset_t normalized_preset;
  esp_err_t ret;

  if (preset == NULL || preset_index < 0 || preset_index >= CTRL_PRESET_MAX_COUNT) {
    return ESP_ERR_INVALID_ARG;
  }

  ctrl_state_init(&state);
  ret = ctrl_state_load(&state);
  if (ret != ESP_OK) {
    return ret;
  }
  if (preset_index >= state.preset_count) {
    return ESP_ERR_INVALID_ARG;
  }

  copy_preset(&normalized_preset, preset);
  normalize_preset(&normalized_preset, state.temperature_step_c, state.time_step_s);
  copy_preset(&state.presets[preset_index], &normalized_preset);
  ret = ctrl_state_persist(&state);
  if (ret == ESP_OK) {
    bump_preset_version();
  }
  return ret;
}

uint32_t ctrl_state_preset_version(void) {
  return atomic_load_explicit(&s_preset_version, memory_order_relaxed);
}

esp_err_t ctrl_state_update_advanced_settings(uint8_t preset_count, float temperature_step_c, float time_step_s) {
  ctrl_state_t state;
  esp_err_t ret;

  if (!ctrl_state_is_supported_preset_count((int)preset_count) ||
      !ctrl_state_is_supported_edit_step(temperature_step_c) ||
      !ctrl_state_is_supported_edit_step(time_step_s)) {
    return ESP_ERR_INVALID_ARG;
  }

  ctrl_state_init(&state);
  ret = ctrl_state_load(&state);
  if (ret != ESP_OK) {
    return ret;
  }

  apply_advanced_settings(&state, preset_count, temperature_step_c, time_step_s, true);
  ret = ctrl_state_persist(&state);
  if (ret == ESP_OK) {
    bump_preset_version();
  }
  return ret;
}

bool ctrl_state_is_supported_preset_count(int preset_count) {
  return preset_count >= 2 && preset_count <= CTRL_PRESET_MAX_COUNT;
}

bool ctrl_state_is_supported_edit_step(float step) {
  return supported_edit_step(step);
}

bool ctrl_state_value_matches_step(float value, float min_value, float max_value, float step) {
  const float clamped = clampf(value, min_value, max_value);

  if (!supported_edit_step(step)) {
    return false;
  }
  if (!approx_equal(value, clamped, 0.0001f)) {
    return false;
  }

  return approx_equal(clamped, snap_to_step(clamped, min_value, max_value, step), 0.0001f);
}

const char *ctrl_language_code(ctrl_language_t language) {
  switch (language) {
    case CTRL_LANGUAGE_DE:
      return "de";
    case CTRL_LANGUAGE_EN:
    default:
      return "en";
  }
}

ctrl_language_t ctrl_language_from_code(const char *code) {
  if (code != NULL && strcmp(code, "de") == 0) {
    return CTRL_LANGUAGE_DE;
  }

  return CTRL_LANGUAGE_EN;
}
