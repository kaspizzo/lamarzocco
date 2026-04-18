#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default number of recipe presets exposed in the on-device presets screen. */
#define CTRL_PRESET_DEFAULT_COUNT 4
/** Maximum number of recipe presets supported by the controller and setup portal. */
#define CTRL_PRESET_MAX_COUNT 6
/** Maximum persisted preset name length including the terminating NUL. */
#define CTRL_PRESET_NAME_LEN 33
/** Optional controller features surfaced when the machine dashboard reports them. */
#define CTRL_FEATURE_BBW (1U << 0)
/** Default rotary increment for the coffee boiler temperature. */
#define CTRL_TEMPERATURE_STEP_DEFAULT_C 0.1f
/** Default rotary increment for the prebrewing timing fields. */
#define CTRL_TIME_STEP_DEFAULT_S 0.1f

/** Logical editable fields shown on the controller UI. */
typedef enum {
  CTRL_FOCUS_TEMPERATURE = 0,
  CTRL_FOCUS_INFUSE,
  CTRL_FOCUS_PAUSE,
  CTRL_FOCUS_STEAM,
  CTRL_FOCUS_STANDBY,
  CTRL_FOCUS_BBW_MODE,
  CTRL_FOCUS_BBW_DOSE_1,
  CTRL_FOCUS_BBW_DOSE_2,
  CTRL_FOCUS_COUNT,
} ctrl_focus_t;

/** Brew by weight controller modes mirrored from the La Marzocco cloud dashboard. */
typedef enum {
  CTRL_BBW_MODE_DOSE_1 = 0,
  CTRL_BBW_MODE_DOSE_2,
  CTRL_BBW_MODE_CONTINUOUS,
} ctrl_bbw_mode_t;

/** Discrete steam boiler levels surfaced by the controller. */
typedef enum {
  CTRL_STEAM_LEVEL_OFF = 0,
  CTRL_STEAM_LEVEL_1,
  CTRL_STEAM_LEVEL_2,
  CTRL_STEAM_LEVEL_3,
} ctrl_steam_level_t;

/** Supported on-device controller languages. */
typedef enum {
  CTRL_LANGUAGE_EN = 0,
  CTRL_LANGUAGE_DE,
} ctrl_language_t;

/** High-level UI screens shown on the controller. */
typedef enum {
  CTRL_SCREEN_MAIN = 0,
  CTRL_SCREEN_PRESETS,
  CTRL_SCREEN_SETUP,
  CTRL_SCREEN_SETUP_RESET_ARM,
  CTRL_SCREEN_SETUP_RESET_CONFIRM,
} ctrl_screen_t;

/** Mutable recipe and machine-facing values used by the controller runtime. */
typedef struct {
  float temperature_c;
  float infuse_s;
  float pause_s;
  ctrl_steam_level_t steam_level;
  bool standby_on;
  ctrl_bbw_mode_t bbw_mode;
  float bbw_dose_1_g;
  float bbw_dose_2_g;
} ctrl_values_t;

/** Stored preset definition with optional custom name and recipe values. */
typedef struct {
  char name[CTRL_PRESET_NAME_LEN];
  ctrl_values_t values;
} ctrl_preset_t;

/** Full UI state for the controller firmware. */
typedef struct {
  ctrl_values_t values;
  uint32_t loaded_mask;
  uint32_t feature_mask;
  ctrl_focus_t focus;
  ctrl_screen_t screen;
  ctrl_preset_t presets[CTRL_PRESET_MAX_COUNT];
  uint8_t preset_count;
  uint8_t preset_index;
  float temperature_step_c;
  float time_step_s;
  uint8_t reset_progress;
  bool reset_confirm_yes;
} ctrl_state_t;

/** Actions emitted by the state machine for status text and side effects. */
typedef enum {
  CTRL_ACTION_NONE = 0,
  CTRL_ACTION_APPLY_FIELD,
  CTRL_ACTION_LOAD_PRESET,
  CTRL_ACTION_SAVE_PRESET,
  CTRL_ACTION_OPEN_SETUP,
  CTRL_ACTION_RESET_NETWORK,
} ctrl_action_type_t;

/** Metadata describing the last state transition that should be surfaced to the user. */
typedef struct {
  ctrl_action_type_t type;
  ctrl_focus_t applied_focus;
  int preset_slot;  // 0..(preset_count - 1) if applicable
} ctrl_action_t;

/** Populate the controller state with defaults before loading persisted data. */
void ctrl_state_init(ctrl_state_t *state);
/** Load the current recipe and presets from NVS if present. */
esp_err_t ctrl_state_load(ctrl_state_t *state);
/** Persist the current recipe and presets to NVS. */
esp_err_t ctrl_state_persist(const ctrl_state_t *state);
/** Apply rotary input to the active screen or preset selector. */
void ctrl_rotate(ctrl_state_t *state, int delta_steps);
/** Select a focus field on the main screen. */
void ctrl_set_focus(ctrl_state_t *state, ctrl_focus_t focus);
/** Toggle or switch a discrete field on the main screen. */
void ctrl_toggle_focus(ctrl_state_t *state, ctrl_focus_t focus);
/** Enter the presets overlay. */
void ctrl_open_presets(ctrl_state_t *state);
/** Enter the setup overlay. */
void ctrl_open_setup(ctrl_state_t *state);
/** Enter the hidden setup reset flow. */
void ctrl_open_setup_reset(ctrl_state_t *state);
/** Cancel the setup reset flow and return to the regular setup screen. */
void ctrl_cancel_setup_reset(ctrl_state_t *state);
/** Return from an overlay screen to the main UI. */
void ctrl_close_overlay(ctrl_state_t *state);
/** Confirm the currently armed setup reset action. */
ctrl_action_t ctrl_confirm_setup_reset(ctrl_state_t *state);
/** Load the selected preset into the live recipe values. */
ctrl_action_t ctrl_load_preset(ctrl_state_t *state);
/** Save the current live recipe values into the selected preset slot. */
ctrl_action_t ctrl_save_preset(ctrl_state_t *state);
/** Erase persisted presets and current recipe values. */
esp_err_t ctrl_state_reset_persisted(void);
/** Refresh the persisted recipe slots and advanced edit settings into the live state. */
esp_err_t ctrl_state_refresh_presets(ctrl_state_t *state);
/** Load one active preset slot from NVS-backed controller state, falling back to defaults if needed. */
esp_err_t ctrl_state_load_preset_slot(int preset_index, ctrl_preset_t *preset);
/** Persist one active preset slot without mutating the current live controller values. */
esp_err_t ctrl_state_store_preset_slot(int preset_index, const ctrl_preset_t *preset);
/** Persist advanced controller edit settings and delete hidden presets when the count shrinks. */
esp_err_t ctrl_state_update_advanced_settings(uint8_t preset_count, float temperature_step_c, float time_step_s);
/** Monotonic revision for stored preset definitions. */
uint32_t ctrl_state_preset_version(void);
/** Report whether a preset count is supported by the controller UI. */
bool ctrl_state_is_supported_preset_count(int preset_count);
/** Report whether a setup-portal edit step is supported by the controller. */
bool ctrl_state_is_supported_edit_step(float step);
/** Check whether a numeric value sits on the configured edit grid inside the allowed range. */
bool ctrl_state_value_matches_step(float value, float min_value, float max_value, float step);
/** Human-readable name for a focus field. */
const char *ctrl_focus_name(ctrl_focus_t focus);
/** Human-readable name for a focus field in the selected controller language. */
const char *ctrl_focus_name_for_language(ctrl_focus_t focus, ctrl_language_t language);
/** Human-readable name for a brew by weight mode in the selected controller language. */
const char *ctrl_bbw_mode_name(ctrl_bbw_mode_t mode, ctrl_language_t language);
/** Clamp a raw steam level into the supported controller range. */
ctrl_steam_level_t ctrl_steam_level_normalize(ctrl_steam_level_t level);
/** Report whether the steam boiler should be enabled for the given level. */
bool ctrl_steam_level_enabled(ctrl_steam_level_t level);
/** Return the target temperature mapped to the given steam level. */
float ctrl_steam_level_target_temperature_c(ctrl_steam_level_t level);
/** Parse a known steam target temperature into the matching controller level. */
bool ctrl_steam_level_from_temperature(float temperature_c, ctrl_steam_level_t *level);
/** Parse a cloud steam target code such as `Level1` into the matching controller level. */
bool ctrl_steam_level_from_cloud_code(const char *code, ctrl_steam_level_t *level);
/** Compact label shown on the steam controller page. */
const char *ctrl_steam_level_label(ctrl_steam_level_t level);
/** Stable La Marzocco cloud code for a brew by weight mode. */
const char *ctrl_bbw_mode_cloud_code(ctrl_bbw_mode_t mode);
/** Parse a La Marzocco cloud code into a controller brew by weight mode. */
ctrl_bbw_mode_t ctrl_bbw_mode_from_cloud_code(const char *code);
/** Human-readable name for a screen. */
const char *ctrl_screen_name(ctrl_screen_t screen);
/** Default fallback name for a preset slot. */
void ctrl_preset_default_name(int preset_index, char *buffer, size_t buffer_size);
/** Display name for a preset slot, falling back to the default slot name when blank. */
void ctrl_preset_display_name(const ctrl_preset_t *preset, int preset_index, char *buffer, size_t buffer_size);
/** Stable language code used in the setup portal and NVS settings. */
const char *ctrl_language_code(ctrl_language_t language);
/** Parse a stable language code from the setup portal and NVS settings. */
ctrl_language_t ctrl_language_from_code(const char *code);

#ifdef __cplusplus
}
#endif
