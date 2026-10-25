// SPDX-License-Identifier: GPL-3.0-or-later
// The look of this fork's own apps (Photos, Team, OTP Messenger, Flock You,
// Drones, Sound link): glass over the theme's background (styleGlass: the aurora,
// a picture or a colour that stands apart), with a faint light rim, softly
// rounded tiles, the accent as the one colour, and a little squish on a press.
// High contrast and the e-paper Pro get solid cards instead. Popups, sheets and
// menus stay solid (nothing blurs what is behind them), so this is for pages.
//
// Cheap on software rendering: no shadows, no zoom transforms, gradients only
// on small elements. Included by UITask.cpp before the apps that use it.

namespace forklook {

constexpr lv_coord_t kCardR = 16, kTileR = 14, kThumbR = 10, kGap = 8;

inline bool plainLook() {
#if defined(HAS_TDECK_PRO)
  return true;
#else
  return s_theme_high_contrast;
#endif
}

// The lit accent, a little softer for fills: `toward_bg` of 255 is the background.
inline uint32_t accentSoft(uint8_t toward_bg) { return rgbMix(COLOR_GLOW, COLOR_BG, toward_bg); }

// A card or panel: glass with a light rim, or the solid card where glass is off.
// `rim` (0: none) colours the rim instead, for a card with a colour of its own
// (a teammate's, a pin's kind, a report's level).
inline void glassBox(lv_obj_t* o, lv_coord_t radius, uint32_t rim = 0) {
  lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(o, 0, LV_PART_MAIN);
  if (plainLook() || !styleGlass(o)) {
    lv_obj_set_style_bg_color(o, lv_color_hex(COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, s_theme_high_contrast ? 2 : (rim ? 2 : 1), LV_PART_MAIN);
    lv_obj_set_style_border_color(o, lv_color_hex(rim ? rim : COLOR_HAIR), LV_PART_MAIN);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    return;
  }
  if (rim) {
    lv_obj_set_style_border_width(o, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, lv_color_hex(rim), LV_PART_MAIN);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    return;
  }
  // The glass's edge: a faint light rim, brighter by day.
  lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(o, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_border_opa(o, s_theme_day ? 200 : 46, LV_PART_MAIN);
}

// A press squishes a control a little and lights its rim in the accent;
// keyboard and trackball focus look the same.
inline void pressLook(lv_obj_t* c, bool rim = true) {
  static const lv_style_prop_t kProps[] = {LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT,
                                           LV_STYLE_BORDER_COLOR, (lv_style_prop_t)0};
  static lv_style_transition_dsc_t s_tr;
  static bool s_ready = false;
  if (plainLook()) {
    lv_obj_set_style_border_color(c, lv_color_hex(COLOR_TEXT), LV_PART_MAIN | LV_STATE_PRESSED);
    return;
  }
  if (!s_ready) {
    lv_style_transition_dsc_init(&s_tr, kProps, lv_anim_path_ease_out, 90, 0, nullptr);
    s_ready = true;
  }
  lv_obj_set_style_transition(c, &s_tr, LV_PART_MAIN);
  lv_obj_set_style_transition(c, &s_tr, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_transform_width(c, -3, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(c, -3, LV_PART_MAIN | LV_STATE_PRESSED);
  if (!rim) return;
  for (lv_state_t st : {(lv_state_t)LV_STATE_PRESSED, (lv_state_t)LV_STATE_FOCUS_KEY}) {
    lv_obj_set_style_border_width(c, 2, LV_PART_MAIN | st);
    lv_obj_set_style_border_color(c, lv_color_hex(COLOR_GLOW), LV_PART_MAIN | st);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, LV_PART_MAIN | st);
  }
}

// A tile or a plain button: glass, rounded, with the press look.
inline void tile(lv_obj_t* c, lv_coord_t radius = kTileR) {
  glassBox(c, radius);
  pressLook(c);
}

// A round accent bubble with an icon (a page header's badge, an empty state's).
inline lv_obj_t* iconBubble(lv_obj_t* parent, lv_coord_t d, const char* icon) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, d, d);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  if (plainLook()) {
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  } else {
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_GLOW), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(b, 70, LV_PART_MAIN);
  }
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, icon);
  lv_obj_set_style_text_font(l, &ui_icons_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(plainLook() ? COLOR_BG : COLOR_ON_GLOW), LV_PART_MAIN);
  lv_obj_center(l);
  return b;
}

// A small accent pill with text (a header's "Newest first", a count).
inline lv_obj_t* pill(lv_obj_t* parent, const char* text) {
  lv_obj_t* p = lv_label_create(parent);
  lv_label_set_text(p, text);
  lv_obj_set_style_text_font(p, &g_font_12, LV_PART_MAIN);
  lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(p, 9, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(p, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
  if (plainLook()) {
    lv_obj_set_style_bg_color(p, lv_color_hex(COLOR_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_text_color(p, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  } else {
    lv_obj_set_style_bg_color(p, lv_color_hex(accentSoft(s_theme_day ? 200 : 170)), LV_PART_MAIN);
    lv_obj_set_style_text_color(p, lv_color_hex(s_theme_day ? COLOR_ACCENT : COLOR_GLOW), LV_PART_MAIN);
  }
  return p;
}

inline bool isColour(lv_color_t c, uint32_t hex) { return c.full == lv_color_hex(hex).full; }

// Gives a page built from the stock styles (styleCard panels, styleButton
// controls) this look, walking the whole tree: panels become glass cards
// (keeping a colour of their own as the rim), plain buttons glass tiles, and
// buttons filled with a colour (the chosen tab, SOS, Delete) keep it, rounded,
// with the press look. Labels and transparent boxes are left alone.
inline void dress(lv_obj_t* o) {
  if (!o || plainLook()) return;
  if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) > LV_OPA_50) {
    const lv_color_t bg = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
    const bool btn = lv_obj_check_type(o, &lv_btn_class);
    if (isColour(bg, COLOR_PANEL)) {
      const lv_color_t bc = lv_obj_get_style_border_color(o, LV_PART_MAIN);
      const bool own = lv_obj_get_style_border_width(o, LV_PART_MAIN) >= 2 && !isColour(bc, COLOR_BORDER) &&
                       !isColour(bc, COLOR_HAIR);
      glassBox(o, kCardR, own ? (lv_color_to32(bc) & 0xFFFFFF) : 0);
      if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) pressLook(o, !own);
    } else if (isColour(bg, COLOR_CONTROL)) {
      tile(o, btn ? 12 : kTileR);
    } else if (btn) {
      lv_obj_set_style_radius(o, 12, LV_PART_MAIN);
      pressLook(o, false);
    }
  }
  const uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; i++) dress(lv_obj_get_child(o, (int32_t)i));
}

}  // namespace forklook
