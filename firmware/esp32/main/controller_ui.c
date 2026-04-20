/**
 * LVGL-based round UI renderer for the controller display.
 *
 * The UI module is intentionally dumb: it renders the supplied controller
 * state and forwards touch actions back into the main loop via callbacks.
 */
#include "controller_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "extra/libs/qrcode/lv_qrcode.h"
#include "lm_ctrl_fonts.h"
#include "machine_link_types.h"

static const char *TAG = "lm_ui";
static const char *TITLE_TEXT = "la marzocco";

static const lv_color_t COLOR_BG = LV_COLOR_MAKE(0x11, 0x0B, 0x08);
static const lv_color_t COLOR_RING = LV_COLOR_MAKE(0x92, 0x63, 0x33);
static const lv_color_t COLOR_TEXT = LV_COLOR_MAKE(0xF4, 0xEF, 0xE7);
static const lv_color_t COLOR_MUTED = LV_COLOR_MAKE(0xB7, 0xA0, 0x8B);
static const lv_color_t COLOR_ACTIVE = LV_COLOR_MAKE(0xFF, 0xC6, 0x85);
static const lv_color_t COLOR_BUTTON = LV_COLOR_MAKE(0x3A, 0x2A, 0x21);
static const char *BATTERY_CHARGING_SYMBOL = LV_SYMBOL_BATTERY_FULL LV_SYMBOL_CHARGE;
static const char *ICON_SLASH_SYMBOL = "/";

#define UI_FONT_14 (&lm_ctrl_font_montserrat_14)
#define UI_FONT_16 (&lm_ctrl_font_montserrat_16)
#define UI_FONT_20 (&lm_ctrl_font_montserrat_20)
#define UI_FONT_28 (&lm_ctrl_font_montserrat_28)
#define UI_FONT_40 (&lm_ctrl_font_montserrat_40)

static const ctrl_focus_t MAIN_PAGE_ORDER[LM_CTRL_UI_MAIN_PAGE_COUNT] = {
  CTRL_FOCUS_TEMPERATURE,
  CTRL_FOCUS_INFUSE,
  CTRL_FOCUS_PAUSE,
  CTRL_FOCUS_STEAM,
  CTRL_FOCUS_STANDBY,
  CTRL_FOCUS_BBW_MODE,
  CTRL_FOCUS_BBW_DOSE_1,
  CTRL_FOCUS_BBW_DOSE_2,
};

enum {
  BIND_POWER_STEAM = 0,
  BIND_POWER_STANDBY,
  BIND_PRESET_LOAD,
  BIND_PRESET_SAVE,
  BIND_SETUP_RESET_CANCEL,
  BIND_SETUP_RESET_CONFIRM,
};

static bool focus_supported(uint32_t feature_mask, ctrl_focus_t focus) {
  if (focus == CTRL_FOCUS_BBW_MODE || focus == CTRL_FOCUS_BBW_DOSE_1 || focus == CTRL_FOCUS_BBW_DOSE_2) {
    return (feature_mask & CTRL_FEATURE_BBW) != 0;
  }

  return focus >= CTRL_FOCUS_TEMPERATURE && focus < CTRL_FOCUS_COUNT;
}

static size_t main_page_count(uint32_t feature_mask) {
  size_t count = 0;

  for (size_t i = 0; i < LM_CTRL_UI_MAIN_PAGE_COUNT; ++i) {
    if (focus_supported(feature_mask, MAIN_PAGE_ORDER[i])) {
      count++;
    }
  }

  return count;
}

static int main_page_index(uint32_t feature_mask, ctrl_focus_t focus) {
  int page_index = 0;

  for (size_t i = 0; i < LM_CTRL_UI_MAIN_PAGE_COUNT; ++i) {
    if (!focus_supported(feature_mask, MAIN_PAGE_ORDER[i])) {
      continue;
    }
    if (MAIN_PAGE_ORDER[i] == focus) {
      return page_index;
    }
    page_index++;
  }

  return 0;
}

static ctrl_focus_t focus_from_page_index(uint32_t feature_mask, int index) {
  const size_t count = main_page_count(feature_mask);
  int wrapped;
  int page_index = 0;

  if (count == 0) {
    return CTRL_FOCUS_TEMPERATURE;
  }

  wrapped = index % (int)count;
  if (wrapped < 0) {
    wrapped += (int)count;
  }

  for (size_t i = 0; i < LM_CTRL_UI_MAIN_PAGE_COUNT; ++i) {
    if (!focus_supported(feature_mask, MAIN_PAGE_ORDER[i])) {
      continue;
    }
    if (page_index == wrapped) {
      return MAIN_PAGE_ORDER[i];
    }
    page_index++;
  }

  return CTRL_FOCUS_TEMPERATURE;
}

static const char *focus_title(ctrl_focus_t focus, ctrl_language_t language) {
  switch (focus) {
    case CTRL_FOCUS_TEMPERATURE:
      return language == CTRL_LANGUAGE_DE ? "Kaffeeboiler" : "Coffee Boiler";
    case CTRL_FOCUS_INFUSE:
      return "Prebrewing";
    case CTRL_FOCUS_PAUSE:
      return "Prebrewing";
    case CTRL_FOCUS_STEAM:
      return language == CTRL_LANGUAGE_DE ? "Dampfboiler" : "Steam Boiler";
    case CTRL_FOCUS_STANDBY:
      return "Status";
    case CTRL_FOCUS_BBW_MODE:
      return "Brew by Weight";
    case CTRL_FOCUS_BBW_DOSE_1:
      return language == CTRL_LANGUAGE_DE ? "BBW Dosis 1" : "BBW Dose 1";
    case CTRL_FOCUS_BBW_DOSE_2:
      return language == CTRL_LANGUAGE_DE ? "BBW Dosis 2" : "BBW Dose 2";
    default:
      return language == CTRL_LANGUAGE_DE ? "Einstellung" : "Setting";
  }
}

static bool is_setup_reset_screen(ctrl_screen_t screen) {
  return screen == CTRL_SCREEN_SETUP_RESET_ARM || screen == CTRL_SCREEN_SETUP_RESET_CONFIRM;
}

static void set_hidden(lv_obj_t *obj, bool hidden) {
  if (obj == NULL) {
    return;
  }

  if (hidden) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
}

static void set_label_text(lv_obj_t *label, const char *text, lv_color_t color) {
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, color, 0);
}

static void style_page_dot(lv_obj_t *dot, bool active) {
  lv_obj_set_style_bg_color(dot, active ? COLOR_ACTIVE : COLOR_RING, 0);
  lv_obj_set_style_bg_opa(dot, active ? LV_OPA_COVER : LV_OPA_30, 0);
  lv_obj_set_style_border_width(dot, active ? 0 : 1, 0);
  lv_obj_set_style_border_color(dot, COLOR_RING, 0);
  lv_obj_set_size(dot, 10, 10);
}

static void style_action_button(lv_obj_t *button, lv_obj_t *label, bool primary) {
  if (button == NULL || label == NULL) {
    return;
  }

  lv_obj_set_style_bg_color(button, primary ? COLOR_ACTIVE : COLOR_BUTTON, 0);
  lv_obj_set_style_bg_opa(button, primary ? LV_OPA_90 : LV_OPA_70, 0);
  lv_obj_set_style_border_width(button, primary ? 0 : 1, 0);
  lv_obj_set_style_border_color(button, COLOR_RING, 0);
  lv_obj_set_style_shadow_width(button, primary ? 14 : 0, 0);
  lv_obj_set_style_shadow_color(button, COLOR_ACTIVE, 0);
  lv_obj_set_style_text_color(label, primary ? COLOR_BG : COLOR_TEXT, 0);
}

static void render_title(lm_ctrl_ui_t *ui, const lm_ctrl_ui_view_t *view) {
  if (ui == NULL || view == NULL) {
    return;
  }

  if (view->custom_logo != NULL) {
    lv_img_set_src(ui->title_image, view->custom_logo);
    set_hidden(ui->title_image, false);
    set_hidden(ui->title_text, true);
    return;
  }

  set_hidden(ui->title_image, true);
  set_hidden(ui->title_text, false);
  set_label_text(ui->title_text, TITLE_TEXT, COLOR_MUTED);
}

static bool is_focus_value_loaded(const ctrl_state_t *state, ctrl_focus_t focus) {
  if (state == NULL) {
    return false;
  }

  switch (focus) {
    case CTRL_FOCUS_TEMPERATURE:
      return (state->loaded_mask & LM_CTRL_MACHINE_FIELD_TEMPERATURE) != 0;
    case CTRL_FOCUS_INFUSE:
      return (state->loaded_mask & LM_CTRL_MACHINE_FIELD_INFUSE) != 0;
    case CTRL_FOCUS_PAUSE:
      return (state->loaded_mask & LM_CTRL_MACHINE_FIELD_PAUSE) != 0;
    case CTRL_FOCUS_STEAM:
      return (state->loaded_mask & LM_CTRL_MACHINE_FIELD_STEAM) != 0;
    case CTRL_FOCUS_STANDBY:
      return (state->loaded_mask & LM_CTRL_MACHINE_FIELD_STANDBY) != 0;
    case CTRL_FOCUS_BBW_MODE:
      return (state->loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_MODE) != 0;
    case CTRL_FOCUS_BBW_DOSE_1:
      return (state->loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0;
    case CTRL_FOCUS_BBW_DOSE_2:
      return (state->loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0;
    default:
      return false;
  }
}

static lm_ctrl_field_presentation_t focus_presentation(
  const ctrl_state_t *state,
  const lm_ctrl_ui_view_t *view,
  ctrl_focus_t focus
) {
  if (state == NULL || view == NULL) {
    return LM_CTRL_FIELD_PRESENTATION_LOADING;
  }

  return lm_ctrl_controller_field_presentation(view->readable_mask, state->loaded_mask, focus);
}

static bool focus_editable(const lm_ctrl_ui_view_t *view, ctrl_focus_t focus) {
  if (view == NULL) {
    return true;
  }

  return lm_ctrl_controller_field_is_editable(view->editable_mask, focus);
}

static void format_main_loading_placeholder(
  ctrl_focus_t focus,
  ctrl_language_t language,
  char *value,
  size_t value_size,
  char *hint,
  size_t hint_size
) {
  if (value != NULL && value_size > 0) {
    snprintf(value, value_size, "--");
  }

  if (hint == NULL || hint_size == 0) {
    return;
  }

  switch (focus) {
    case CTRL_FOCUS_INFUSE:
    case CTRL_FOCUS_PAUSE:
    case CTRL_FOCUS_BBW_MODE:
    case CTRL_FOCUS_BBW_DOSE_1:
    case CTRL_FOCUS_BBW_DOSE_2:
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "Werte werden geladen,\nsobald die Cloud bereit ist."
          : "Values load once\ncloud sync is ready."
      );
      break;
    default:
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "Werte werden geladen,\nsobald die Maschine verbunden ist."
          : "Values load once\nthe machine is connected."
      );
      break;
  }
}

static void format_main_unavailable_placeholder(
  ctrl_focus_t focus,
  ctrl_language_t language,
  char *value,
  size_t value_size,
  char *hint,
  size_t hint_size
) {
  if (value != NULL && value_size > 0) {
    snprintf(value, value_size, "--");
  }

  if (hint == NULL || hint_size == 0) {
    return;
  }

  switch (focus) {
    case CTRL_FOCUS_INFUSE:
    case CTRL_FOCUS_PAUSE:
    case CTRL_FOCUS_BBW_MODE:
    case CTRL_FOCUS_BBW_DOSE_1:
    case CTRL_FOCUS_BBW_DOSE_2:
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "Maschine offline.\nCloud-Werte nicht verfügbar."
          : "Machine offline.\nCloud values unavailable."
      );
      break;
    default:
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "Maschine nicht erreichbar.\nBLE oder Cloud verbinden."
          : "Machine unreachable.\nConnect via BLE or cloud."
      );
      break;
  }
}

static void format_main_value(
  const ctrl_state_t *state,
  const lm_ctrl_ui_view_t *view,
  ctrl_language_t language,
  char *value,
  size_t value_size,
  char *hint,
  size_t hint_size
) {
  switch (state->focus) {
    case CTRL_FOCUS_TEMPERATURE:
      snprintf(value, value_size, "%.1f C", state->values.temperature_c);
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "Die Kaffeeboiler-Einstellung regelt die Wassertemperatur für die Espresso-Zubereitung."
          : "The coffee boiler setting controls the brewing water temperature."
      );
      break;
    case CTRL_FOCUS_INFUSE:
      snprintf(value, value_size, "%.1f s", state->values.infuse_s);
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "An-Zeit: Dauer des Pumpenimpulses während Prebrewing."
          : "On-time: pump pulse duration during prebrewing."
      );
      break;
    case CTRL_FOCUS_PAUSE:
      snprintf(value, value_size, "%.1f s", state->values.pause_s);
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "Aus-Zeit: Pause zwischen zwei Pumpenimpulsen."
          : "Off-time: pause between prebrewing pump pulses."
      );
      break;
    case CTRL_FOCUS_STEAM:
      snprintf(
        value,
        value_size,
        "%s",
        ctrl_steam_level_label(state->values.steam_level)
      );
      if (view != NULL && view->heat_arc_visible && view->heat_eta_text[0] != '\0') {
        snprintf(
          hint,
          hint_size,
          language == CTRL_LANGUAGE_DE ? "Aufheizen läuft.\nBereit in %s." : "Heating up.\nReady in %s.",
          view->heat_eta_text
        );
      } else {
        snprintf(
          hint,
          hint_size,
          "%s",
          language == CTRL_LANGUAGE_DE
            ? "Dampflevel direkt\nam Controller einstellen."
            : "Adjust the steam level\ndirectly on the controller."
        );
      }
      break;
    case CTRL_FOCUS_STANDBY:
      snprintf(
        value,
        value_size,
        "%s",
        state->values.standby_on
          ? "Standby"
          : (language == CTRL_LANGUAGE_DE ? "An" : "On")
      );
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "Maschinenstatus direkt\nam Controller einstellen."
          : "Adjust machine status\ndirectly on the controller."
      );
      if (view != NULL && view->heat_arc_visible && view->heat_eta_text[0] != '\0') {
        snprintf(
          hint,
          hint_size,
          language == CTRL_LANGUAGE_DE ? "Aufheizen läuft.\nBereit in %s." : "Heating up.\nReady in %s.",
          view->heat_eta_text
        );
      }
      break;
    case CTRL_FOCUS_BBW_MODE:
      snprintf(value, value_size, "%s", ctrl_bbw_mode_name(state->values.bbw_mode, language));
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "Aktiven Brew-by-Weight\nModus wählen."
          : "Select the active\nbrew-by-weight mode."
      );
      break;
    case CTRL_FOCUS_BBW_DOSE_1:
      snprintf(value, value_size, "%.1f g", state->values.bbw_dose_1_g);
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "Zielgewicht für\nBBW Dosis 1."
          : "Target weight for\nBBW dose 1."
      );
      break;
    case CTRL_FOCUS_BBW_DOSE_2:
      snprintf(value, value_size, "%.1f g", state->values.bbw_dose_2_g);
      snprintf(
        hint,
        hint_size,
        "%s",
        language == CTRL_LANGUAGE_DE
          ? "Zielgewicht für\nBBW Dosis 2."
          : "Target weight for\nBBW dose 2."
      );
      break;
    default:
      if (value_size > 0) {
        value[0] = '\0';
      }
      if (hint_size > 0) {
        hint[0] = '\0';
      }
      break;
  }
}

static void format_preset_body(const ctrl_state_t *state, const ctrl_preset_t *preset, ctrl_language_t language, char *body, size_t body_size) {
  const ctrl_values_t *values = preset != NULL ? &preset->values : NULL;

  if (values == NULL) {
    if (body_size > 0) {
      body[0] = '\0';
    }
    return;
  }

  if ((state->feature_mask & CTRL_FEATURE_BBW) != 0) {
    snprintf(
      body,
      body_size,
      language == CTRL_LANGUAGE_DE
        ? "Kaffeeboiler %.1f C\nAn-Zeit %.1f s\nAus-Zeit %.1f s\nBBW %s\nDosis 1 %.1f g\nDosis 2 %.1f g"
        : "Coffee Boiler %.1f C\nOn-Time %.1f s\nOff-Time %.1f s\nBBW %s\nDose 1 %.1f g\nDose 2 %.1f g",
      values->temperature_c,
      values->infuse_s,
      values->pause_s,
      ctrl_bbw_mode_name(values->bbw_mode, language),
      values->bbw_dose_1_g,
      values->bbw_dose_2_g
    );
    return;
  }

  snprintf(
    body,
    body_size,
    language == CTRL_LANGUAGE_DE
      ? "Kaffeeboiler %.1f C\nAn-Zeit %.1f s\nAus-Zeit %.1f s"
      : "Coffee Boiler %.1f C\nOn-Time %.1f s\nOff-Time %.1f s",
    values->temperature_c,
    values->infuse_s,
    values->pause_s
  );
}

static void dispatch_action_direct(lm_ctrl_ui_t *ui, lm_ctrl_ui_action_t action, ctrl_focus_t focus) {
  if (ui == NULL || ui->action_cb == NULL) {
    return;
  }

  ui->action_cb(action, focus, ui->action_user_data);
}

static void dispatch_action(lv_event_t *event) {
  lm_ctrl_ui_binding_t *binding = lv_event_get_user_data(event);
  if (binding == NULL || binding->ui == NULL || binding->ui->action_cb == NULL) {
    return;
  }

  binding->ui->action_cb(binding->action, binding->focus, binding->ui->action_user_data);
}

static void handle_setup_long_press(lv_event_t *event) {
  lm_ctrl_ui_t *ui = lv_event_get_user_data(event);

  if (ui == NULL || ui->rendered_shot_timer_visible || ui->rendered_screen != CTRL_SCREEN_SETUP) {
    return;
  }

  dispatch_action_direct(ui, LM_CTRL_UI_ACTION_OPEN_SETUP_RESET, CTRL_FOCUS_TEMPERATURE);
}

static lv_obj_t *create_button(
  lv_obj_t *parent,
  int width,
  int height,
  int x,
  int y,
  lv_obj_t **out_label
) {
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_size(button, width, height);
  lv_obj_align(button, LV_ALIGN_CENTER, x, y);
  lv_obj_set_style_radius(button, 18, 0);
  lv_obj_set_style_pad_all(button, 10, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_t *label = lv_label_create(button);
  lv_obj_center(label);
  lv_obj_set_style_text_font(label, UI_FONT_16, 0);
  lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
  if (out_label != NULL) {
    *out_label = label;
  }

  return button;
}

static lv_obj_t *create_panel(lv_obj_t *parent, int width, int height) {
  lv_obj_t *panel = lv_obj_create(parent);
  lv_obj_set_size(panel, width, height);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, -8);
  lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_radius(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_set_style_shadow_width(panel, 0, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
  return panel;
}

static void bind_button(lm_ctrl_ui_t *ui, size_t index, lv_obj_t *button, lm_ctrl_ui_action_t action, ctrl_focus_t focus) {
  ui->bindings[index] = (lm_ctrl_ui_binding_t){
    .ui = ui,
    .action = action,
    .focus = focus,
  };
  lv_obj_add_event_cb(button, dispatch_action, LV_EVENT_CLICKED, &ui->bindings[index]);
}

static void dispatch_focus_change(lm_ctrl_ui_t *ui, int delta) {
  if (ui == NULL || ui->action_cb == NULL || ui->rendered_screen != CTRL_SCREEN_MAIN) {
    return;
  }

  dispatch_action_direct(
    ui,
    LM_CTRL_UI_ACTION_SELECT_FOCUS,
    focus_from_page_index(ui->rendered_feature_mask, main_page_index(ui->rendered_feature_mask, ui->rendered_focus) + delta)
  );
}

static void handle_screen_gesture(lv_event_t *event) {
  lm_ctrl_ui_t *ui = lv_event_get_user_data(event);
  lv_indev_t *indev = lv_indev_get_act();
  lv_dir_t dir;

  if (ui == NULL || indev == NULL) {
    return;
  }

  dir = lv_indev_get_gesture_dir(indev);
  if (ui->rendered_shot_timer_visible) {
    if (ui->rendered_shot_timer_dismissable &&
        (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT || dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM)) {
      dispatch_action_direct(ui, LM_CTRL_UI_ACTION_DISMISS_SHOT_TIMER, CTRL_FOCUS_TEMPERATURE);
      lv_indev_wait_release(indev);
    }
    return;
  }

  switch (ui->rendered_screen) {
    case CTRL_SCREEN_MAIN:
      if (dir == LV_DIR_LEFT) {
        dispatch_focus_change(ui, +1);
      } else if (dir == LV_DIR_RIGHT) {
        dispatch_focus_change(ui, -1);
      } else if (dir == LV_DIR_TOP) {
        dispatch_action_direct(ui, LM_CTRL_UI_ACTION_OPEN_SETUP, CTRL_FOCUS_TEMPERATURE);
      } else if (dir == LV_DIR_BOTTOM) {
        dispatch_action_direct(ui, LM_CTRL_UI_ACTION_OPEN_PRESETS, CTRL_FOCUS_TEMPERATURE);
      } else {
        return;
      }
      break;
    case CTRL_SCREEN_PRESETS:
      if (dir != LV_DIR_TOP) {
        return;
      }
      dispatch_action_direct(ui, LM_CTRL_UI_ACTION_CLOSE_SCREEN, CTRL_FOCUS_TEMPERATURE);
      break;
    case CTRL_SCREEN_SETUP:
      if (dir != LV_DIR_BOTTOM) {
        return;
      }
      dispatch_action_direct(ui, LM_CTRL_UI_ACTION_CLOSE_SCREEN, CTRL_FOCUS_TEMPERATURE);
      break;
    case CTRL_SCREEN_SETUP_RESET_ARM:
    case CTRL_SCREEN_SETUP_RESET_CONFIRM:
      if (dir != LV_DIR_BOTTOM) {
        return;
      }
      dispatch_action_direct(ui, LM_CTRL_UI_ACTION_CANCEL_SETUP_RESET, CTRL_FOCUS_TEMPERATURE);
      break;
    default:
      return;
  }

  lv_indev_wait_release(indev);
}

static void render_main_screen(
  lm_ctrl_ui_t *ui,
  const ctrl_state_t *state,
  const lm_ctrl_ui_view_t *view,
  ctrl_language_t language
) {
  char value[40];
  char hint[128];
  lm_ctrl_field_presentation_t presentation;
  lv_color_t title_color = COLOR_ACTIVE;
  lv_color_t value_color = COLOR_TEXT;
  bool has_hint;
  const int page_index = main_page_index(state->feature_mask, state->focus);
  const size_t page_count = main_page_count(state->feature_mask);

  set_hidden(ui->main_card, false);
  set_hidden(ui->presets_card, true);
  set_hidden(ui->setup_card, true);
  set_hidden(ui->shot_timer_card, true);
  set_hidden(ui->setup_reset_arc, true);
  set_hidden(ui->heat_arc, view == NULL || !view->heat_arc_visible);
  set_hidden(ui->setup_secondary_button, true);
  set_hidden(ui->setup_primary_button, true);
  set_hidden(ui->focus, false);
  set_hidden(ui->value, false);
  set_hidden(ui->hint, false);
  set_hidden(ui->page_label, true);
  set_hidden(ui->power_left_button, true);
  set_hidden(ui->power_right_button, true);
  set_hidden(ui->power_hint, true);

  presentation = focus_presentation(state, view, state->focus);
  if (presentation == LM_CTRL_FIELD_PRESENTATION_UNAVAILABLE || !focus_editable(view, state->focus)) {
    title_color = COLOR_MUTED;
  }
  if (presentation != LM_CTRL_FIELD_PRESENTATION_READY) {
    value_color = COLOR_MUTED;
  }
  set_label_text(ui->focus, focus_title(state->focus, language), title_color);

  if (presentation == LM_CTRL_FIELD_PRESENTATION_READY && is_focus_value_loaded(state, state->focus)) {
    format_main_value(state, view, language, value, sizeof(value), hint, sizeof(hint));
  } else if (presentation == LM_CTRL_FIELD_PRESENTATION_LOADING) {
    format_main_loading_placeholder(state->focus, language, value, sizeof(value), hint, sizeof(hint));
  } else {
    format_main_unavailable_placeholder(state->focus, language, value, sizeof(value), hint, sizeof(hint));
  }
  if (view != NULL && view->heat_arc_visible) {
    lv_arc_set_value(ui->heat_arc, view->heat_progress_permille);
  }
  has_hint = hint[0] != '\0';
  set_label_text(ui->value, value, value_color);
  set_hidden(ui->hint, !has_hint);
  if (has_hint) {
    set_label_text(ui->hint, hint, COLOR_MUTED);
  }

  for (size_t i = 0; i < LM_CTRL_UI_MAIN_PAGE_COUNT; ++i) {
    int x_offset = 0;

    if (i >= page_count) {
      set_hidden(ui->page_dots[i], true);
      continue;
    }

    x_offset = (int)((int)i * 17) - (int)(((int)page_count - 1) * 17 / 2);
    lv_obj_align(ui->page_dots[i], LV_ALIGN_CENTER, x_offset, 106);
    set_hidden(ui->page_dots[i], false);
    style_page_dot(ui->page_dots[i], (int)i == page_index);
  }

}

static void render_shot_timer_screen(lm_ctrl_ui_t *ui, const lm_ctrl_ui_view_t *view) {
  const ctrl_language_t language = view != NULL ? view->language : CTRL_LANGUAGE_EN;

  if (ui == NULL || view == NULL) {
    return;
  }

  set_hidden(ui->main_card, true);
  set_hidden(ui->presets_card, true);
  set_hidden(ui->setup_card, true);
  set_hidden(ui->shot_timer_card, false);
  set_hidden(ui->setup_reset_arc, true);
  set_hidden(ui->heat_arc, true);
  set_hidden(ui->page_label, true);
  set_hidden(ui->setup_secondary_button, true);
  set_hidden(ui->setup_primary_button, true);
  set_hidden(ui->power_left_button, true);
  set_hidden(ui->power_right_button, true);
  set_hidden(ui->power_hint, true);
  for (size_t i = 0; i < LM_CTRL_UI_MAIN_PAGE_COUNT; ++i) {
    set_hidden(ui->page_dots[i], true);
  }

  set_label_text(ui->shot_timer_title, language == CTRL_LANGUAGE_DE ? "Shot-Timer" : "Shot Timer", COLOR_ACTIVE);
  set_label_text(ui->shot_timer_value, view->shot_timer_text, COLOR_TEXT);
}

static void render_connection_icons(lm_ctrl_ui_t *ui, const lm_ctrl_ui_view_t *view) {
  typedef struct {
    lv_obj_t *obj;
    const char *symbol;
    lv_color_t color;
  } icon_slot_t;
  icon_slot_t slots[5];
  lm_ctrl_indicator_state_t wifi_indicator = LM_CTRL_INDICATOR_HIDDEN;
  bool wifi_crossed = false;
  size_t visible_count = 0;
  const int step = 28;
  const int y = 48;

  if (ui == NULL || view == NULL) {
    return;
  }

  wifi_indicator = lm_ctrl_remote_path_indicator_state(view->remote_path_state);

  set_hidden(ui->wifi_icon, true);
  set_hidden(ui->wifi_slash_icon, true);
  set_hidden(ui->usb_icon, true);
  set_hidden(ui->battery_icon, true);
  set_hidden(ui->heat_icon, true);
  set_hidden(ui->ble_icon, true);

  switch (wifi_indicator) {
    case LM_CTRL_INDICATOR_CROSSED:
      slots[visible_count++] = (icon_slot_t){
        .obj = ui->wifi_icon,
        .symbol = LV_SYMBOL_WIFI,
        .color = COLOR_MUTED,
      };
      wifi_crossed = true;
      break;
    case LM_CTRL_INDICATOR_ACTIVE:
      slots[visible_count++] = (icon_slot_t){
        .obj = ui->wifi_icon,
        .symbol = LV_SYMBOL_WIFI,
        .color = COLOR_ACTIVE,
      };
      break;
    case LM_CTRL_INDICATOR_HIDDEN:
    default:
      break;
  }

  if (view->heat_visible) {
    slots[visible_count++] = (icon_slot_t){
      .obj = ui->heat_icon,
      .symbol = LV_SYMBOL_CHARGE,
      .color = COLOR_ACTIVE,
    };
  }

  if (view->ble_visible) {
    slots[visible_count++] = (icon_slot_t){
      .obj = ui->ble_icon,
      .symbol = LV_SYMBOL_BLUETOOTH,
      .color = view->ble_authenticated ? COLOR_ACTIVE : COLOR_MUTED,
    };
  }

  if (view->usb_visible) {
    slots[visible_count++] = (icon_slot_t){
      .obj = ui->usb_icon,
      .symbol = LV_SYMBOL_USB,
      .color = COLOR_ACTIVE,
    };
  }

  if (view->battery_visible) {
    slots[visible_count++] = (icon_slot_t){
      .obj = ui->battery_icon,
      .symbol = view->battery_charging ? BATTERY_CHARGING_SYMBOL : LV_SYMBOL_BATTERY_EMPTY,
      .color = COLOR_ACTIVE,
    };
  }

  for (size_t i = 0; i < visible_count; ++i) {
    const int x = ((int)i * step) - (((int)visible_count - 1) * step / 2);
    set_hidden(slots[i].obj, false);
    set_label_text(slots[i].obj, slots[i].symbol, slots[i].color);
    lv_obj_align(slots[i].obj, LV_ALIGN_TOP_MID, x, y);
    if (wifi_crossed && slots[i].obj == ui->wifi_icon) {
      set_hidden(ui->wifi_slash_icon, false);
      set_label_text(ui->wifi_slash_icon, ICON_SLASH_SYMBOL, COLOR_MUTED);
      lv_obj_align(ui->wifi_slash_icon, LV_ALIGN_TOP_MID, x + 1, y - 1);
    }
  }
}

static void style_disabled_button(lv_obj_t *button, lv_obj_t *label) {
  if (button == NULL || label == NULL) {
    return;
  }

  lv_obj_set_style_bg_color(button, COLOR_BUTTON, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_40, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, COLOR_RING, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_text_color(label, COLOR_MUTED, 0);
}

static void render_presets_screen(
  lm_ctrl_ui_t *ui,
  const ctrl_state_t *state,
  const lm_ctrl_ui_view_t *view,
  ctrl_language_t language
) {
  char body[160];
  char preset_name[CTRL_PRESET_NAME_LEN];
  char title[32];
  const ctrl_preset_t *preset = &state->presets[state->preset_index];
  const bool preset_load_enabled = view == NULL || view->preset_load_enabled;

  set_hidden(ui->main_card, true);
  set_hidden(ui->presets_card, false);
  set_hidden(ui->setup_card, true);
  set_hidden(ui->shot_timer_card, true);
  set_hidden(ui->setup_reset_arc, true);
  set_hidden(ui->heat_arc, true);
  set_hidden(ui->setup_secondary_button, true);
  set_hidden(ui->setup_primary_button, true);
  for (size_t i = 0; i < LM_CTRL_UI_MAIN_PAGE_COUNT; ++i) {
    set_hidden(ui->page_dots[i], true);
  }

  set_hidden(ui->page_label, true);
  snprintf(title, sizeof(title), "Preset %u/%u", (unsigned int)(state->preset_index + 1U), (unsigned int)state->preset_count);
  set_label_text(ui->presets_title, title, COLOR_ACTIVE);
  ctrl_preset_display_name(preset, (int)state->preset_index, preset_name, sizeof(preset_name));
  set_label_text(ui->presets_name, preset_name, COLOR_MUTED);
  format_preset_body(state, preset, language, body, sizeof(body));
  set_label_text(ui->presets_body, body, COLOR_TEXT);
  set_label_text(
    ui->presets_load_label,
    language == CTRL_LANGUAGE_DE ? "Laden" : "Load",
    preset_load_enabled ? COLOR_BG : COLOR_MUTED
  );
  set_label_text(ui->presets_save_label, language == CTRL_LANGUAGE_DE ? "Sichern" : "Save", COLOR_TEXT);
  if (preset_load_enabled) {
    lv_obj_clear_state(ui->presets_load_button, LV_STATE_DISABLED);
    style_action_button(ui->presets_load_button, ui->presets_load_label, true);
  } else {
    lv_obj_add_state(ui->presets_load_button, LV_STATE_DISABLED);
    style_disabled_button(ui->presets_load_button, ui->presets_load_label);
  }
  style_action_button(ui->presets_save_button, ui->presets_save_label, false);
}

static void render_setup_screen(lm_ctrl_ui_t *ui, const ctrl_state_t *state, const lm_ctrl_ui_view_t *view) {
  const ctrl_language_t language = view != NULL ? view->language : CTRL_LANGUAGE_EN;
  const char *body = (view != NULL && view->setup_status_text[0] != '\0')
    ? view->setup_status_text
    : (language == CTRL_LANGUAGE_DE ? "Setup-Portal wird gestartet." : "Setup portal is starting.");
  char reset_body[160];
  char recovery_actions[96];
  const int reset_progress = state != NULL ? (int)state->reset_progress : 0;
  const int reset_progress_pct = (reset_progress * 100) / 24;

  set_hidden(ui->main_card, true);
  set_hidden(ui->presets_card, true);
  set_hidden(ui->setup_card, false);
  set_hidden(ui->shot_timer_card, true);
  set_hidden(ui->heat_arc, true);
  for (size_t i = 0; i < LM_CTRL_UI_MAIN_PAGE_COUNT; ++i) {
    set_hidden(ui->page_dots[i], true);
  }

  set_hidden(ui->page_label, true);
  set_hidden(ui->setup_reset_arc, !is_setup_reset_screen(state->screen));
  set_hidden(ui->setup_secondary_button, state->screen != CTRL_SCREEN_SETUP_RESET_CONFIRM);
  set_hidden(ui->setup_primary_button, state->screen != CTRL_SCREEN_SETUP_RESET_CONFIRM);
  set_hidden(ui->setup_action_list, state->screen != CTRL_SCREEN_SETUP_RESET_CONFIRM);

  if (state->screen == CTRL_SCREEN_SETUP_RESET_ARM) {
    lv_obj_align(ui->setup_title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_align(ui->setup_body, LV_ALIGN_BOTTOM_MID, 0, -6);
    set_label_text(ui->setup_title, language == CTRL_LANGUAGE_DE ? "Zurücksetzen" : "Reset", COLOR_ACTIVE);
    snprintf(
      reset_body,
      sizeof(reset_body),
      "%s\n\n%s",
      language == CTRL_LANGUAGE_DE
        ? "Einmal im Uhrzeigersinn drehen, um die Wiederherstellung zu öffnen."
        : "Rotate clockwise once to open recovery.",
      language == CTRL_LANGUAGE_DE
        ? "Nach unten wischen zum Abbrechen."
        : "Swipe down to cancel."
    );
    set_label_text(ui->setup_body, reset_body, COLOR_TEXT);
    set_label_text(ui->setup_action_list, "", COLOR_TEXT);
    lv_arc_set_value(ui->setup_reset_arc, reset_progress_pct);
    set_hidden(ui->setup_qr, true);
    return;
  }

  if (state->screen == CTRL_SCREEN_SETUP_RESET_CONFIRM) {
    lv_obj_align(ui->setup_title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_align(ui->setup_body, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_align(ui->setup_action_list, LV_ALIGN_TOP_MID, 0, 102);
    lv_obj_align(ui->setup_secondary_button, LV_ALIGN_CENTER, -50, 74);
    lv_obj_align(ui->setup_primary_button, LV_ALIGN_CENTER, 50, 74);
    set_label_text(ui->setup_title, language == CTRL_LANGUAGE_DE ? "Wiederherstellung" : "Recovery", COLOR_ACTIVE);
    snprintf(
      recovery_actions,
      sizeof(recovery_actions),
      "%s %s\n%s %s",
      state->recovery_action == CTRL_RECOVERY_ACTION_CLEAR_WEB_PASSWORD ? ">" : " ",
      language == CTRL_LANGUAGE_DE ? "Web-Passwort löschen" : "Clear web password",
      state->recovery_action == CTRL_RECOVERY_ACTION_RESET_NETWORK ? ">" : " ",
      language == CTRL_LANGUAGE_DE ? "Netzwerk zurücksetzen" : "Reset network"
    );
    set_label_text(
      ui->setup_body,
      language == CTRL_LANGUAGE_DE
        ? "Aktion mit dem Drehknopf wählen."
        : "Rotate to choose the recovery action.",
      COLOR_TEXT
    );
    set_label_text(ui->setup_action_list, recovery_actions, COLOR_TEXT);
    set_label_text(ui->setup_secondary_label, language == CTRL_LANGUAGE_DE ? "Zurück" : "Back", COLOR_TEXT);
    set_label_text(ui->setup_primary_label, language == CTRL_LANGUAGE_DE ? "Start" : "Run", COLOR_BG);
    style_action_button(ui->setup_secondary_button, ui->setup_secondary_label, false);
    style_action_button(ui->setup_primary_button, ui->setup_primary_label, true);
    lv_arc_set_value(ui->setup_reset_arc, 100);
    set_hidden(ui->setup_qr, true);
    return;
  }

  lv_obj_align(ui->setup_title, LV_ALIGN_TOP_MID, 0, 24);
  lv_obj_align(ui->setup_qr, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_align_to(ui->setup_body, ui->setup_qr, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
  set_label_text(ui->setup_title, "Setup", COLOR_ACTIVE);
  set_label_text(ui->setup_body, body, COLOR_TEXT);
  set_label_text(ui->setup_action_list, "", COLOR_TEXT);

  if (view != NULL &&
      view->setup_qr_payload[0] != '\0' &&
      lv_qrcode_update(ui->setup_qr, view->setup_qr_payload, strlen(view->setup_qr_payload)) == LV_RES_OK) {
    set_hidden(ui->setup_qr, false);
  } else {
    set_hidden(ui->setup_qr, true);
  }
}

esp_err_t lm_ctrl_ui_init(
  lm_ctrl_ui_t *ui,
  const ctrl_state_t *state,
  const lm_ctrl_ui_view_t *view,
  lm_ctrl_ui_action_cb_t action_cb,
  void *action_user_data
) {
  if (ui == NULL || state == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  ui->action_cb = action_cb;
  ui->action_user_data = action_user_data;
  ui->rendered_focus = state->focus;
  ui->rendered_screen = state->screen;
  ui->rendered_feature_mask = state->feature_mask;

  ui->screen = lv_obj_create(NULL);
  lv_obj_remove_style_all(ui->screen);
  lv_obj_set_size(ui->screen, 360, 360);
  lv_obj_set_style_bg_color(ui->screen, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(ui->screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(ui->screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(ui->screen, handle_screen_gesture, LV_EVENT_GESTURE, ui);
  lv_scr_load(ui->screen);

  ui->ring = lv_obj_create(ui->screen);
  lv_obj_remove_style_all(ui->ring);
  lv_obj_set_size(ui->ring, 320, 320);
  lv_obj_center(ui->ring);
  lv_obj_set_style_radius(ui->ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(ui->ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ui->ring, 2, 0);
  lv_obj_set_style_border_color(ui->ring, COLOR_RING, 0);
  lv_obj_set_style_border_opa(ui->ring, LV_OPA_30, 0);

  ui->title_text = lv_label_create(ui->screen);
  lv_obj_set_width(ui->title_text, 170);
  lv_obj_set_style_text_font(ui->title_text, UI_FONT_16, 0);
  lv_obj_set_style_text_letter_space(ui->title_text, 2, 0);
  lv_obj_set_style_text_align(ui->title_text, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ui->title_text, LV_ALIGN_TOP_MID, 0, 18);

  ui->title_image = lv_img_create(ui->screen);
  lv_obj_align(ui->title_image, LV_ALIGN_TOP_MID, 0, 14);
  set_hidden(ui->title_image, true);

  ui->wifi_icon = lv_label_create(ui->screen);
  lv_obj_set_style_text_font(ui->wifi_icon, UI_FONT_14, 0);
  lv_obj_align(ui->wifi_icon, LV_ALIGN_TOP_MID, -68, 48);

  ui->wifi_slash_icon = lv_label_create(ui->screen);
  lv_obj_set_style_text_font(ui->wifi_slash_icon, UI_FONT_20, 0);
  lv_obj_align(ui->wifi_slash_icon, LV_ALIGN_TOP_MID, -68, 47);
  set_hidden(ui->wifi_slash_icon, true);

  ui->usb_icon = lv_label_create(ui->screen);
  lv_obj_set_style_text_font(ui->usb_icon, UI_FONT_14, 0);
  lv_obj_align(ui->usb_icon, LV_ALIGN_TOP_MID, 34, 48);
  set_hidden(ui->usb_icon, true);

  ui->battery_icon = lv_label_create(ui->screen);
  lv_obj_set_style_text_font(ui->battery_icon, UI_FONT_14, 0);
  lv_obj_align(ui->battery_icon, LV_ALIGN_TOP_MID, 68, 48);
  set_hidden(ui->battery_icon, true);

  ui->heat_icon = lv_label_create(ui->screen);
  lv_obj_set_style_text_font(ui->heat_icon, UI_FONT_14, 0);
  lv_obj_align(ui->heat_icon, LV_ALIGN_TOP_MID, -34, 48);
  set_hidden(ui->heat_icon, true);

  ui->ble_icon = lv_label_create(ui->screen);
  lv_obj_set_style_text_font(ui->ble_icon, UI_FONT_14, 0);
  lv_obj_align(ui->ble_icon, LV_ALIGN_TOP_MID, 0, 48);

  ui->page_label = lv_label_create(ui->screen);
  lv_obj_set_style_text_font(ui->page_label, UI_FONT_14, 0);
  lv_obj_align(ui->page_label, LV_ALIGN_TOP_MID, 0, 46);

  ui->heat_arc = lv_arc_create(ui->screen);
  lv_obj_set_size(ui->heat_arc, 328, 328);
  lv_obj_center(ui->heat_arc);
  lv_arc_set_rotation(ui->heat_arc, 270);
  lv_arc_set_bg_angles(ui->heat_arc, 0, 360);
  lv_arc_set_mode(ui->heat_arc, LV_ARC_MODE_REVERSE);
  lv_arc_set_range(ui->heat_arc, 0, 1000);
  lv_arc_set_value(ui->heat_arc, 1000);
  lv_obj_set_style_arc_width(ui->heat_arc, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ui->heat_arc, COLOR_ACTIVE, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(ui->heat_arc, 1, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ui->heat_arc, COLOR_RING, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(ui->heat_arc, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ui->heat_arc, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui->heat_arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_clear_flag(ui->heat_arc, LV_OBJ_FLAG_CLICKABLE);
  set_hidden(ui->heat_arc, true);

  ui->main_card = create_panel(ui->screen, 280, 192);

  ui->focus = lv_label_create(ui->main_card);
  lv_obj_set_style_text_font(ui->focus, UI_FONT_20, 0);
  lv_obj_set_style_text_align(ui->focus, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(ui->focus, 230);
  lv_obj_align(ui->focus, LV_ALIGN_TOP_MID, 0, 22);
  lv_obj_add_flag(ui->focus, LV_OBJ_FLAG_GESTURE_BUBBLE);

  ui->value = lv_label_create(ui->main_card);
  lv_obj_set_style_text_font(ui->value, UI_FONT_40, 0);
  lv_obj_align(ui->value, LV_ALIGN_CENTER, 0, -6);
  lv_obj_add_flag(ui->value, LV_OBJ_FLAG_GESTURE_BUBBLE);

  ui->hint = lv_label_create(ui->main_card);
  lv_obj_set_width(ui->hint, 226);
  lv_label_set_long_mode(ui->hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(ui->hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(ui->hint, UI_FONT_14, 0);
  lv_obj_align(ui->hint, LV_ALIGN_BOTTOM_MID, 0, -18);
  lv_obj_add_flag(ui->hint, LV_OBJ_FLAG_GESTURE_BUBBLE);

  ui->shot_timer_card = create_panel(ui->screen, 280, 192);
  ui->shot_timer_title = lv_label_create(ui->shot_timer_card);
  lv_obj_set_width(ui->shot_timer_title, 230);
  lv_obj_set_style_text_font(ui->shot_timer_title, UI_FONT_20, 0);
  lv_obj_set_style_text_align(ui->shot_timer_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ui->shot_timer_title, LV_ALIGN_TOP_MID, 0, 30);
  ui->shot_timer_value = lv_label_create(ui->shot_timer_card);
  lv_obj_set_style_text_font(ui->shot_timer_value, UI_FONT_40, 0);
  lv_obj_set_style_text_align(ui->shot_timer_value, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ui->shot_timer_value, LV_ALIGN_CENTER, 0, -6);

  ui->power_left_button = lv_btn_create(ui->main_card);
  lv_obj_set_size(ui->power_left_button, 110, 88);
  lv_obj_align(ui->power_left_button, LV_ALIGN_CENTER, -60, -6);
  lv_obj_set_style_radius(ui->power_left_button, 22, 0);
  lv_obj_set_style_pad_all(ui->power_left_button, 10, 0);
  lv_obj_add_flag(ui->power_left_button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  bind_button(ui, BIND_POWER_STEAM, ui->power_left_button, LM_CTRL_UI_ACTION_TOGGLE_FOCUS, CTRL_FOCUS_STEAM);

  ui->power_left_title = lv_label_create(ui->power_left_button);
  lv_obj_set_style_text_font(ui->power_left_title, UI_FONT_16, 0);
  lv_obj_align(ui->power_left_title, LV_ALIGN_TOP_MID, 0, 12);
  ui->power_left_value = lv_label_create(ui->power_left_button);
  lv_obj_set_style_text_font(ui->power_left_value, UI_FONT_28, 0);
  lv_obj_align(ui->power_left_value, LV_ALIGN_BOTTOM_MID, 0, -12);

  ui->power_right_button = lv_btn_create(ui->main_card);
  lv_obj_set_size(ui->power_right_button, 110, 88);
  lv_obj_align(ui->power_right_button, LV_ALIGN_CENTER, 60, -6);
  lv_obj_set_style_radius(ui->power_right_button, 22, 0);
  lv_obj_set_style_pad_all(ui->power_right_button, 10, 0);
  lv_obj_add_flag(ui->power_right_button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  bind_button(ui, BIND_POWER_STANDBY, ui->power_right_button, LM_CTRL_UI_ACTION_TOGGLE_FOCUS, CTRL_FOCUS_STANDBY);

  ui->power_right_title = lv_label_create(ui->power_right_button);
  lv_obj_set_style_text_font(ui->power_right_title, UI_FONT_16, 0);
  lv_obj_align(ui->power_right_title, LV_ALIGN_TOP_MID, 0, 12);
  ui->power_right_value = lv_label_create(ui->power_right_button);
  lv_obj_set_style_text_font(ui->power_right_value, UI_FONT_28, 0);
  lv_obj_align(ui->power_right_value, LV_ALIGN_BOTTOM_MID, 0, -12);

  ui->power_hint = lv_label_create(ui->main_card);
  lv_obj_set_width(ui->power_hint, 228);
  lv_label_set_long_mode(ui->power_hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(ui->power_hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(ui->power_hint, UI_FONT_14, 0);
  lv_obj_align(ui->power_hint, LV_ALIGN_BOTTOM_MID, 0, -14);

  ui->presets_card = create_panel(ui->screen, 282, 196);
  ui->presets_title = lv_label_create(ui->presets_card);
  lv_obj_set_style_text_font(ui->presets_title, UI_FONT_20, 0);
  lv_obj_align(ui->presets_title, LV_ALIGN_TOP_MID, 0, 14);

  ui->presets_name = lv_label_create(ui->presets_card);
  lv_obj_set_width(ui->presets_name, 230);
  lv_label_set_long_mode(ui->presets_name, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(ui->presets_name, UI_FONT_14, 0);
  lv_obj_set_style_text_align(ui->presets_name, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ui->presets_name, LV_ALIGN_TOP_MID, 0, 40);

  ui->presets_body = lv_label_create(ui->presets_card);
  lv_obj_set_width(ui->presets_body, 226);
  lv_label_set_long_mode(ui->presets_body, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(ui->presets_body, UI_FONT_16, 0);
  lv_obj_set_style_text_align(ui->presets_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ui->presets_body, LV_ALIGN_TOP_MID, 0, 68);

  ui->presets_load_button = create_button(ui->presets_card, 74, 36, -44, 66, &ui->presets_load_label);
  ui->presets_save_button = create_button(ui->presets_card, 74, 36, 44, 66, &ui->presets_save_label);
  bind_button(ui, BIND_PRESET_LOAD, ui->presets_load_button, LM_CTRL_UI_ACTION_LOAD_PRESET, CTRL_FOCUS_TEMPERATURE);
  bind_button(ui, BIND_PRESET_SAVE, ui->presets_save_button, LM_CTRL_UI_ACTION_SAVE_PRESET, CTRL_FOCUS_TEMPERATURE);
  set_label_text(ui->presets_load_label, "Load", COLOR_BG);
  set_label_text(ui->presets_save_label, "Save", COLOR_TEXT);

  ui->setup_card = create_panel(ui->screen, 282, 252);
  lv_obj_add_event_cb(ui->setup_card, handle_setup_long_press, LV_EVENT_LONG_PRESSED, ui);
  ui->setup_title = lv_label_create(ui->setup_card);
  lv_obj_set_style_text_font(ui->setup_title, UI_FONT_20, 0);
  lv_obj_align(ui->setup_title, LV_ALIGN_TOP_MID, 0, 24);

  ui->setup_qr = lv_qrcode_create(ui->setup_card, 112, COLOR_BG, lv_color_white());
  lv_obj_align(ui->setup_qr, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_style_border_width(ui->setup_qr, 8, 0);
  lv_obj_set_style_border_color(ui->setup_qr, lv_color_white(), 0);
  lv_obj_set_style_border_opa(ui->setup_qr, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(ui->setup_qr, 12, 0);
  lv_obj_add_event_cb(ui->setup_qr, handle_setup_long_press, LV_EVENT_LONG_PRESSED, ui);

  ui->setup_body = lv_label_create(ui->setup_card);
  lv_obj_set_width(ui->setup_body, 236);
  lv_label_set_long_mode(ui->setup_body, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(ui->setup_body, UI_FONT_14, 0);
  lv_obj_set_style_text_align(ui->setup_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(ui->setup_body, ui->setup_qr, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
  lv_obj_add_event_cb(ui->setup_body, handle_setup_long_press, LV_EVENT_LONG_PRESSED, ui);

  ui->setup_action_list = lv_label_create(ui->setup_card);
  lv_obj_set_width(ui->setup_action_list, 236);
  lv_label_set_long_mode(ui->setup_action_list, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(ui->setup_action_list, UI_FONT_14, 0);
  lv_obj_set_style_text_align(ui->setup_action_list, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ui->setup_action_list, LV_ALIGN_TOP_MID, 0, 102);
  lv_obj_add_event_cb(ui->setup_action_list, handle_setup_long_press, LV_EVENT_LONG_PRESSED, ui);
  set_hidden(ui->setup_action_list, true);

  ui->setup_reset_arc = lv_arc_create(ui->screen);
  lv_obj_set_size(ui->setup_reset_arc, 314, 314);
  lv_obj_center(ui->setup_reset_arc);
  lv_arc_set_rotation(ui->setup_reset_arc, 270);
  lv_arc_set_bg_angles(ui->setup_reset_arc, 0, 360);
  lv_arc_set_range(ui->setup_reset_arc, 0, 100);
  lv_arc_set_value(ui->setup_reset_arc, 0);
  lv_obj_set_style_arc_width(ui->setup_reset_arc, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ui->setup_reset_arc, COLOR_ACTIVE, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(ui->setup_reset_arc, 2, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ui->setup_reset_arc, COLOR_RING, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(ui->setup_reset_arc, LV_OPA_40, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ui->setup_reset_arc, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui->setup_reset_arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_clear_flag(ui->setup_reset_arc, LV_OBJ_FLAG_CLICKABLE);
  set_hidden(ui->setup_reset_arc, true);

  ui->setup_secondary_button = create_button(ui->setup_card, 84, 38, -50, 86, &ui->setup_secondary_label);
  ui->setup_primary_button = create_button(ui->setup_card, 84, 38, 50, 86, &ui->setup_primary_label);
  bind_button(ui, BIND_SETUP_RESET_CANCEL, ui->setup_secondary_button, LM_CTRL_UI_ACTION_CANCEL_SETUP_RESET, CTRL_FOCUS_TEMPERATURE);
  bind_button(ui, BIND_SETUP_RESET_CONFIRM, ui->setup_primary_button, LM_CTRL_UI_ACTION_CONFIRM_SETUP_RESET, CTRL_FOCUS_TEMPERATURE);
  set_hidden(ui->setup_secondary_button, true);
  set_hidden(ui->setup_primary_button, true);

  for (size_t i = 0; i < LM_CTRL_UI_MAIN_PAGE_COUNT; ++i) {
    ui->page_dots[i] = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->page_dots[i]);
    lv_obj_set_style_radius(ui->page_dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_align(ui->page_dots[i], LV_ALIGN_CENTER, -26 + ((int)i * 17), 106);
    lv_obj_move_foreground(ui->page_dots[i]);
  }

  lv_obj_move_foreground(ui->title_text);
  lv_obj_move_foreground(ui->title_image);
  lv_obj_move_foreground(ui->wifi_icon);
  lv_obj_move_foreground(ui->wifi_slash_icon);
  lv_obj_move_foreground(ui->usb_icon);
  lv_obj_move_foreground(ui->battery_icon);
  lv_obj_move_foreground(ui->heat_icon);
  lv_obj_move_foreground(ui->ble_icon);
  lv_obj_move_foreground(ui->page_label);

  lm_ctrl_ui_render(ui, state, view);
  ESP_LOGI(TAG, "UI initialized");
  return ESP_OK;
}

void lm_ctrl_ui_render(lm_ctrl_ui_t *ui, const ctrl_state_t *state, const lm_ctrl_ui_view_t *view) {
  const ctrl_language_t language = view != NULL ? view->language : CTRL_LANGUAGE_EN;

  if (ui == NULL || state == NULL) {
    return;
  }

  ui->rendered_focus = state->focus;
  ui->rendered_screen = state->screen;
  ui->rendered_feature_mask = state->feature_mask;
  ui->rendered_shot_timer_visible = view != NULL && view->shot_timer_visible;
  ui->rendered_shot_timer_dismissable = view != NULL && view->shot_timer_dismissable;
  render_title(ui, view);

  if (ui->rendered_shot_timer_visible) {
    render_shot_timer_screen(ui, view);
    render_connection_icons(ui, view);
    return;
  }

  switch (state->screen) {
    case CTRL_SCREEN_PRESETS:
      render_presets_screen(ui, state, view, language);
      break;
    case CTRL_SCREEN_SETUP:
    case CTRL_SCREEN_SETUP_RESET_ARM:
    case CTRL_SCREEN_SETUP_RESET_CONFIRM:
      render_setup_screen(ui, state, view);
      break;
    case CTRL_SCREEN_MAIN:
    default:
      render_main_screen(ui, state, view, language);
      break;
  }

  render_connection_icons(ui, view);
}
