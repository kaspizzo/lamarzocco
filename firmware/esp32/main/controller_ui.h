#pragma once

#include "esp_err.h"
#include "lvgl.h"

#include "controller_state.h"

/** Maximum number of horizontally swipeable main pages in the round UI. */
#define LM_CTRL_UI_MAIN_PAGE_COUNT 8
/** Maximum number of touch bindings stored for button-like actions. */
#define LM_CTRL_UI_BINDING_COUNT 6
/** Maximum setup status text length passed into the UI view model. */
#define LM_CTRL_UI_STATUS_TEXT_LEN 256
/** Maximum setup QR payload length passed into the UI view model. */
#define LM_CTRL_UI_SETUP_QR_LEN 192

/** Read-only UI view model built by the runtime layer. */
typedef struct {
  ctrl_language_t language;
  bool wifi_visible;
  bool wifi_connected;
  bool ble_visible;
  bool ble_authenticated;
  const lv_img_dsc_t *custom_logo;
  char setup_status_text[LM_CTRL_UI_STATUS_TEXT_LEN];
  char setup_qr_payload[LM_CTRL_UI_SETUP_QR_LEN];
} lm_ctrl_ui_view_t;

/** UI-level actions forwarded back into the controller input queue. */
typedef enum {
  LM_CTRL_UI_ACTION_SELECT_FOCUS = 0,
  LM_CTRL_UI_ACTION_TOGGLE_FOCUS,
  LM_CTRL_UI_ACTION_OPEN_PRESETS,
  LM_CTRL_UI_ACTION_LOAD_PRESET,
  LM_CTRL_UI_ACTION_SAVE_PRESET,
  LM_CTRL_UI_ACTION_OPEN_SETUP,
  LM_CTRL_UI_ACTION_CLOSE_SCREEN,
  LM_CTRL_UI_ACTION_OPEN_SETUP_RESET,
  LM_CTRL_UI_ACTION_CANCEL_SETUP_RESET,
  LM_CTRL_UI_ACTION_CONFIRM_SETUP_RESET,
} lm_ctrl_ui_action_t;

/** Callback invoked when the UI needs the main loop to handle a touch action. */
typedef void (*lm_ctrl_ui_action_cb_t)(lm_ctrl_ui_action_t action, ctrl_focus_t focus, void *user_data);

typedef struct lm_ctrl_ui_s lm_ctrl_ui_t;

/** Touch binding metadata used to map LVGL objects back to controller actions. */
typedef struct {
  lm_ctrl_ui_t *ui;
  lm_ctrl_ui_action_t action;
  ctrl_focus_t focus;
} lm_ctrl_ui_binding_t;

/** Live LVGL object tree for the round controller UI. */
struct lm_ctrl_ui_s {
  lv_obj_t *screen;
  lv_obj_t *ring;
  lv_obj_t *title_text;
  lv_obj_t *title_image;
  lv_obj_t *wifi_icon;
  lv_obj_t *ble_icon;
  lv_obj_t *page_label;
  lv_obj_t *page_dots[LM_CTRL_UI_MAIN_PAGE_COUNT];

  lv_obj_t *main_card;
  lv_obj_t *focus;
  lv_obj_t *value;
  lv_obj_t *hint;
  lv_obj_t *power_left_button;
  lv_obj_t *power_left_title;
  lv_obj_t *power_left_value;
  lv_obj_t *power_right_button;
  lv_obj_t *power_right_title;
  lv_obj_t *power_right_value;
  lv_obj_t *power_hint;

  lv_obj_t *presets_card;
  lv_obj_t *presets_title;
  lv_obj_t *presets_name;
  lv_obj_t *presets_body;
  lv_obj_t *presets_load_button;
  lv_obj_t *presets_load_label;
  lv_obj_t *presets_save_button;
  lv_obj_t *presets_save_label;

  lv_obj_t *setup_card;
  lv_obj_t *setup_title;
  lv_obj_t *setup_qr;
  lv_obj_t *setup_body;
  lv_obj_t *setup_action_list;
  lv_obj_t *setup_reset_arc;
  lv_obj_t *setup_secondary_button;
  lv_obj_t *setup_secondary_label;
  lv_obj_t *setup_primary_button;
  lv_obj_t *setup_primary_label;

  ctrl_focus_t rendered_focus;
  ctrl_screen_t rendered_screen;
  uint32_t rendered_feature_mask;
  lm_ctrl_ui_action_cb_t action_cb;
  void *action_user_data;
  lm_ctrl_ui_binding_t bindings[LM_CTRL_UI_BINDING_COUNT];
};

/**
 * Build the LVGL controller UI on the active display.
 *
 * The initial render uses the provided state and status text, then later updates
 * are applied through lm_ctrl_ui_render().
 */
esp_err_t lm_ctrl_ui_init(
  lm_ctrl_ui_t *ui,
  const ctrl_state_t *state,
  const lm_ctrl_ui_view_t *view,
  lm_ctrl_ui_action_cb_t action_cb,
  void *action_user_data
);
/** Refresh the UI to reflect the latest controller state and setup status text. */
void lm_ctrl_ui_render(lm_ctrl_ui_t *ui, const ctrl_state_t *state, const lm_ctrl_ui_view_t *view);
