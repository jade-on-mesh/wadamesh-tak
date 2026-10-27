// SPDX-License-Identifier: GPL-3.0-or-later
// OTP Messenger: the app's screens. Included by UITask.cpp (it draws with the
// firmware's own widgets, fonts, keyboard and dialogs); the protocol lives in
// OtpCore.cpp and the mesh/storage glue in OtpMessenger.cpp.
//
// Screens: Lock -> Conversations -> Chat -> Pad, plus New conversation and
// Settings. Back (the bar, Esc) goes one screen up and closes from the list.
//
// The look follows the rest of the firmware, not the Lua app it came from: the
// Chats list's rows and glyph avatars, the chat's bubbles (yours on the right) and
// pill composer, and the Settings kit's cards, action rows and switches. Surfaces
// and text take the firmware theme; the OTP theme is the app's accent only (the
// send button, the pad gauge, status words), nudged until it reads on the page.

#include "OtpMessenger.h"
#include "OtpThemes.h"
#include "PadQr.h"
#include "src/extra/libs/qrcode/qrcodegen.h"   // LVGL's QR encoder: alphanumeric mode at level L

#if CAP_SOUND_LINK
// Sound link (SoundLinkUI.inc, included further down): the pad's check code
// over the speaker, and a short listen for the other side's.
namespace soundui { static bool playRaw(const char* msg); static void listenFor(uint32_t ms); }
#endif

namespace otpui {

enum class Screen : uint8_t { List, Pick, Chat, Pad, Settings, Lock, Import };

// The six themes of OTP_3_RC1.lua (OtpThemes.h, shared with Settings > Theme).
using Theme = OtpThemeColors;
static const Theme* const kThemes = kOtpThemes;

static lv_obj_t* s_root = nullptr;    // page root
static lv_obj_t* s_body = nullptr;    // current screen
static Screen s_screen = Screen::List;
static char s_tid[48] = {0};          // open conversation
static uint32_t s_seen = 0;
static lv_timer_t* s_timer = nullptr;
static bool s_pick_channels = false;
// chat widgets
static lv_obj_t* s_list = nullptr;     // bubble column
static lv_obj_t* s_ta = nullptr;
static lv_obj_t* s_counter = nullptr;  // byte budget, over the bottom of the bubbles while typing
static lv_obj_t* s_info = nullptr;     // header subtitle: pad, sync, signal
static lv_obj_t* s_parts = nullptr;    // channel participants strip
static lv_obj_t* s_bar = nullptr;      // pad gauge / transfer progress under the header
static lv_obj_t* s_send_btn = nullptr;
static lv_obj_t* s_parts_btn = nullptr;
static int s_shown_n = 60;             // bubbles drawn (newest)
// prompt
static lv_obj_t* s_prompt = nullptr;
static lv_obj_t* s_prompt_ta = nullptr;
static void (*s_prompt_cb)(const char*) = nullptr;
static uint32_t s_wipe_armed = 0;

static const Theme& th() { return kThemes[OtpMessenger::core().theme % 6]; }
static otp::Target* cur() { return s_tid[0] ? OtpMessenger::core().findById(s_tid) : nullptr; }
static void toast(const char* m, int ms = 1800) { if (g_lv.task) g_lv.task->showAlert(m, ms); }

static void show(Screen s);
static void close();
static void back();

// ---- colour ---------------------------------------------------------------------------------
static uint32_t darken(uint32_t c, float f) {
  return ((uint32_t)(((c >> 16) & 0xFF) * f) << 16) | ((uint32_t)(((c >> 8) & 0xFF) * f) << 8) | (uint32_t)((c & 0xFF) * f);
}
static float lin(float c) { c /= 255.0f; return c <= 0.03928f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float lum(uint32_t h) { return 0.2126f * lin((h >> 16) & 0xFF) + 0.7152f * lin((h >> 8) & 0xFF) + 0.0722f * lin(h & 0xFF); }
static float contrast(uint32_t a, uint32_t b) {
  const float la = lum(a) + 0.05f, lb = lum(b) + 0.05f;
  return la > lb ? la / lb : lb / la;
}
// Dark or light text on a fill of this colour.
static uint32_t contrastOf(uint32_t col) {
  const float l = lum(col) + 0.05f;
  if (0.96f / l >= 4.5f) return 0xF5F5F5;
  const uint32_t shade = darken(col, 0.2f);
  if (l / (lum(shade) + 0.05f) >= 4.5f) return shade;
  return 0x14171C;
}
// E-paper and high contrast keep the firmware's own plain look: no theme accents.
static bool plainLook() {
#if defined(HAS_TDECK_PRO)
  return true;
#else
  return s_theme_high_contrast;
#endif
}
// The app's accent: the OTP theme's main colour.
static uint32_t accent() { return plainLook() ? COLOR_GLOW : th().channel; }
// An OTP theme colour used as text or an icon on the page, eased toward the text
// colour until it reads (B&W's near-whites vanish on the day theme otherwise).
static uint32_t ink(uint32_t c) {
  if (plainLook()) return COLOR_TEXT;
  uint32_t out = c;
  for (int t = 51; t <= 255 && contrast(out, COLOR_BG) < 3.0f; t += 51) out = rgbMix(c, COLOR_TEXT, (uint8_t)t);
  return out;
}

// The screens are built with the firmware's own kit (cards, rows, buttons) in its
// greys; this pass then gives them the OTP theme: cards and buttons tinted with
// its colour, primary buttons and switches in it, bubbles in its send and receive
// colours. It recolours by matching the kit's colours, so anything already given
// a meaning (danger red, a status colour) is left alone.
static bool isColor(lv_color_t c, uint32_t hex) { return c.full == lv_color_hex(hex).full; }
static void bgPair(lv_obj_t* o, uint32_t fill, uint32_t pressed) {
  lv_obj_set_style_bg_color(o, lv_color_hex(fill), LV_PART_MAIN);
  lv_obj_set_style_bg_color(o, lv_color_hex(pressed), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, LV_PART_MAIN);
}
// Glass (the fork apps' look, ForkLook.inc) where the theme's background allows
// it, rimmed in the OTP theme's colour; otherwise the tinted solid kit.
static bool glassOn() { return s_glass_on && !plainLook(); }
static void glassAccent(lv_obj_t* o, lv_coord_t radius, uint32_t a, lv_opa_t rim_opa) {
  forklook::glassBox(o, radius);
  lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(o, lv_color_hex(a), LV_PART_MAIN);
  lv_obj_set_style_border_opa(o, rim_opa, LV_PART_MAIN);
}
// `glass` false for popups (the prompt): they stay solid.
static void themeTree(lv_obj_t* o, bool glass = true) {
  if (!o || plainLook()) return;
  const Theme& t = th();
  const uint32_t a = accent();
  if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) > LV_OPA_50) {
    const lv_color_t bg = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
    if (isColor(bg, COLOR_CONTROL)) {            // buttons, fields
      if (glass && glassOn()) {
        glassAccent(o, forklook::kTileR - 2, a, 110);
        lv_obj_set_style_bg_color(o, lv_color_hex(rgbMix(COLOR_CONTROL_PRESSED, a, 120)), LV_PART_MAIN | LV_STATE_PRESSED);
        if (lv_obj_check_type(o, &lv_btn_class)) forklook::pressLook(o);
      } else {
        bgPair(o, rgbMix(COLOR_CONTROL, a, 72), rgbMix(COLOR_CONTROL_PRESSED, a, 120));
        lv_obj_set_style_border_color(o, lv_color_hex(rgbMix(COLOR_BORDER, a, 150)), LV_PART_MAIN);
      }
    } else if (isColor(bg, COLOR_PANEL)) {       // cards
      if (glass && glassOn()) {
        glassAccent(o, forklook::kCardR, a, 70);
      } else {
        lv_obj_set_style_bg_color(o, lv_color_hex(rgbMix(COLOR_PANEL, a, 26)), LV_PART_MAIN);
        lv_obj_set_style_border_color(o, lv_color_hex(rgbMix(COLOR_BORDER, a, 110)), LV_PART_MAIN);
      }
    } else if (isColor(bg, COLOR_GLOW)) {        // primary buttons
      bgPair(o, a, darken(a, 0.8f));
      lv_obj_set_style_text_color(o, lv_color_hex(contrastOf(a)), LV_PART_MAIN);
    } else if (isColor(bg, COLOR_CHAT_SENT_BG)) {
      lv_obj_set_style_bg_color(o, lv_color_hex(rgbMix(COLOR_BG, t.encrypt, 80)), LV_PART_MAIN);
    } else if (isColor(bg, COLOR_CHAT_RECV_BG)) {
      lv_obj_set_style_bg_color(o, lv_color_hex(rgbMix(COLOR_BG, t.decrypt, 60)), LV_PART_MAIN);
    }
  }
  if (isColor(lv_obj_get_style_text_color(o, LV_PART_MAIN), COLOR_GLOW))
    lv_obj_set_style_text_color(o, lv_color_hex(ink(a)), LV_PART_MAIN);
  if (lv_obj_check_type(o, &lv_switch_class))
    lv_obj_set_style_bg_color(o, lv_color_hex(a), LV_PART_INDICATOR | LV_STATE_CHECKED);
  const uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; i++) themeTree(lv_obj_get_child(o, (int32_t)i), glass);
}

// ---- small widget helpers --------------------------------------------------------------------
static lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  useChainedFont(l);
  return l;
}

static lv_obj_t* para(lv_obj_t* parent, const char* text, uint32_t color = 0) {
  lv_obj_t* l = label(parent, text, &g_font_12, color ? color : COLOR_SUB);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  return l;
}

static lv_obj_t* box(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

// A bare line icon that lifts when pressed, as beside the Chat composer.
static lv_obj_t* iconBtn(lv_obj_t* parent, const char* icon, lv_event_cb_t cb, uint32_t color = 0) {
  lv_obj_t* b = lv_btn_create(parent);
  styleButton(b);
  const lv_coord_t sz = SC(30);
  lv_obj_set_size(b, sz, sz);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
  lv_obj_set_ext_click_area(b, 4);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* l = lv_label_create(b);
  setIconLabel(l, icon);
  lv_obj_set_style_text_color(l, lv_color_hex(color ? color : COLOR_SUB), LV_PART_MAIN);
  lv_obj_center(l);
  return b;
}

// A round button filled with the accent (send, new conversation).
static lv_obj_t* roundAccent(lv_obj_t* parent, const char* icon, lv_event_cb_t cb, lv_coord_t sz) {
  lv_obj_t* b = lv_btn_create(parent);
  stylePrimary(b);
  lv_obj_set_size(b, sz, sz);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
  uint32_t fg = COLOR_ON_GLOW;
  if (!plainLook()) {
    const uint32_t a = accent();
    lv_obj_set_style_bg_color(b, lv_color_hex(a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(darken(a, 0.8f)), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_outline_color(b, lv_color_hex(a), LV_PART_MAIN);
    fg = contrastOf(a);
  }
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* l = lv_label_create(b);
  setIconLabel(l, icon);
  lv_obj_set_style_text_color(l, lv_color_hex(fg), LV_PART_MAIN);
  lv_obj_center(l);
  return b;
}

// The conversation's glyph: the same one the Chats list shows for that contact or
// channel (a square for a channel, a circle for a person), its emoji if it has one.
static lv_obj_t* avatar(lv_obj_t* parent, const char* name, bool channel, lv_coord_t sz) {
  lv_obj_t* av = makeGlyphAvatar(parent, name, channel ? GLYPH_SQUARE : GLYPH_CIRCLE, sz);
  char eb[20];
  if (touchPrefsGetChannelEmoji(name, eb, sizeof eb) || (!channel && nameFirstEmoji(name, eb, sizeof eb)))
    glyphAvatarSetEmoji(av, name, eb);
  return av;
}
static lv_obj_t* avatar(lv_obj_t* parent, const otp::Target& t, lv_coord_t sz) {
  return avatar(parent, t.name, t.kind == otp::Kind::Channel, sz);
}

static void displayName(const char* name, bool channel, char* out, size_t cap) {
  char raw[48];
  snprintf(raw, sizeof raw, "%s%s", channel ? "#" : "", name);
  copyUtf8ReplacingMissingGlyphs(&g_font_semi_14, out, cap, raw);
}
static void displayName(const otp::Target& t, char* out, size_t cap) {
  displayName(t.name, t.kind == otp::Kind::Channel, out, cap);
}

// The width rows and cards share: the Settings cards' (settingsCardIn).
static lv_coord_t rowW() { return lv_disp_get_hor_res(nullptr) - 22; }

// A scrolling screen body below the bar, with the Settings pages' margins.
static lv_obj_t* newBody(bool scroll = true) {
  // Screens are swapped from their own buttons' handlers: never delete the
  // tree an event is still being dispatched in.
  if (s_body) { lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN); lv_obj_del_async(s_body); s_body = nullptr; }
  s_list = s_ta = s_counter = s_info = s_parts = s_bar = s_send_btn = s_parts_btn = nullptr;
  s_body = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_body);
  lv_obj_set_size(s_body, LV_PCT(100), LV_PCT(100));
  if (scroll) {
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(s_body, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_body, SC(8), LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_body, SC(6), LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
  } else {
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
  }
  return s_body;
}

// A page title with optional icon buttons on its right (filled in by the caller).
static lv_obj_t* titleRow(lv_obj_t* parent, const char* text) {
  lv_obj_t* r = box(parent);
  lv_obj_set_size(r, rowW(), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, SC(6), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(r, SC(2), LV_PART_MAIN);
  lv_obj_t* l = label(r, text, &g_font_semi_16, COLOR_TEXT);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_flex_grow(l, 1);
  return r;
}

// A list row in the Chats list's language: soft rounded, tinted while it holds
// something new, lit by a press or the keypad cursor.
static lv_obj_t* listRow(lv_obj_t* parent, lv_coord_t h, bool tinted, lv_event_cb_t cb, void* ud) {
  lv_obj_t* r = lv_btn_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, rowW(), h);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_set_style_radius(r, SC(10), LV_PART_MAIN);
  if (glassOn()) {
    // A glass row; one holding something new is rimmed in the theme's colour.
    if (tinted) glassAccent(r, SC(12), accent(), LV_OPA_COVER);
    else forklook::glassBox(r, SC(12));
    forklook::pressLook(r);
  } else if (tinted && !s_theme_high_contrast) {
    lv_obj_set_style_bg_color(r, lv_color_hex(rgbMix(COLOR_BG, accent(), 22)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN);
  }
  lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_CONTROL), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_CONTROL), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_PANEL), LV_PART_MAIN | LV_STATE_DISABLED);
  lv_obj_set_style_opa(r, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
  if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
  return r;
}

// An empty state: a lit icon in a soft disc, a line, and help under it.
static void emptyState(lv_obj_t* parent, const char* icon, const char* title, const char* help) {
  lv_obj_t* col = box(parent);
  lv_obj_set_size(col, rowW(), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, SC(6), LV_PART_MAIN);
  lv_obj_set_style_pad_ver(col, SC(10), LV_PART_MAIN);
  lv_obj_t* disc = box(col);
  lv_obj_set_size(disc, SC(44), SC(44));
  lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(disc, lv_color_hex(plainLook() ? COLOR_CONTROL : rgbMix(COLOR_PANEL, accent(), 50)), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, LV_PART_MAIN);
  if (glassOn()) {   // the fork apps' accent bubble, in the theme's colour
    lv_obj_set_style_bg_color(disc, lv_color_hex(accent()), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(disc, lv_color_hex(darken(accent(), 0.7f)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(disc, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_width(disc, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(disc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(disc, 70, LV_PART_MAIN);
  }
  lv_obj_t* il = lv_label_create(disc);
  setIconLabel(il, icon);
  lv_obj_set_style_text_color(il, lv_color_hex(glassOn() ? contrastOf(accent()) : ink(accent())), LV_PART_MAIN);
  lv_obj_center(il);
  label(col, title, &g_font_semi_14, COLOR_TEXT);
  if (help && *help) {
    lv_obj_t* h = label(col, help, &g_font_12, COLOR_SUB);
    lv_obj_set_width(h, rowW() - SC(24));
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  }
}

static void fmtAgo(uint32_t epoch, uint32_t at_ms, char* out, size_t cap) {
  const uint32_t now_e = the_mesh.getRTCClock()->getCurrentTime();
  if (epoch && now_e > 1700000000UL) {
    time_t t = (time_t)epoch, n = (time_t)now_e;
    struct tm a, b;
    if (localtime_r(&t, &a) && localtime_r(&n, &b)) {
      if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) strftime(out, cap, "%H:%M", &a);
      else strftime(out, cap, "%b %d", &a);
      return;
    }
  }
  if (!at_ms) { out[0] = 0; return; }
  const uint32_t s = (millis() - at_ms) / 1000;
  if (s < 60) snprintf(out, cap, "now");
  else if (s < 3600) snprintf(out, cap, "%um", (unsigned)(s / 60));
  else if (s < 86400) snprintf(out, cap, "%uh", (unsigned)(s / 3600));
  else snprintf(out, cap, "%ud", (unsigned)(s / 86400));
}

// The pad's state in one colour: plenty, running low, used up.
static uint32_t padColor(int avail) {
  if (avail <= 0) return COLOR_STATUS_DANGER_TEXT;
  if (avail < otp::kLowPadBytes) return COLOR_STATUS_WARN;
  return ink(accent());
}

// ---- text prompt (long fields: pad hex, passphrases) --------------------------------
static void promptClose() {
  if (g_lv.keyboard) lv_keyboard_set_textarea(g_lv.keyboard, nullptr);
  if (s_prompt) popupClose(&s_prompt);
  s_prompt_ta = nullptr;
}
static void promptDone(bool ok) {
  void (*cb)(const char*) = s_prompt_cb;
  s_prompt_cb = nullptr;
  char* text = nullptr;
  if (ok && s_prompt_ta) {
    kbMirrorSyncToReal();
    const char* t = lv_textarea_get_text(s_prompt_ta);
    text = strdup(t ? t : "");
  }
  promptClose();
  hideKb();
  if (cb) cb(ok ? (text ? text : "") : nullptr);
  if (text) { memset(text, 0, strlen(text)); free(text); }
}
static void prompt(const char* title, const char* hint, bool secret, uint32_t max_len, void (*cb)(const char*)) {
  promptClose();
  s_prompt_cb = cb;
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr), sh = lv_disp_get_ver_res(nullptr);
  s_prompt = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_prompt);
  lv_obj_set_size(s_prompt, sw, sh - STATUSBAR_H);
  lv_obj_set_pos(s_prompt, 0, STATUSBAR_H);
  lv_obj_set_style_bg_color(s_prompt, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_prompt, LV_OPA_60, LV_PART_MAIN);
  lv_obj_add_flag(s_prompt, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_prompt, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* card = lv_obj_create(s_prompt);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, sw - 20, LV_SIZE_CONTENT);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 6);
  styleCard(card);
  lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, SC(10), LV_PART_MAIN);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, SC(8), LV_PART_MAIN);
  label(card, title, &g_font_semi_14, COLOR_TEXT);
  if (hint && *hint) para(card, hint);
  s_prompt_ta = lv_textarea_create(card);
  lv_obj_set_width(s_prompt_ta, LV_PCT(100));
  styleCard(s_prompt_ta);
  lv_obj_set_style_bg_color(s_prompt_ta, lv_color_hex(COLOR_FIELD), LV_PART_MAIN);
  lv_obj_set_style_radius(s_prompt_ta, SC(8), LV_PART_MAIN);
  lv_obj_set_style_text_font(s_prompt_ta, &g_font_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_prompt_ta, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  lv_textarea_set_max_length(s_prompt_ta, max_len);
  if (secret) {
    lv_textarea_set_one_line(s_prompt_ta, true);
    lv_textarea_set_password_mode(s_prompt_ta, true);
  } else {
    lv_obj_set_height(s_prompt_ta, 64);
  }
  attachSettingsTaEvents(s_prompt_ta);
  lv_obj_add_event_cb(s_prompt_ta, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_READY) promptDone(true);
  }, LV_EVENT_READY, nullptr);
  lv_obj_t* r = box(card);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, SC(8), LV_PART_MAIN);
  for (int i = 0; i < 2; i++) {
    lv_obj_t* b = lv_btn_create(r);
    if (i) stylePrimary(b); else styleButton(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, SC(32));
    lv_obj_set_style_pad_hor(b, SC(14), LV_PART_MAIN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(b, i ? (lv_event_cb_t)[](lv_event_t*) { promptDone(true); }
                             : (lv_event_cb_t)[](lv_event_t*) { promptDone(false); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = label(b, i ? "OK" : TR("Cancel"), i ? &g_font_semi_14 : &g_font_14,
                        i ? COLOR_ON_GLOW : COLOR_TEXT);
    lv_obj_center(l);
  }
  themeTree(s_prompt, false);   // a popup: solid
  if (g_lv.keyboard) kbMirrorBind(s_prompt_ta);
  lv_group_t* g = lv_group_get_default();
  if (g) lv_group_focus_obj(s_prompt_ta);
}

// ---- Lock screen ---------------------------------------------------------------------
static void unlockWith(const char* pass) {
  if (!pass) return;
  if (!OtpMessenger::core().unlock(pass)) { toast(TR("Wrong passphrase"), 1600); return; }
  show(Screen::List);
}
static void panicConfirmed() {
  OtpMessenger::core().panicWipe();
  toast(TR("Everything wiped. Protection is off."), 2200);
  s_tid[0] = 0;
  show(Screen::List);
}
static void panicAsk(lv_event_t*) {
  showConfirm(TR("Erase every pad and message on this device, and turn protection off?\n\nThis cannot be undone."),
              TR("Wipe"), panicConfirmed);
}
static void buildLock() {
  lv_obj_t* b = newBody();
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_top(b, SC(18), LV_PART_MAIN);
  emptyState(b, UI_ICON_LOCK, "OTP Messenger",
             TR("Locked. Your pads and messages are encrypted with your passphrase. "
                "Messages that arrive while locked are kept and opened when you unlock."));
  lv_obj_t* u = lv_btn_create(b);
  stylePrimary(u);
  lv_obj_set_size(u, LV_MIN(rowW(), SC(200)), SC(36));
  lv_obj_set_style_radius(u, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_add_event_cb(u, [](lv_event_t*) { prompt(TR("Passphrase"), nullptr, true, 64, unlockWith); },
                      LV_EVENT_CLICKED, nullptr);
  lv_obj_t* ul = label(u, TR("Unlock"), &g_font_semi_14, COLOR_ON_GLOW);
  lv_obj_center(ul);
  lv_obj_t* p = lv_btn_create(b);
  styleDanger(p);
  lv_obj_set_size(p, LV_MIN(rowW(), SC(200)), SC(32));
  lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_add_event_cb(p, panicAsk, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* pl = label(p, TR("Panic wipe"), &g_font_14, COLOR_STATUS_DANGER_TEXT);
  lv_obj_center(pl);
}

// ---- Conversations ---------------------------------------------------------------------
static void openChatCb(lv_event_t* e) {
  otp::Target* t = (otp::Target*)lv_event_get_user_data(e);
  if (!t) return;
  snprintf(s_tid, sizeof s_tid, "%s", t->id);
  show(Screen::Chat);
}

static const otp::Entry* lastShown(otp::Target& t) {
  OtpMessenger::core().ensureLog(t);
  return t.log_n ? &t.log[t.log_n - 1] : nullptr;
}

// Two lines, as a Chats row: the name and the time on top, the last message and
// the unread count (or what is left of the pad) under it.
static lv_coord_t twoLineRowH() {
  return LV_MAX(SC(42), (lv_coord_t)(lv_font_get_line_height(&g_font_semi_14) + lv_font_get_line_height(&g_font_12) + SC(10)));
}

static void buildList() {
  otp::Core& c = OtpMessenger::core();
  lv_obj_t* b = newBody();
  lv_obj_set_style_pad_row(b, SC(2), LV_PART_MAIN);
  lv_obj_t* top = titleRow(b, TR("Conversations"));
  iconBtn(top, UI_ICON_SETTINGS, [](lv_event_t*) { show(Screen::Settings); });
  roundAccent(top, UI_ICON_PLUS, [](lv_event_t*) { show(Screen::Pick); }, SC(30));

  // Newest activity first.
  otp::Target* order[otp::kMaxTargets];
  int n = 0;
  for (int i = 0; i < c.targetCount(); i++) {
    otp::Target* t = c.targetAt(i);
    if (!t || !t->keep) continue;
    order[n++] = t;
  }
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0; j--) {
      const otp::Entry* a = lastShown(*order[j - 1]);
      const otp::Entry* z = lastShown(*order[j]);
      const uint32_t ka = a ? a->seq : 0, kz = z ? z->seq : 0;
      if (kz > ka) { otp::Target* x = order[j]; order[j] = order[j - 1]; order[j - 1] = x; }
    }
  if (!n) {
    emptyState(b, UI_ICON_LOCK, TR("No conversations yet"),
               TR("Pick a contact or a private channel, then give it a pad: generate one and export it "
                  "to the SD card for the other device, or import theirs."));
    lv_obj_t* go = setButton(b, UI_ICON_PLUS, "New conversation", [](lv_event_t*) { show(Screen::Pick); }, nullptr, SET_PRIMARY);
    lv_obj_set_width(go, rowW());
    return;
  }
  const lv_coord_t rh = twoLineRowH(), av = SC(30), pl = SC(6), pr = SC(8);
  const lv_coord_t tx = pl + av + SC(10);
  for (int i = 0; i < n; i++) {
    otp::Target* t = order[i];
    lv_obj_t* r = listRow(b, rh, t->unread > 0, openChatCb, t);
    lv_obj_t* a = avatar(r, *t, av);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, pl, 0);

    const otp::Entry* e = lastShown(*t);
    // Time, top right: lit while there is something unread.
    char when[16] = "";
    if (e) fmtAgo(e->at_epoch, e->at_ms, when, sizeof when);
    lv_coord_t tw = 0;
    if (when[0]) {
      lv_obj_t* tl = label(r, when, t->unread ? &g_font_semi_12 : &g_font_12, t->unread ? ink(accent()) : COLOR_TERTIARY);
      lv_obj_align(tl, LV_ALIGN_TOP_RIGHT, -pr, SC(5));
      tw = lv_txt_get_width(when, strlen(when), &g_font_semi_12, 0, LV_TEXT_FLAG_NONE);
    }
    char nm[56];
    displayName(*t, nm, sizeof nm);
    lv_obj_t* nl = label(r, nm, &g_font_semi_14, COLOR_TEXT);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_set_size(nl, LV_MAX((lv_coord_t)40, (lv_coord_t)(rowW() - tx - pr - tw - SC(8))),
                    lv_font_get_line_height(&g_font_semi_14));
    lv_obj_align(nl, LV_ALIGN_TOP_LEFT, tx, SC(4));

    // Bottom right: the unread count, or the pad left.
    lv_coord_t rw = 0;
    if (t->unread) {
      lv_obj_t* pill = makeCountPill(r, t->unread > 99 ? 100 : t->unread);
      if (!plainLook()) {
        lv_obj_set_style_bg_color(pill, lv_color_hex(accent()), LV_PART_MAIN);
        if (lv_obj_t* pt = lv_obj_get_child(pill, 0))
          lv_obj_set_style_text_color(pt, lv_color_hex(contrastOf(accent())), LV_PART_MAIN);
      }
      lv_obj_update_layout(pill);
      rw = lv_obj_get_width(pill);
      lv_obj_align(pill, LV_ALIGN_BOTTOM_RIGHT, -pr, -SC(5));
    } else {
      const int avail = c.available(*t);
      char pad[24];
      if (avail > 0) snprintf(pad, sizeof pad, "%d B", avail);
      else snprintf(pad, sizeof pad, "%s", c.hasPad(*t) ? TR("used up") : TR("no pad"));
      lv_obj_t* pl2 = label(r, pad, &g_font_12, avail >= otp::kLowPadBytes ? COLOR_TERTIARY : padColor(avail));
      lv_obj_align(pl2, LV_ALIGN_BOTTOM_RIGHT, -pr, -SC(5));
      rw = lv_txt_get_width(pad, strlen(pad), &g_font_12, 0, LV_TEXT_FLAG_NONE);
    }
    // The last message, "You:" quiet before your own.
    lv_coord_t px = tx;
    char prev[96];
    if (e) {
      if (e->mine) {
        lv_obj_t* you = label(r, TR("You:"), &g_font_12, COLOR_TERTIARY);
        lv_obj_align(you, LV_ALIGN_BOTTOM_LEFT, px, -SC(5));
        px += lv_txt_get_width(TR("You:"), strlen(TR("You:")), &g_font_12, 0, LV_TEXT_FLAG_NONE) + 4;
      }
      // A channel names who said it: "Sam: text".
      char raw[112];
      const bool named = t->kind == otp::Kind::Channel && !e->mine && e->who[0];
      snprintf(raw, sizeof raw, "%s%s%.*s", named ? e->who : "", named ? ": " : "",
               (int)(e->len > 70 ? 70 : e->len), e->text ? e->text : "");
      for (char* q = raw; *q; ++q) if (*q == '\n' || *q == '\r') *q = ' ';
      copyUtf8ReplacingMissingGlyphs(&g_font_12, prev, sizeof prev, raw);
    } else {
      snprintf(prev, sizeof prev, "%s", c.hasPad(*t) ? TR("No messages yet") : TR("Tap to add a pad"));
    }
    lv_obj_t* pv = label(r, prev, &g_font_12, COLOR_SUB);
    lv_label_set_long_mode(pv, LV_LABEL_LONG_DOT);
    lv_obj_set_size(pv, LV_MAX((lv_coord_t)30, (lv_coord_t)(rowW() - px - pr - rw - SC(8))),
                    lv_font_get_line_height(&g_font_12));
    lv_obj_align(pv, LV_ALIGN_BOTTOM_LEFT, px, -SC(5));
  }
}

// ---- New conversation ----------------------------------------------------------------------
static void pickContactCb(lv_event_t* e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  uint8_t pub[32];
  char name[33];
  if (!OtpMessenger::contactAt(i, pub, name, sizeof name)) return;
  otp::Target* t = OtpMessenger::core().dmTarget(pub, name);
  if (!t) { toast(TR("Too many conversations")); return; }
  snprintf(s_tid, sizeof s_tid, "%s", t->id);
  show(OtpMessenger::core().hasPad(*t) ? Screen::Chat : Screen::Pad);
}
static void pickChannelCb(lv_event_t* e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  char name[33];
  if (!OtpMessenger::channelAt(i, name, sizeof name)) return;
  otp::Target* t = OtpMessenger::core().channelTarget(name);
  if (!t) { toast(TR("OTP is not available on the Public channel"), 2200); return; }
  snprintf(s_tid, sizeof s_tid, "%s", t->id);
  show(OtpMessenger::core().hasPad(*t) ? Screen::Chat : Screen::Pad);
}
// One pick row: the glyph, the name, a status line, a chevron.
static void pickRow(lv_obj_t* parent, const char* name, bool channel, const char* sub, uint32_t sub_col,
                    bool enabled, lv_event_cb_t cb, int idx) {
  const lv_coord_t rh = twoLineRowH(), av = SC(30), pl = SC(6), pr = SC(8);
  const lv_coord_t tx = pl + av + SC(10);
  lv_obj_t* r = listRow(parent, rh, false, enabled ? cb : nullptr, (void*)(intptr_t)idx);
  if (!enabled) lv_obj_add_state(r, LV_STATE_DISABLED);
  lv_obj_t* a = avatar(r, name, channel, av);
  lv_obj_align(a, LV_ALIGN_LEFT_MID, pl, 0);
  lv_obj_t* ch = lv_label_create(r);
  setIconLabel(ch, UI_ICON_CHEVRON_RIGHT);
  lv_obj_set_style_text_color(ch, lv_color_hex(COLOR_TERTIARY), LV_PART_MAIN);
  lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -pr, 0);
  char nm[56];
  displayName(name, channel, nm, sizeof nm);
  const lv_coord_t tw = LV_MAX((lv_coord_t)40, (lv_coord_t)(rowW() - tx - pr - SC(26)));
  lv_obj_t* nl = label(r, nm, &g_font_semi_14, COLOR_TEXT);
  lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
  lv_obj_set_size(nl, tw, lv_font_get_line_height(&g_font_semi_14));
  lv_obj_align(nl, LV_ALIGN_TOP_LEFT, tx, SC(4));
  lv_obj_t* sl = label(r, sub, &g_font_12, sub_col);
  lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
  lv_obj_set_size(sl, tw, lv_font_get_line_height(&g_font_12));
  lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, tx, -SC(5));
}
static void buildPick() {
  lv_obj_t* b = newBody();
  lv_obj_set_style_pad_row(b, SC(2), LV_PART_MAIN);
  titleRow(b, TR("New conversation"));
  // Contacts | Channels, as the Settings choice tiles.
  lv_obj_t* seg = box(b);
  lv_obj_set_size(seg, rowW(), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(seg, SC(6), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(seg, SC(6), LV_PART_MAIN);
  const char* segs[2] = {TR("Contacts"), TR("Channels")};
  for (int i = 0; i < 2; i++) {
    lv_obj_t* t = lv_btn_create(seg);
    styleButton(t);
    lv_obj_set_size(t, (rowW() - SC(6)) / 2, SC(30));
    lv_obj_add_event_cb(t, [](lv_event_t* e) {
      s_pick_channels = (int)(intptr_t)lv_event_get_user_data(e) == 1;
      show(Screen::Pick);
    }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = label(t, segs[i], &g_font_14, COLOR_TEXT);
    lv_obj_center(l);
    setTileStyle(t, (i == 1) == s_pick_channels, true);
  }
  otp::Core& c = OtpMessenger::core();
  int shown = 0;
  if (!s_pick_channels) {
    const int n = OtpMessenger::contactCount();
    for (int i = 0; i < n && shown < 120; i++) {
      uint8_t pub[32];
      char name[33];
      if (!OtpMessenger::contactAt(i, pub, name, sizeof name)) continue;
      char hex[9]; otp::toHex(pub, 4, hex);
      char id[16]; snprintf(id, sizeof id, "dm:%s", hex);
      otp::Target* t = c.findById(id);
      char sub[48];
      const int avail = t ? c.available(*t) : 0;
      if (avail > 0) snprintf(sub, sizeof sub, "%d B of pad left", avail);
      else snprintf(sub, sizeof sub, "%s", TR("No pad yet"));
      pickRow(b, name, false, sub, avail > 0 ? padColor(avail) : COLOR_SUB, true, pickContactCb, i);
      shown++;
    }
    if (!shown) emptyState(b, UI_ICON_USER_PLUS, TR("No chat contacts yet"), TR("Add the other person as a contact first."));
  } else {
    for (int i = 0; i < OtpMessenger::channelCount(); i++) {
      char name[33];
      if (!OtpMessenger::channelAt(i, name, sizeof name)) continue;
      const bool pub = otp::Core::isPublicChannel(name);
      pickRow(b, name, true, pub ? TR("Not available: everyone has its key") : TR("Private channel"),
              COLOR_SUB, !pub, pickChannelCb, i);
      shown++;
    }
    if (!shown) emptyState(b, UI_ICON_HASH, TR("No channels configured"), TR("Add a private channel in Chats first."));
    else para(b, TR("Use a private channel: Public uses a key everyone has, so a pad there protects nothing."));
  }
}

// ---- Chat --------------------------------------------------------------------------------
static int sigGrade(uint32_t* color, const char** word) {
  // Average link margin over the last samples, as the Lua app's signal row.
  static float hist[8];
  static int n = 0, idx = 0;
  static uint32_t next_ms = 0;
  if ((int32_t)(millis() - next_ms) >= 0) {
    next_ms = millis() + 3000;
    hist[idx] = radio_driver.getCurrentRSSI() - radio_driver.getNoiseFloor();
    idx = (idx + 1) % 8;
    if (n < 8) n++;
  }
  float m = 0;
  for (int i = 0; i < n; i++) m += hist[i];
  m = n ? m / n : -99;
  if (m >= 20) { *color = COLOR_STATUS_OK_TEXT; *word = "excellent signal"; return 4; }
  if (m >= 12) { *color = COLOR_STATUS_OK_TEXT; *word = "good signal"; return 3; }
  if (m >= 6) { *color = COLOR_STATUS_WARN; *word = "fair signal"; return 2; }
  if (m >= 0) { *color = COLOR_STATUS_WARN; *word = "poor signal"; return 1; }
  *color = n ? COLOR_STATUS_DANGER_TEXT : COLOR_TERTIARY;
  *word = n ? "very poor signal" : "no signal data";
  return 0;
}

static void refreshInfo() {
  otp::Target* t = cur();
  if (!t || !s_info) return;
  otp::Core& c = OtpMessenger::core();
  const int avail = c.available(*t);
  // Subtitle: the pad, then sync, then the signal, each in its state's colour.
  char pad[64];
  uint32_t pcol = COLOR_SUB;
  if (t->badge[0]) { snprintf(pad, sizeof pad, "%s", t->badge); pcol = ink(th().ok); }
  else if (avail <= 0) { snprintf(pad, sizeof pad, "%s", c.hasPad(*t) ? TR("Pad used up") : TR("No pad yet")); pcol = COLOR_STATUS_DANGER_TEXT; }
  else if (avail < otp::kLowPadBytes) { snprintf(pad, sizeof pad, "%d B left, running low", avail); pcol = COLOR_STATUS_WARN; }
  else snprintf(pad, sizeof pad, "%d B left", avail);
  const char* sync = t->sync_ok == 1 ? TR("in sync") : t->sync_ok == 0 ? TR("out of sync") : TR("sync unchecked");
  const uint32_t ycol = t->sync_ok == 1 ? COLOR_SUB : t->sync_ok == 0 ? COLOR_STATUS_DANGER_TEXT : COLOR_TERTIARY;
  uint32_t scol; const char* sword;
  sigGrade(&scol, &sword);
  if (scol == COLOR_STATUS_OK_TEXT) scol = COLOR_SUB;   // a good link needs no colour
  char line[220];
  snprintf(line, sizeof line, "#%06lX %s#  #%06lX %s#  #%06lX %s#", (unsigned long)pcol, pad,
           (unsigned long)ycol, sync, (unsigned long)scol, sword);
  if (strcmp(lv_label_get_text(s_info), line) != 0) lv_label_set_text(s_info, line);

  // The gauge: a transfer's progress while one runs, else what is left of the pad.
  if (s_bar) {
    int total = 0;
    int done = c.sendProgress(*t, &total);
    uint32_t bc = accent();
    if (done < 0) { done = c.recvProgress(*t, &total); bc = plainLook() ? COLOR_GLOW : th().ok; }
    if (done < 0 || total <= 1) {
      total = (int)t->pad.n;
      done = avail;
      bc = avail <= 0 ? COLOR_STATUS_DANGER : avail < otp::kLowPadBytes ? COLOR_STATUS_WARN : accent();
    }
    if (total > 0) {
      lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
      lv_bar_set_range(s_bar, 0, total);
      lv_bar_set_value(s_bar, LV_MIN(done, total), LV_ANIM_OFF);
      lv_obj_set_style_bg_color(s_bar, lv_color_hex(bc), LV_PART_INDICATOR);
    } else {
      lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }
  }
  // Who is on the channel: a dot per person, lit when they are in step.
  if (s_parts) {
    const bool on = t->kind == otp::Kind::Channel && c.show_participants;
    if (s_parts_btn && lv_obj_get_child(s_parts_btn, 0))
      lv_obj_set_style_text_color(lv_obj_get_child(s_parts_btn, 0), lv_color_hex(on ? ink(accent()) : COLOR_SUB), LV_PART_MAIN);
    if (!on) { lv_obj_add_flag(s_parts, LV_OBJ_FLAG_HIDDEN); return; }
    otp::Participant ps[8];
    const int n = c.activeParticipants(*t, ps, 8);
    char sig[160] = "";
    size_t o = 0;
    for (int i = 0; i < n && o + 40 < sizeof sig; i++) o += snprintf(sig + o, sizeof sig - o, "%d%s|", ps[i].in_sync, ps[i].name);
    const char* old = (const char*)lv_obj_get_user_data(s_parts);
    if (!old || strcmp(old, sig) != 0) {
      free((void*)old);
      lv_obj_set_user_data(s_parts, strdup(sig));
      lv_obj_clean(s_parts);
      if (!n) label(s_parts, TR("No one heard yet"), &g_font_12, COLOR_TERTIARY);
      for (int i = 0; i < n; i++) {
        lv_obj_t* chip = box(s_parts);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(chip, 4, LV_PART_MAIN);
        lv_obj_t* dot = box(chip);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(ps[i].in_sync == 1 ? COLOR_STATUS_OK : ps[i].in_sync == 0 ? COLOR_STATUS_DANGER : COLOR_TERTIARY), LV_PART_MAIN);
        char nm[40];
        copyUtf8ReplacingMissingGlyphs(&g_font_12, nm, sizeof nm, ps[i].name);
        label(chip, nm, &g_font_12, COLOR_SUB);
      }
    }
    lv_obj_clear_flag(s_parts, LV_OBJ_FLAG_HIDDEN);
  }
}

static void refreshCounter() {
  otp::Target* t = cur();
  if (!t || !s_ta || !s_counter) return;
  otp::Core& c = OtpMessenger::core();
  const char* text = lv_textarea_get_text(s_ta);
  const int typed = (int)strlen(text ? text : "");
  const int name_len = (int)strlen(the_mesh.getNodePrefs()->node_name);
  const int avail = c.available(*t);
  char buf[96];
  bool bad = false;
  if (t->kind == otp::Kind::Channel) {
    const int cap = otp::maxChannelBytes(name_len);
    snprintf(buf, sizeof buf, "%d / %d  ·  %d B pad", typed, cap, avail);
    bad = typed > cap || typed > avail;
  } else {
    const int ccap = otp::maxChunkBytes(name_len);
    const int parts = typed ? (typed + ccap - 1) / ccap : 0;
    const int need = typed + parts * 12;
    if (parts > 1) snprintf(buf, sizeof buf, "%d parts  ·  %d of %d B", parts, need, avail);
    else snprintf(buf, sizeof buf, "%d of %d B", need, avail);
    bad = need > avail;
  }
  // Only while there is something to weigh: the header already says what is left.
  if (!typed && !bad) { lv_obj_add_flag(s_counter, LV_OBJ_FLAG_HIDDEN); return; }
  lv_obj_clear_flag(s_counter, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s_counter, buf);
  lv_obj_set_style_text_color(s_counter, lv_color_hex(bad ? COLOR_STATUS_DANGER_TEXT : COLOR_SUB), LV_PART_MAIN);
}

static void sendNow() {
  otp::Target* t = cur();
  if (!t || !s_ta) return;
  kbMirrorSyncToReal();
  const char* text = lv_textarea_get_text(s_ta);
  if (!text || !*text) return;
  char err[160];
  const otp::SendResult r = OtpMessenger::core().send(*t, text, strlen(text), err, sizeof err);
  if (r != otp::SendResult::Ok) { toast(err, 2600); return; }
  setTextareaSynced(s_ta, "");
  refreshCounter();
}

// A bubble's status word colour (yours) by its state.
static uint32_t stateColor(otp::State s) {
  switch (s) {
    case otp::State::Delivered: return ink(plainLook() ? COLOR_GLOW : th().ok);
    case otp::State::Retrying:  return COLOR_STATUS_WARN;
    case otp::State::Failed: case otp::State::Lost: return COLOR_STATUS_DANGER_TEXT;
    default: return COLOR_TERTIARY;
  }
}

static void buildBubbles(bool keep_scroll) {
  otp::Target* t = cur();
  if (!t || !s_list) return;
  otp::Core& c = OtpMessenger::core();
  c.ensureLog(*t);
  const lv_coord_t old_y = lv_obj_get_scroll_y(s_list);
  const lv_coord_t old_bottom = lv_obj_get_scroll_bottom(s_list);
  lv_obj_clean(s_list);
  const lv_coord_t w = lv_obj_get_content_width(s_list);
  const lv_coord_t max_w = w * 4 / 5;
  const int first = t->log_n > s_shown_n ? t->log_n - s_shown_n : 0;
  if (first > 0) {
    lv_obj_t* wrap = box(s_list);
    lv_obj_set_size(wrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* more = lv_btn_create(wrap);
    styleButton(more);
    lv_obj_set_size(more, LV_SIZE_CONTENT, SC(24));
    lv_obj_set_style_pad_hor(more, SC(12), LV_PART_MAIN);
    lv_obj_set_style_radius(more, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(more, [](lv_event_t*) {
      s_shown_n += 60;
      lv_async_call([](void*) { buildBubbles(false); }, nullptr);   // not from inside the list being rebuilt
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* ml = label(more, TR("Earlier messages"), &g_font_12, COLOR_SUB);
    lv_obj_center(ml);
  }
  if (!t->log_n) {
    emptyState(s_list, c.hasPad(*t) ? UI_ICON_SHIELD_CHECK : UI_ICON_LOCK,
               c.hasPad(*t) ? TR("No messages yet") : TR("No pad yet"),
               c.hasPad(*t) ? TR("Everything you send here is one-time-pad encrypted.")
                            : TR("Open the pad to generate or import one."));
    lv_obj_t* last = lv_obj_get_child(s_list, -1);
    if (last) lv_obj_set_width(last, LV_PCT(100));
  }
  const bool plain = plainLook();
  const bool colorful = !plain && touchPrefsGetColorfulBubbles();
  const lv_font_t* mf = chatMessageFont();
  for (int i = first; i < t->log_n; i++) {
    const otp::Entry& e = t->log[i];
    const bool own = e.mine;
    const char* who = e.who[0] ? e.who : t->name;
    // Yours on the right in the sent colour, theirs on the left, as in Chats.
    lv_color_t bg = lv_color_hex(own ? COLOR_CHAT_SENT_BG : COLOR_CHAT_RECV_BG);
    if (colorful) usernameBubbleColors(own ? the_mesh.getNodePrefs()->node_name : who, &bg, nullptr);
    lv_obj_t* wrap = box(s_list);
    lv_obj_set_size(wrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    const lv_flex_align_t side = own ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START;
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START, side, side);
    lv_obj_set_style_pad_row(wrap, 2, LV_PART_MAIN);
    lv_obj_t* bub = box(wrap);
    lv_obj_set_size(bub, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bub, max_w, LV_PART_MAIN);
    lv_obj_set_flex_flow(bub, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_radius(bub, 9, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bub, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bub, 5, LV_PART_MAIN);
#if defined(HAS_TDECK_PRO)
    lv_obj_set_style_bg_opa(bub, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(bub, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(bub, 2, LV_PART_MAIN);
#else
    lv_obj_set_style_bg_color(bub, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, LV_PART_MAIN);
    if (s_theme_high_contrast) {
      lv_obj_set_style_border_color(bub, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
      lv_obj_set_style_border_width(bub, 2, LV_PART_MAIN);
    } else if (own && (e.state == otp::State::Failed || e.state == otp::State::Lost)) {
      lv_obj_set_style_border_color(bub, lv_color_hex(COLOR_STATUS_DANGER), LV_PART_MAIN);
      lv_obj_set_style_border_width(bub, 1, LV_PART_MAIN);
    }
#endif
    // Still on its way: a little faded until it lands.
    if (own && (e.state == otp::State::Pending || e.state == otp::State::Retrying || e.state == otp::State::Waiting))
      lv_obj_set_style_opa(bub, LV_OPA_70, LV_PART_MAIN);
    // The sender, in their colour, inside the bubble on a channel.
    if (!own && t->kind == otp::Kind::Channel) {
      char nm[40];
      copyUtf8ReplacingMissingGlyphs(&g_font_semi_12, nm, sizeof nm, who);
      lv_color_t nc = lv_color_hex(COLOR_GLOW);
      if (!plain) usernameBubbleColors(who, nullptr, &nc);
      else nc = lv_color_hex(COLOR_CHAT_TEXT);
      lv_obj_t* sl = lv_label_create(bub);
      lv_label_set_text(sl, nm);
      lv_obj_set_style_text_font(sl, &g_font_semi_12, LV_PART_MAIN);
      lv_obj_set_style_text_color(sl, nc, LV_PART_MAIN);
      lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
      lv_obj_set_style_max_width(sl, max_w - 16, LV_PART_MAIN);
    }
    lv_obj_t* tl = lv_label_create(bub);
    lv_label_set_text(tl, e.text ? e.text : "");
    lv_obj_set_style_text_font(tl, mf, LV_PART_MAIN);
#if defined(HAS_TDECK_PRO)
    lv_obj_set_style_text_color(tl, lv_color_black(), LV_PART_MAIN);
#else
    lv_obj_set_style_text_color(tl, lv_color_hex(COLOR_CHAT_TEXT), LV_PART_MAIN);
#endif
    useChainedFont(tl);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(tl, max_w - 16, LV_PART_MAIN);
    // Under it: the time, and for yours the delivery state in its colour.
    char when[24], cap[96], sw[48];
    fmtAgo(e.at_epoch, e.at_ms, when, sizeof when);
    const char* st = own ? c.statusWord(*t, e, sw, sizeof sw) : nullptr;
    if (st && *st) snprintf(cap, sizeof cap, "#%06lX %s#  #%06lX %s#", (unsigned long)stateColor(e.state), st,
                            (unsigned long)COLOR_TERTIARY, when);
    else snprintf(cap, sizeof cap, "#%06lX %s#", (unsigned long)COLOR_TERTIARY, when);
    lv_obj_t* cl = label(wrap, cap, &g_font_12, COLOR_TERTIARY);
    lv_label_set_recolor(cl, true);
    lv_obj_set_style_pad_hor(cl, 2, LV_PART_MAIN);
  }
  themeTree(s_list);
  lv_obj_update_layout(s_list);
  if (keep_scroll && old_bottom > 4) lv_obj_scroll_to_y(s_list, old_y, LV_ANIM_OFF);
  else lv_obj_scroll_to_y(s_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void buildChat() {
  otp::Target* t = cur();
  if (!t) { show(Screen::List); return; }
  otp::Core& c = OtpMessenger::core();
  c.setOpenTarget(t);
  s_shown_n = 60;
  lv_obj_t* b = newBody(false);
  const lv_coord_t W = lv_obj_get_width(s_root), H = lv_obj_get_height(s_root);
  const bool channel = t->kind == otp::Kind::Channel;

  // Header, as the open chat's: the glyph, the name in SemiBold, a status line
  // under it, and the conversation's actions on the right.
  const lv_coord_t hh = LV_MAX(SC(40), (lv_coord_t)(lv_font_get_line_height(&g_font_semi_14) + lv_font_get_line_height(&g_font_12) + SC(8)));
  const lv_coord_t av = SC(28), btn = SC(30);
  lv_obj_t* hdr = box(b);
  lv_obj_set_size(hdr, W, hh);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_t* a = avatar(hdr, *t, av);
  lv_obj_align(a, LV_ALIGN_LEFT_MID, 8, 0);
  lv_obj_t* actions = box(hdr);
  lv_obj_set_size(actions, LV_SIZE_CONTENT, btn);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(actions, 2, LV_PART_MAIN);
  lv_obj_align(actions, LV_ALIGN_RIGHT_MID, -6, 0);
  if (channel) {
    s_parts_btn = iconBtn(actions, UI_ICON_USERS, [](lv_event_t*) {
      otp::Core& cc = OtpMessenger::core();
      cc.show_participants = !cc.show_participants;
      cc.saveSettings();
      show(Screen::Chat);   // the strip changes the list's height
    });
  }
  iconBtn(actions, UI_ICON_REFRESH_CW, [](lv_event_t*) {
    if (otp::Target* x = cur()) { OtpMessenger::core().requestSyncCheck(*x); toast(TR("Sync check sent"), 1200); }
  });
  iconBtn(actions, UI_ICON_SHIELD_CHECK, [](lv_event_t*) { show(Screen::Pad); });
  const int nbtn = channel ? 3 : 2;
  const lv_coord_t tx = 8 + av + 8;
  const lv_coord_t tw = LV_MAX((lv_coord_t)40, (lv_coord_t)(W - tx - 6 - nbtn * (btn + 2) - 6));
  char nm[56];
  displayName(*t, nm, sizeof nm);
  lv_obj_t* tl = label(hdr, nm, &g_font_semi_14, COLOR_TEXT);
  lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
  lv_obj_set_size(tl, tw, lv_font_get_line_height(&g_font_semi_14));
  const lv_coord_t top = (hh - lv_font_get_line_height(&g_font_semi_14) - lv_font_get_line_height(&g_font_12)) / 2;
  lv_obj_set_pos(tl, tx, top);
  s_info = lv_label_create(hdr);
  lv_label_set_recolor(s_info, true);
  lv_label_set_text(s_info, "");
  lv_obj_set_style_text_font(s_info, &g_font_12, LV_PART_MAIN);
  lv_label_set_long_mode(s_info, LV_LABEL_LONG_DOT);
  lv_obj_set_size(s_info, tw, lv_font_get_line_height(&g_font_12));
  lv_obj_set_pos(s_info, tx, top + lv_font_get_line_height(&g_font_semi_14));

  // The pad gauge, a hairline under the header.
  lv_coord_t y = hh;
  s_bar = lv_bar_create(b);
  lv_obj_set_size(s_bar, W - 16, 3);
  lv_obj_set_pos(s_bar, 8, y);
  lv_obj_set_style_radius(s_bar, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(s_bar, 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
  y += 3 + SC(2);
  // Who is on the channel, when asked for.
  if (channel && c.show_participants) {
    s_parts = box(b);
    lv_obj_set_size(s_parts, W - 16, lv_font_get_line_height(&g_font_12) + 4);
    lv_obj_set_pos(s_parts, 8, y);
    lv_obj_set_flex_flow(s_parts, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_parts, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_parts, SC(10), LV_PART_MAIN);
    lv_obj_add_event_cb(s_parts, [](lv_event_t* e) {
      lv_obj_t* o = lv_event_get_target(e);
      free(lv_obj_get_user_data(o));
      lv_obj_set_user_data(o, nullptr);
    }, LV_EVENT_DELETE, nullptr);
    y += lv_font_get_line_height(&g_font_12) + 4;
  }

  // The composer: a pill that grows no further than one line, and the send button.
  const lv_coord_t send = SC(30), comp_h = send + SC(10);
  const lv_coord_t cy = H - comp_h + SC(5);
  s_ta = lv_textarea_create(b);
  lv_obj_set_size(s_ta, W - 8 - 8 - send - 6, send);
  lv_obj_set_pos(s_ta, 8, cy);
  styleCard(s_ta);
  lv_obj_set_style_bg_color(s_ta, lv_color_hex(COLOR_FIELD), LV_PART_MAIN);
  lv_obj_set_style_radius(s_ta, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(s_ta, SC(12), LV_PART_MAIN);
  lv_obj_set_style_pad_ver(s_ta, 3, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(s_ta, LV_SCROLLBAR_MODE_OFF);
  lv_textarea_set_one_line(s_ta, true);
  lv_textarea_set_max_length(s_ta, channel ? 65 : 2000);
  lv_obj_set_style_text_font(s_ta, &g_font_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_ta, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  if (lv_obj_t* tlb = lv_textarea_get_label(s_ta)) lv_obj_set_style_pad_top(tlb, 4, LV_PART_MAIN);
#if !defined(HAS_TDECK_PRO)
  if (!s_theme_high_contrast) {
    const lv_color_t edge = lv_color_mix(lv_color_hex(accent()), lv_color_hex(COLOR_BORDER), 90);
    for (lv_style_selector_t st : { (lv_style_selector_t)(LV_PART_MAIN | LV_STATE_FOCUSED),
                                    (lv_style_selector_t)(LV_PART_MAIN | LV_STATE_FOCUS_KEY) })
      lv_obj_set_style_border_color(s_ta, edge, st);
  }
#endif
  {
    char ph[64];
    if (c.hasPad(*t)) snprintf(ph, sizeof ph, "%s %s", TR("Encrypted to"), nm);
    else snprintf(ph, sizeof ph, "%s", TR("No pad yet"));
    taSetPlaceholder(s_ta, ph);
  }
  attachSettingsTaEvents(s_ta);
  lv_obj_add_event_cb(s_ta, [](lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) sendNow();
    else if (code == LV_EVENT_VALUE_CHANGED) refreshCounter();
    else if (code == LV_EVENT_FOCUSED) { if (otp::Target* x = cur()) x->next_sync_check = 0; }
  }, LV_EVENT_ALL, nullptr);
  s_send_btn = roundAccent(b, UI_ICON_ARROW_UP, [](lv_event_t*) { sendNow(); }, send);
  lv_obj_set_pos(s_send_btn, W - 8 - send, cy);

  // Bubbles fill the middle; the byte budget floats over their bottom while typing.
  s_list = lv_obj_create(b);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_pos(s_list, 0, y);
  lv_obj_set_size(s_list, W, H - y - comp_h);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(s_list, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(s_list, SC(4), LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_list, SC(6), LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  s_counter = label(b, "", &g_font_12, COLOR_SUB);
  lv_obj_set_style_bg_color(s_counter, lv_color_hex(COLOR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_counter, LV_OPA_80, LV_PART_MAIN);
  lv_obj_set_style_radius(s_counter, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(s_counter, SC(6), LV_PART_MAIN);
  lv_obj_set_style_pad_ver(s_counter, 1, LV_PART_MAIN);
  lv_obj_align(s_counter, LV_ALIGN_BOTTOM_RIGHT, -8, -(comp_h + 2));
  lv_obj_add_flag(s_counter, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_ta);
  lv_obj_move_foreground(s_send_btn);
  refreshInfo();
  refreshCounter();
  buildBubbles(false);
  if (g_lv.keyboard) kbMirrorBind(s_ta);
}

// ---- Pad as QR codes ---------------------------------------------------------------------------
// The unused pad as a loop of QR codes (PadQr.h) for MeshCore Open TAK's
// OTP Pad > Scan QR: each part a few hundred bytes, shown in turn until the
// phone has them all. White below the status bar, so the codes have a clean margin.
constexpr int kQrMaxVersion = 13;
static lv_obj_t* s_qr = nullptr;
static lv_obj_t* s_qr_canvas = nullptr;
static lv_obj_t* s_qr_part = nullptr;
static lv_obj_t* s_qr_pause = nullptr;
static lv_timer_t* s_qr_timer = nullptr;
static uint8_t* s_qr_buf = nullptr;      // the canvas: 1-bit indexed
static char* s_qr_hex = nullptr;         // the pad as hex (wiped on close)
static size_t s_qr_hex_len = 0;
static uint32_t s_qr_crc = 0;
static size_t s_qr_chunk = 0;
static int s_qr_i = 1, s_qr_n = 1, s_qr_ver = 1, s_qr_scale = 3, s_qr_px = 0;
static bool s_qr_paused = false;

static void qrClose() {
  if (s_qr_timer) { lv_timer_del(s_qr_timer); s_qr_timer = nullptr; }
  if (s_qr) { lv_obj_del(s_qr); s_qr = nullptr; }
  s_qr_canvas = s_qr_part = s_qr_pause = nullptr;
  if (s_qr_hex) { memset(s_qr_hex, 0, s_qr_hex_len); heap_caps_free(s_qr_hex); s_qr_hex = nullptr; }
  if (s_qr_buf) {
    memset(s_qr_buf, 0, LV_CANVAS_BUF_SIZE_INDEXED_1BIT(s_qr_px, s_qr_px));
    heap_caps_free(s_qr_buf);
    s_qr_buf = nullptr;
  }
  s_qr_hex_len = 0;
}

static void qrDraw() {
  if (!s_qr_canvas || !s_qr_hex) return;
  char text[padqr::kHeaderMax + 2 * 600];
  if (!padqr::frame(s_qr_hex, s_qr_hex_len, s_qr_crc, s_qr_i, s_qr_n, s_qr_chunk, text, sizeof text)) return;
  uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(kQrMaxVersion)];
  uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(kQrMaxVersion)];
  const bool ok = qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, s_qr_ver, s_qr_ver, qrcodegen_Mask_AUTO, false);
  memset(text, 0, sizeof text);
  if (!ok) return;
  const int m = qrcodegen_getSize(qr);
  const int stride = (s_qr_px + 7) / 8;
  uint8_t* px = s_qr_buf + 8;   // after the two palette entries
  memset(px, 0, (size_t)stride * s_qr_px);
  const int off = (s_qr_px - m * s_qr_scale) / 2;
  for (int y = 0; y < m; y++) {
    for (int x = 0; x < m; x++) {
      if (!qrcodegen_getModule(qr, x, y)) continue;
      for (int dy = 0; dy < s_qr_scale; dy++) {
        uint8_t* row = px + (size_t)(off + y * s_qr_scale + dy) * stride;
        for (int dx = 0; dx < s_qr_scale; dx++) {
          const int X = off + x * s_qr_scale + dx;
          row[X >> 3] |= (uint8_t)(0x80 >> (X & 7));
        }
      }
    }
  }
  memset(qr, 0, sizeof qr);
  memset(tmp, 0, sizeof tmp);
  lv_obj_invalidate(s_qr_canvas);
  if (s_qr_part) {
    char l[32];
    snprintf(l, sizeof l, "Part %d of %d", s_qr_i, s_qr_n);
    lv_label_set_text(s_qr_part, l);
  }
}

static void qrStep(int d) {
  s_qr_i = ((s_qr_i - 1 + d) % s_qr_n + s_qr_n) % s_qr_n + 1;
  qrDraw();
}

static void qrPause(bool paused) {
  s_qr_paused = paused;
  if (s_qr_pause) lv_label_set_text(s_qr_pause, paused ? "Play" : "Pause");
}

static lv_obj_t* qrBtn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, lv_coord_t w, lv_obj_t** lbl_out = nullptr) {
  lv_obj_t* b = lv_btn_create(parent);
  styleButton(b);
  lv_obj_set_size(b, w, SC(30));
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  useChainedFont(l);
  lv_obj_center(l);
  if (lbl_out) *lbl_out = l;
  return b;
}

static void qrOpen() {
  otp::Target* t = cur();
  if (!t) return;
  otp::Bytes hex;
  if (!OtpMessenger::core().exportHex(*t, hex) || !hex.n) {
    toast(TR("Only an unused pad can be shown: its bytes would be out of step on the other side"), 3000);
    return;
  }
  qrClose();
  s_qr_hex = (char*)heap_caps_malloc(hex.n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_qr_hex) { memset(hex.p, 0, hex.n); toast(TR("Out of memory")); return; }
  memcpy(s_qr_hex, hex.p, hex.n);
  s_qr_hex[hex.n] = 0;
  s_qr_hex_len = hex.n;
  memset(hex.p, 0, hex.n);

  // Landscape screens put the code on the left and the controls beside it.
  // Everything sits below the status bar, which is drawn over the top layer:
  // a full-screen overlay lost its title, the part counter and the code's top
  // rows under it.
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr), sh = appPageContentH();
  const bool side = sw - sh >= SC(80);
  const lv_coord_t avail = side ? LV_MIN(sh - 8, sw - SC(96)) : LV_MIN(sw - 8, sh - SC(30) - SC(40));
  s_qr_ver = 0;
  for (int scale = 6; scale >= 2 && !s_qr_ver; scale--) {
    const int v = padqr::versionFor(avail, scale, kQrMaxVersion);
    if (v >= 10 || (scale == 2 && v)) { s_qr_ver = v; s_qr_scale = scale; }
  }
  if (!s_qr_ver) { qrClose(); toast(TR("Screen too small for a QR code")); return; }
  s_qr_px = (padqr::modules(s_qr_ver) + 4) * s_qr_scale;
  s_qr_chunk = padqr::chunkBytes(s_qr_ver);
  s_qr_n = padqr::partsFor(s_qr_hex_len / 2, s_qr_chunk);
  s_qr_crc = padqr::crc32Hex(s_qr_hex, s_qr_hex_len);
  s_qr_i = 1;
  s_qr_paused = false;
  s_qr_buf = (uint8_t*)heap_caps_malloc(LV_CANVAS_BUF_SIZE_INDEXED_1BIT(s_qr_px, s_qr_px), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_qr_buf) { qrClose(); toast(TR("Out of memory")); return; }

  s_qr = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_qr);
  lv_obj_set_size(s_qr, sw, sh);
  lv_obj_set_pos(s_qr, 0, appPageContentTop());
  lv_obj_set_style_bg_color(s_qr, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_qr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_qr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_qr, LV_OBJ_FLAG_SCROLLABLE);

  s_qr_canvas = lv_canvas_create(s_qr);
  lv_canvas_set_buffer(s_qr_canvas, s_qr_buf, s_qr_px, s_qr_px, LV_IMG_CF_INDEXED_1BIT);
  lv_canvas_set_palette(s_qr_canvas, 0, lv_color_white());
  lv_canvas_set_palette(s_qr_canvas, 1, lv_color_black());
  lv_obj_add_flag(s_qr_canvas, LV_OBJ_FLAG_CLICKABLE);   // tap the code: pause / go on
  lv_obj_add_event_cb(s_qr_canvas, [](lv_event_t*) { qrPause(!s_qr_paused); }, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* col = lv_obj_create(s_qr);
  lv_obj_remove_style_all(col);
  lv_obj_set_flex_flow(col, side ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW_WRAP);
  // From the top beside the code: centred, an overflow would be cut off above as well as below.
  lv_obj_set_flex_align(col, side ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, SC(4), LV_PART_MAIN);
  lv_obj_set_style_pad_column(col, SC(6), LV_PART_MAIN);
  lv_obj_set_style_pad_ver(col, SC(4), LV_PART_MAIN);
  lv_obj_set_scroll_dir(col, LV_DIR_VER);   // a short screen scrolls the hint rather than cutting it off
  lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
  const lv_coord_t bw = side ? sw - s_qr_px - SC(14) : (sw - SC(30)) / 3;
  if (side) {
    lv_obj_set_pos(s_qr_canvas, 4, (sh - s_qr_px) / 2);
    lv_obj_set_size(col, bw, sh);
    lv_obj_set_pos(col, s_qr_px + SC(9), 0);
  } else {
    lv_obj_set_pos(s_qr_canvas, (sw - s_qr_px) / 2, SC(26));
    lv_obj_set_size(col, sw, sh - s_qr_px - SC(26));
    lv_obj_set_pos(col, 0, s_qr_px + SC(26));
  }
  lv_obj_t* title = lv_label_create(side ? col : s_qr);
  char nm[56];
  displayName(*t, nm, sizeof nm);
  lv_label_set_text(title, nm);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, side ? bw : sw - 12);
  lv_obj_set_style_text_font(title, &g_font_semi_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  if (!side) lv_obj_set_pos(title, 6, SC(4));
  s_qr_part = lv_label_create(side ? col : s_qr);
  lv_obj_set_style_text_font(s_qr_part, &g_font_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_qr_part, lv_color_hex(0x333333), LV_PART_MAIN);
  if (!side) lv_obj_align(s_qr_part, LV_ALIGN_TOP_RIGHT, -6, SC(6));
  lv_obj_t* pb = qrBtn(col, "Pause", [](lv_event_t*) { qrPause(!s_qr_paused); }, bw, &s_qr_pause);
  lv_obj_t* nb = qrBtn(col, "Next", [](lv_event_t*) { qrPause(true); qrStep(1); }, bw);
  qrBtn(col, "Close", [](lv_event_t*) { qrClose(); }, bw);
  if (side) {
    lv_obj_t* hint = lv_label_create(col);
    lv_label_set_text(hint, TR("Scan in Open TAK: OTP Pad > Scan QR. Keep it private."));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, bw);
    lv_obj_set_style_text_font(hint, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    useChainedFont(hint);
  }
  if (s_qr_n == 1) {   // one code: nothing to step through
    lv_obj_add_flag(pb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nb, LV_OBJ_FLAG_HIDDEN);
  }
  qrDraw();
  s_qr_timer = lv_timer_create([](lv_timer_t*) { if (!s_qr_paused && s_qr_n > 1) qrStep(1); }, 1400, nullptr);
  lv_obj_move_foreground(s_qr);
}

// ---- Pad --------------------------------------------------------------------------------
static void importHexText(const char* text) {
  otp::Target* t = cur();
  if (!t || !text) return;
  char err[160];
  if (!OtpMessenger::core().importHex(*t, text, strlen(text), err, sizeof err)) { toast(err, 2600); return; }
  char msg[64]; snprintf(msg, sizeof msg, "Imported %d bytes", OtpMessenger::core().available(*t));
  toast(msg);
  show(Screen::Pad);
}
static void addTextBytes(const char* text) {
  otp::Target* t = cur();
  if (!t || !text) return;
  char err[160];
  if (!OtpMessenger::core().addBytes(*t, (const uint8_t*)text, strlen(text), err, sizeof err)) { toast(err, 2600); return; }
  show(Screen::Pad);
}
static void wipeConfirmed() {
  if (otp::Target* t = cur()) { OtpMessenger::core().wipe(*t); toast(TR("Pad wiped")); show(Screen::Pad); }
}
static void forgetConfirmed() {
  if (otp::Target* t = cur()) OtpMessenger::core().forget(*t);
  s_tid[0] = 0;
  show(Screen::List);
}
static void exportPadNow() {
  otp::Target* t = cur();
  if (!t) return;
  fs::FS* sd = backupSdFs();
  if (!sd) { toast(TR("No SD card"), 1800); return; }
  char path[64];
  char safe[33];
  size_t j = 0;
  for (const char* p = t->name; *p && j < 24; ++p) {
    const char ch = *p;
    safe[j++] = ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) ? ch : '_';
  }
  safe[j] = 0;
  snprintf(path, sizeof path, "/otp_%s.hex", safe);
  if (!OtpMessenger::exportPad(*t, sd, path)) {
    toast(TR("Only an unused pad can be exported"), 2400);
    return;
  }
  char msg[96];
  snprintf(msg, sizeof msg, "Saved %s to the SD card. Import it on the other device, then delete it.", path);
  toast(msg, 3500);
}

// Pad sizes to generate, as in MeshCore Open TAK (1700 B still fits one QR
// code on a phone). Remembered while the device is on; 4096 B to start.
static const int kGenSizes[] = {512, 1700, 4096, otp::kMaxTotalPadBytes};
constexpr int kGenSizeCount = sizeof kGenSizes / sizeof kGenSizes[0];
static uint8_t s_gen_size = 2;

// ---- checking a pad (otp::padFingerprint) ----------------------------------------------------
// The check code is on the pad screen to read out; with Sound link it can be
// played, and a code heard by Sound link is compared here.
static char s_chk_tid[48] = "";      // the pad the last result is about
static char s_chk_msg[120] = "";
static int8_t s_chk_ok = -1;
static char s_chk_listen[48] = "";   // the pad a "Listen for a check" was started from

// A check code was heard: compare it with the pad being checked, else every
// pad here. `out` gets what to tell the user.
[[maybe_unused]] static void onPadCheckHeard(const uint8_t fp[8], uint32_t n, char* out, size_t cap) {
  otp::Core& c = OtpMessenger::core();
  uint8_t mine[8];
  uint32_t mn = 0;
  char nm[56];
  if (!OtpMessenger::ready() || c.locked()) {
    snprintf(out, cap, "%s", TR("Pad check heard: unlock OTP Messenger to compare it"));
    toast(out, 3000);
    return;
  }
  otp::Target* want = s_chk_listen[0] ? c.findById(s_chk_listen) : nullptr;
  s_chk_listen[0] = 0;
  otp::Target* hit = nullptr;
  if (want && c.padCheck(*want, mine, &mn)) {
    hit = want;
    displayName(*want, nm, sizeof nm);
    s_chk_ok = !memcmp(mine, fp, 8);
    if (s_chk_ok) snprintf(out, cap, "%s %s", TR("Same pad on both sides:"), nm);
    else if (n && n != mn)
      snprintf(out, cap, "%s %s: %lu / %lu B. %s", TR("Different pad for"), nm, (unsigned long)n, (unsigned long)mn,
               TR("Share it again."));
    else snprintf(out, cap, "%s %s. %s", TR("Different pad bytes for"), nm, TR("Share it again before using it."));
  } else {
    s_chk_ok = 0;
    for (int i = 0; i < c.targetCount() && !hit; i++) {
      otp::Target* t = c.targetAt(i);
      if (t && t->used && c.padCheck(*t, mine, &mn) && !memcmp(mine, fp, 8)) hit = t;
    }
    if (hit) {
      s_chk_ok = 1;
      displayName(*hit, nm, sizeof nm);
      snprintf(out, cap, "%s %s", TR("Same pad on both sides:"), nm);
    } else {
      snprintf(out, cap, "%s", TR("No pad here matches that check code"));
    }
  }
  snprintf(s_chk_msg, sizeof s_chk_msg, "%s", out);
  snprintf(s_chk_tid, sizeof s_chk_tid, "%s", hit ? hit->id : "");
  toast(out, 3500);
  if (s_root && s_screen == Screen::Pad) show(Screen::Pad);
}

static void buildPad() {
  otp::Target* t = cur();
  if (!t) { show(Screen::List); return; }
  otp::Core& c = OtpMessenger::core();
  c.setOpenTarget(t);
  lv_obj_t* b = newBody();
  const int avail = c.available(*t);
  const bool has = c.hasPad(*t);
  char line[200];

  // Who the pad is with: the glyph, the name, what it is.
  lv_obj_t* who = box(b);
  lv_obj_set_size(who, rowW(), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_bottom(who, SC(4), LV_PART_MAIN);
  lv_obj_t* a = avatar(who, *t, SC(36));
  lv_obj_align(a, LV_ALIGN_LEFT_MID, 2, 0);
  char nm[56];
  displayName(*t, nm, sizeof nm);
  lv_obj_t* nl = label(who, nm, &g_font_semi_16, COLOR_TEXT);
  lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(nl, rowW() - SC(36) - SC(14));
  lv_obj_set_pos(nl, SC(36) + SC(12), 0);
  lv_obj_t* kl = label(who, t->kind == otp::Kind::Channel ? TR("Channel pad, shared by everyone on it") : TR("One-time pad"),
                       &g_font_12, COLOR_SUB);
  lv_obj_set_pos(kl, SC(36) + SC(12), lv_font_get_line_height(&g_font_semi_16));
  lv_obj_set_height(who, LV_MAX(SC(36), (lv_coord_t)(lv_font_get_line_height(&g_font_semi_16) + lv_font_get_line_height(&g_font_12))));

  // The pad: what is left, a gauge, the counters, sync.
  lv_obj_t* card = setCardIn(b, "");
  lv_obj_t* big = box(card);
  lv_obj_set_size(big, s_settings_content_w, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(big, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(big, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(big, SC(6), LV_PART_MAIN);
  if (has && avail > 0) {
    snprintf(line, sizeof line, "%d", avail);
    label(big, line, &g_font_semi_16, padColor(avail));
    snprintf(line, sizeof line, "B left of %u", (unsigned)t->pad.n);
    label(big, line, &g_font_12, COLOR_SUB);
  } else {
    label(big, has ? TR("Pad used up") : TR("No pad yet"), &g_font_semi_16, has ? COLOR_STATUS_DANGER_TEXT : COLOR_TEXT);
  }
  if (t->pad.n > 0) {
    lv_obj_t* g = lv_bar_create(card);
    lv_obj_set_size(g, s_settings_content_w, SC(6));
    lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, LV_PART_MAIN);
    lv_bar_set_range(g, 0, (int32_t)t->pad.n);
    lv_bar_set_value(g, avail, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g, lv_color_hex(avail < otp::kLowPadBytes ? (avail <= 0 ? COLOR_STATUS_DANGER : COLOR_STATUS_WARN) : accent()),
                              LV_PART_INDICATOR);
  }
  if (t->kind == otp::Kind::Dm) {
    const char ar = c.autoRole(*t);
    snprintf(line, sizeof line, "Role %c%s  ·  sent %u B  ·  received %u B", t->role,
             ar == t->role ? " (auto)" : " (manual)", (unsigned)t->ctr_mine, (unsigned)t->ctr_theirs);
  } else {
    snprintf(line, sizeof line, "Offset %u  ·  shared by everyone on #%s", (unsigned)t->ctr, t->name);
  }
  setNote(card, line);
  snprintf(line, sizeof line, "%s %s", TR("Sync:"), t->sync_text[0] ? t->sync_text : TR("not checked yet"));
  setNote(card, line, t->sync_ok == 1 ? SET_TONE_OK : t->sync_ok == 0 ? SET_TONE_DANGER : SET_TONE_INFO);
  if (t->wq_n) {
    snprintf(line, sizeof line, "%d message(s) waiting for a path", t->wq_n);
    setNote(card, line, SET_TONE_WARN);
  }
  if (has && avail > 0)
    setButton(card, UI_ICON_MESSAGE_SQUARE, "Open chat", [](lv_event_t*) { show(Screen::Chat); }, nullptr, SET_PRIMARY);
  setButton(card, UI_ICON_REFRESH_CW, "Check sync", [](lv_event_t*) {
    if (otp::Target* x = cur()) { OtpMessenger::core().requestSyncCheck(*x); show(Screen::Pad); }
  });

  lv_obj_t* add = setCardIn(b, TR("Add pad bytes"));
  // Generate: the same sizes as MeshCore Open TAK's pad screen. Tap the size
  // to change it; Generate adds that many bytes (up to the pad cap).
  {
    char sz[24];
    const int gs = kGenSizes[s_gen_size % kGenSizeCount];
    snprintf(sz, sizeof sz, gs == otp::kMaxTotalPadBytes ? "%d B (max)" : gs == 1700 ? "%d B (QR-safe)" : "%d B", gs);
    setActionRow(add, UI_ICON_HASH, "Size to generate", sz, [](lv_event_t*) {
      s_gen_size = (uint8_t)((s_gen_size + 1) % kGenSizeCount);
      show(Screen::Pad);
    }, nullptr, SET_NORMAL, false);
    char gl[40];
    snprintf(gl, sizeof gl, "Generate %d random bytes", gs);
    setActionRow(add, UI_ICON_ZAP, gl, nullptr, [](lv_event_t*) {
      otp::Target* x = cur();
      if (!x) return;
      const int want = kGenSizes[s_gen_size % kGenSizeCount];
      const int n = OtpMessenger::core().generate(*x, want);
      char msg[96];
      if (n == want) snprintf(msg, sizeof msg, "Added %d random bytes (hardware RNG)", n);
      else if (n > 0) snprintf(msg, sizeof msg, "Added %d random bytes: the pad is now at the %d byte cap", n, otp::kMaxTotalPadBytes);
      else snprintf(msg, sizeof msg, "The pad is at the %d byte cap", otp::kMaxTotalPadBytes);
      toast(msg, 2200);
      show(Screen::Pad);
    }, nullptr, SET_NORMAL, false);
  }
  setActionRow(add, UI_ICON_FOLDER, "Import a file from the SD card", nullptr, [](lv_event_t*) { show(Screen::Import); });
  setActionRow(add, UI_ICON_COPY, "Paste hex", nullptr, [](lv_event_t*) {
    prompt(TR("Pad as hex"), TR("Spaces and line breaks are ignored."), false, 16400, importHexText);
  });
  setActionRow(add, UI_ICON_FILE_TEXT, "Add text", nullptr, [](lv_event_t*) {
    prompt(TR("Pad text"), TR("Typed characters become pad bytes as-is. Both sides must add exactly the same text."),
           false, 2000, addTextBytes);
  });

  lv_obj_t* share = setCardIn(b, TR("Share"));
  setActionRow(share, UI_ICON_QR_CODE, "Show as QR code", nullptr, [](lv_event_t*) { qrOpen(); }, nullptr, SET_NORMAL, false);
  {
    uint8_t fp[8];
    uint32_t pn = 0;
    if (c.padCheck(*t, fp, &pn)) {
      char code[20];
      otp::padCheckCode(fp, code);
      setActionRow(share, UI_ICON_CHECK_CHECK, "Check code", code, nullptr, nullptr, SET_NORMAL, false);
#if CAP_SOUND_LINK
      setActionRow(share, UI_ICON_WAVES, "Play the check code", nullptr, [](lv_event_t*) {
        otp::Target* x = cur();
        uint8_t f[8];
        uint32_t n = 0;
        char link[80];
        if (!x || !OtpMessenger::core().padCheck(*x, f, &n) || !otp::padCheckLink(f, n, link, sizeof link)) return;
        if (soundui::playRaw(link)) toast(TR("Playing the check code"), 1500);
      }, nullptr, SET_NORMAL, false);
      setActionRow(share, UI_ICON_WAVES, "Listen for a check code", nullptr, [](lv_event_t*) {
        otp::Target* x = cur();
        if (!x) return;
        snprintf(s_chk_listen, sizeof s_chk_listen, "%s", x->id);
        soundui::listenFor(120000);
        toast(TR("Listening for 2 minutes: play the check code on the other device"), 2500);
      }, nullptr, SET_NORMAL, false);
#endif
      if (s_chk_msg[0] && !strcmp(s_chk_tid, t->id))
        setNote(share, s_chk_msg, s_chk_ok == 1 ? SET_TONE_OK : SET_TONE_DANGER);
    }
  }
  setActionRow(share, UI_ICON_SHARE_2, "Export to the SD card", nullptr, [](lv_event_t*) { exportPadNow(); }, nullptr, SET_NORMAL, false);
  if (t->kind == otp::Kind::Dm) {
    char role[4] = {t->role, 0};
    setActionRow(share, UI_ICON_USERS, "Flip role", role, [](lv_event_t*) {
      if (otp::Target* x = cur()) { OtpMessenger::core().flipRole(*x); show(Screen::Pad); }
    }, nullptr, SET_NORMAL, false);
  }
  setNote(share, "Check code: the same on both devices means the same pad. Read it out, or play it by sound "
                 "(Sound link) and listen on the other one; it reveals nothing about the pad.");
  setNote(share, "Show as QR code: MeshCore Open TAK scans it from its OTP Pad screen (Scan QR), a few parts in turn. "
                 "Export writes this pad, unused, to the SD card as hex. The other device imports the same file; "
                 "roles A/B are worked out from your keys, so both sides import it unchanged.");

  lv_obj_t* danger = setCardIn(b, TR("Danger zone"));
  if (t->wq_n) {
    setActionRow(danger, UI_ICON_X, "Cancel waiting messages", nullptr, [](lv_event_t*) {
      if (otp::Target* x = cur()) { OtpMessenger::core().cancelWaiting(*x); show(Screen::Pad); }
    }, nullptr, SET_NORMAL, false);
  }
  setActionRow(danger, UI_ICON_TRASH_2, "Wipe pad", nullptr, [](lv_event_t*) {
    showConfirm(TR("Wipe this pad? Messages already received stay readable; nothing new can be sent or read until a new pad is added."),
                TR("Wipe"), wipeConfirmed);
  }, nullptr, SET_DANGER, false);
  setActionRow(danger, UI_ICON_TRASH_2, "Delete conversation", nullptr, [](lv_event_t*) {
    showConfirm(TR("Delete the pad and every message of this conversation?"), TR("Delete"), forgetConfirmed);
  }, nullptr, SET_DANGER, false);
}

// ---- Import from SD -------------------------------------------------------------------------
static auto& s_import_names = psramStatic<char[24][40]>();
static uint32_t s_import_sizes[24];
static int s_import_n = 0;
static void importFileCb(lv_event_t* e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  otp::Target* t = cur();
  fs::FS* sd = backupSdFs();
  if (!t || !sd || i < 0 || i >= s_import_n) return;
  File f = sd->open(s_import_names[i], "r");
  if (!f) { toast(TR("Could not open the file")); return; }
  const size_t n = f.size();
  if (n > 40000) { f.close(); toast(TR("File too large for a pad")); return; }
  char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { f.close(); toast(TR("Out of memory")); return; }
  const size_t rd = f.read((uint8_t*)buf, n);
  f.close();
  buf[rd] = 0;
  char err[160];
  bool ok;
  const char* nm = s_import_names[i];
  const size_t nl = strlen(nm);
  const bool is_txt = nl > 4 && !strcasecmp(nm + nl - 4, ".txt");
  if (is_txt) ok = OtpMessenger::core().addBytes(*t, (const uint8_t*)buf, rd, err, sizeof err);
  else ok = OtpMessenger::core().importHex(*t, buf, rd, err, sizeof err);
  memset(buf, 0, n + 1);
  heap_caps_free(buf);
  if (!ok) { toast(err, 2600); return; }
  if (!OtpMessenger::core().keep_files) sd->remove(nm);
  char msg[96];
  snprintf(msg, sizeof msg, "Imported %s. %s", nm, OtpMessenger::core().keep_files ? "Delete the file when both sides have it." : "File deleted.");
  toast(msg, 2600);
  show(Screen::Pad);
}
static void scanDir(fs::FS* sd, const char* dir) {
  File root = sd->open(dir);
  if (!root || !root.isDirectory()) return;
  for (File f = root.openNextFile(); f && s_import_n < 24; f = root.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }
    const char* nm = f.name();
    const char* base = strrchr(nm, '/');
    base = base ? base + 1 : nm;
    const size_t n = strlen(base);
    if (n > 4 && (!strcasecmp(base + n - 4, ".hex") || !strcasecmp(base + n - 4, ".txt")) && f.size() > 0) {
      s_import_sizes[s_import_n] = (uint32_t)f.size();
      snprintf(s_import_names[s_import_n++], sizeof s_import_names[0], "%s%s%s", dir, dir[1] ? "/" : "", base);
    }
    f.close();
  }
  root.close();
}
static void buildImport() {
  otp::Target* t = cur();
  if (!t) { show(Screen::List); return; }
  lv_obj_t* b = newBody();
  titleRow(b, TR("Import pad file"));
  fs::FS* sd = backupSdFs();
  s_import_n = 0;
  if (!sd) { emptyState(b, UI_ICON_CIRCLE_ALERT, TR("No SD card found"), TR("Insert the card with the pad file and try again.")); return; }
  scanDir(sd, "/");
  scanDir(sd, "/otp");
  if (!s_import_n) {
    emptyState(b, UI_ICON_FOLDER, TR("No pad files"), TR("No .hex or .txt files in the SD card's top folder or /otp."));
    return;
  }
  lv_obj_t* card = setCardIn(b, TR("SD card"));
  for (int i = 0; i < s_import_n; i++) {
    char sz[24];
    if (s_import_sizes[i] >= 1024) snprintf(sz, sizeof sz, "%.1f KB", s_import_sizes[i] / 1024.0f);
    else snprintf(sz, sizeof sz, "%u B", (unsigned)s_import_sizes[i]);
    setActionRowRaw(card, UI_ICON_FILE_TEXT, s_import_names[i], sz, importFileCb, (void*)(intptr_t)i, SET_NORMAL, false);
  }
  setNote(card, ".hex files are imported as a new pad, replacing this one. .txt files are added as raw bytes.");
}

// ---- moving over from the Lua app -----------------------------------------------------------------
#if CAP_LUA_APPS
// luaHostAppFs / luaHostAppPath: declared at file scope in UITask.cpp
static char s_lua_dir[96];     // <data>/apps/<id>.d
static char s_lua_sav[96];     // <data>/apps/<id>.sav

static bool luaReadCb(void*, const char* name, otp::Bytes& out) {
  fs::FS* fs = luaHostAppFs();
  if (!fs) return false;
  char path[160];
  snprintf(path, sizeof path, "%s/%s", s_lua_dir, name);
  File f = fs->open(path, "r");
  if (!f) return false;
  const size_t n = f.size();
  if (!out.reserve((uint32_t)n + 1)) { f.close(); return false; }
  const size_t rd = f.read(out.p, n);
  f.close();
  out.n = (uint32_t)rd;
  out.p[rd] = 0;
  return rd == n;
}
static bool luaContactCb(void*, const char* hex8, uint8_t pub[32], char* name, size_t cap) {
  const int n = OtpMessenger::contactCount();
  for (int i = 0; i < n; i++) {
    uint8_t p[32];
    char nm[33];
    if (!OtpMessenger::contactAt(i, p, nm, sizeof nm)) continue;
    char h[9];
    otp::toHex(p, 4, h);
    if (strcasecmp(h, hex8) != 0) continue;
    memcpy(pub, p, 32);
    snprintf(name, cap, "%s", nm);
    return true;
  }
  return false;
}
static bool luaChannelCb(void*, const char* san, char* name, size_t cap) {
  for (int i = 0; i < OtpMessenger::channelCount(); i++) {
    char nm[33], s2[40];
    if (!OtpMessenger::channelAt(i, nm, sizeof nm)) continue;
    size_t j = 0;
    for (const char* c = nm; *c && j + 1 < sizeof s2; ++c)
      s2[j++] = ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')) ? *c : '_';
    s2[j] = 0;
    if (strcmp(s2, san) == 0) { snprintf(name, cap, "%s", nm); return true; }
  }
  return false;
}
// The installed Lua app that holds OTP pads (any id: it was sideloaded).
static bool findLuaApp(otp::Bytes& sav) {
  fs::FS* fs = luaHostAppFs();
  if (!fs) return false;
  char apps[80];
  ::luaHostAppPath(apps, sizeof apps, "/apps");
  File root = fs->open(apps);
  if (!root || !root.isDirectory()) return false;
  bool found = false;
  for (File f = root.openNextFile(); f && !found; f = root.openNextFile()) {
    const char* nm = f.name();
    const char* base = strrchr(nm, '/');
    base = base ? base + 1 : nm;
    const size_t n = strlen(base);
    if (!f.isDirectory() && n > 4 && !strcmp(base + n - 4, ".sav") && f.size() < 64000) {
      sav.clear();
      if (sav.reserve((uint32_t)f.size() + 1)) {
        sav.n = (uint32_t)f.read(sav.p, f.size());
        sav.p[sav.n] = 0;
        if (strstr((const char*)sav.p, "padmeta_")) {
          snprintf(s_lua_sav, sizeof s_lua_sav, "%s/%s", apps, base);
          snprintf(s_lua_dir, sizeof s_lua_dir, "%s/%.*s.d", apps, (int)(n - 4), base);
          found = true;
        }
      }
    }
    f.close();
  }
  root.close();
  return found;
}
static void luaImportWith(const char* pass) {
  if (!pass) return;
  otp::Bytes sav;
  if (!findLuaApp(sav)) { toast(TR("No OTP pads from the Lua app found"), 2200); return; }
  otp::Core::LuaSource src{luaReadCb, luaContactCb, luaChannelCb, nullptr};
  char err[120];
  const int n = OtpMessenger::core().importLua((const char*)sav.p, sav.n, src, pass, err, sizeof err);
  if (n < 0) { toast(err, 2400); return; }
  char msg[120];
  if (n) snprintf(msg, sizeof msg, "Moved %d pad(s), counters included. Remove the Lua app so only one copy is used.", n);
  else snprintf(msg, sizeof msg, "%s", TR("Nothing to move: those pads are already here, or their contacts are unknown"));
  toast(msg, 3500);
  show(Screen::List);
}
static void luaImport() {
  otp::Bytes sav;
  if (!findLuaApp(sav)) { toast(TR("No OTP pads from the Lua app found"), 2200); return; }
  if (otp::Core::luaNeedsPassphrase((const char*)sav.p, sav.n))
    prompt(TR("Lua app passphrase"), TR("The Lua app's pads are protected. Its passphrase unlocks them once, to move them."),
           true, 64, luaImportWith);
  else luaImportWith("");
}
#endif

// ---- Settings -----------------------------------------------------------------------------------
static char s_pass1[65];
static void passSecond(const char* p) {
  if (!p) { memset(s_pass1, 0, sizeof s_pass1); return; }
  if (strcmp(p, s_pass1) != 0) { memset(s_pass1, 0, sizeof s_pass1); toast(TR("Passphrases did not match"), 1800); return; }
  const bool ok = OtpMessenger::core().enableProtection(p);
  memset(s_pass1, 0, sizeof s_pass1);
  toast(ok ? TR("Protection on. Pads and messages are now encrypted.") : TR("Could not enable protection"), 2400);
  show(Screen::Settings);
}
static void passFirst(const char* p) {
  if (!p) return;
  if (!*p) { toast(TR("The passphrase cannot be empty")); return; }
  snprintf(s_pass1, sizeof s_pass1, "%s", p);
  prompt(TR("Confirm passphrase"), nullptr, true, 64, passSecond);
}
static void buildSettings() {
  otp::Core& c = OtpMessenger::core();
  lv_obj_t* b = newBody();
  titleRow(b, TR("Settings"));

  // Theme: the six OTP_3_RC1 themes as tiles, with the chosen one's colours under them.
  lv_obj_t* look = setCardIn(b, TR("Appearance"));
  const char* names[6];
  for (int i = 0; i < 6; i++) names[i] = kThemes[i].name;
  setChoiceRow(look, "Theme", "The accent for the send button, the pad gauge and the status words",
               names, 6, c.theme % 6, [](lv_event_t* e) {
                 otp::Core& cc = OtpMessenger::core();
                 cc.theme = (uint8_t)(setChoiceIndex(e) % 6);
                 cc.saveSettings();
                 show(Screen::Settings);
               });
  lv_obj_t* sw = box(look);
  lv_obj_set_size(sw, s_settings_content_w, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(sw, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  const Theme& t = th();
  const uint32_t cols[] = {t.channel, t.ok, t.decrypt, t.newpad, t.keyphrase, t.padinfo, t.encrypt, t.ciphertext, t.plaintext, t.ready};
  const lv_coord_t dot = LV_MIN(SC(18), (lv_coord_t)(s_settings_content_w / 10 - 4));
  for (uint32_t col : cols) {
    lv_obj_t* d = box(sw);
    lv_obj_set_size(d, dot, dot);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d, lv_color_hex(col), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(d, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(d, 1, LV_PART_MAIN);
  }

  lv_obj_t* beh = setCardIn(b, TR("Behaviour"));
  setSwitchRow(beh, "Sound", "Play the message sound when an OTP message arrives", c.sound, [](lv_event_t* e) {
    otp::Core& cc = OtpMessenger::core();
    cc.sound = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    cc.saveSettings();
  });
  setSwitchRow(beh, "Keep imported files", "Leave pad files on the SD card after importing them", c.keep_files, [](lv_event_t* e) {
    otp::Core& cc = OtpMessenger::core();
    cc.keep_files = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    cc.saveSettings();
  });

  lv_obj_t* lock = setCardIn(b, TR("Passphrase lock"));
  if (c.protectionEnabled()) {
    setNote(lock, "On: pads, messages and the conversation list are encrypted on this device.", SET_TONE_OK);
    setButton(lock, UI_ICON_LOCK, "Lock now", [](lv_event_t*) {
      OtpMessenger::core().lockNow();
      s_tid[0] = 0;
      show(Screen::Lock);
    }, nullptr, SET_PRIMARY);
  } else {
    setNote(lock, "Off. With a passphrase, everything is encrypted on this device and the app asks for it "
                  "after every restart. Incoming messages wait until you unlock.");
    setButton(lock, UI_ICON_LOCK, "Set passphrase", [](lv_event_t*) {
      prompt(TR("New passphrase"), nullptr, true, 64, passFirst);
    }, nullptr, SET_PRIMARY);
  }
  setButton(lock, UI_ICON_TRASH_2, "Panic wipe", panicAsk, nullptr, SET_DANGER);

#if CAP_LUA_APPS
  lv_obj_t* lua = setCardIn(b, TR("Coming from the Lua app"));
  setNote(lua, "Moves its pads here with their counters, so you carry on in step with the other side. "
               "Afterwards remove the Lua app: two apps using one pad would burn the same bytes twice.");
  setActionRow(lua, UI_ICON_HISTORY, "Move pads from the Lua app", nullptr, [](lv_event_t*) { luaImport(); },
               nullptr, SET_NORMAL, false);
#endif
  char line[120];
  snprintf(line, sizeof line, "OTP Messenger %s · wire-compatible with OTP_3_RC1 and MeshCore Open TAK", otp::Core::kBuildId);
  lv_obj_t* f = para(b, line, COLOR_TERTIARY);
  lv_obj_set_width(f, rowW());
  lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}


// ---- navigation -----------------------------------------------------------------------------------
static void show(Screen s) {
  if (!s_root) return;
  qrClose();
  promptClose();
  otp::Core& c = OtpMessenger::core();
  if (c.locked()) s = Screen::Lock;
  if (s != Screen::Chat && s != Screen::Pad) c.setOpenTarget(nullptr);
  if (s != Screen::Chat) hideKb();
  s_screen = s;
  switch (s) {
    case Screen::Lock: buildLock(); break;
    case Screen::List: buildList(); break;
    case Screen::Pick: buildPick(); break;
    case Screen::Chat: buildChat(); break;
    case Screen::Pad: buildPad(); break;
    case Screen::Import: buildImport(); break;
    case Screen::Settings: buildSettings(); break;
  }
  themeTree(s_body);
  s_seen = OtpMessenger::version();
}

static void back() {
  if (s_qr) { qrClose(); return; }
  if (s_prompt) { promptDone(false); return; }
  switch (s_screen) {
    case Screen::Chat: show(Screen::List); break;
    case Screen::Pad: show(cur() && OtpMessenger::core().hasPad(*cur()) ? Screen::Chat : Screen::List); break;
    case Screen::Import: show(Screen::Pad); break;
    case Screen::Pick: case Screen::Settings: show(Screen::List); break;
    default: close(); break;
  }
}

static void timerCb(lv_timer_t*) {
  if (!s_root) return;
  const uint32_t v = OtpMessenger::version();
  if (v != s_seen) {
    s_seen = v;
    if (s_screen == Screen::Chat) { refreshInfo(); refreshCounter(); buildBubbles(true); }
    else if (s_screen == Screen::List) { buildList(); themeTree(s_body); }
  } else if (s_screen == Screen::Chat) {
    refreshInfo();   // signal grade, badge expiry
  }
}

static void close() {
  qrClose();
  promptClose();
  if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
  if (OtpMessenger::ready()) OtpMessenger::core().setOpenTarget(nullptr);
  hideKb();
  s_body = s_list = s_ta = s_counter = s_info = s_parts = s_bar = s_send_btn = s_parts_btn = nullptr;
  if (s_root) { lv_obj_t* r = s_root; s_root = nullptr; appPageDeleteRootAsync(r); }
  appPageEnd(back);
}

static void open() {
  if (!OtpMessenger::ready()) { toast(TR("Storage not ready yet")); return; }
  if (s_root) return;
  s_root = appPageCreateRoot(COLOR_BG);
  appPageBeginSlim("OTP Messenger", back);
  s_timer = lv_timer_create(timerCb, 500, nullptr);
  otp::Target* t = cur();
  show(t ? Screen::Chat : Screen::List);
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

static bool isOpen() { return s_root != nullptr; }

// ---- background callbacks -------------------------------------------------------------------------
static void toastCb(const char* m) { toast(m, 2200); }
static void incomingCb(otp::Target& t, const otp::Entry& e) {
  const bool showing = s_root && s_screen == Screen::Chat && !strcmp(s_tid, t.id);
  if (showing) return;
  // Name only: the text stays inside the app.
  char msg[64];
  snprintf(msg, sizeof msg, "OTP: %s%s", t.kind == otp::Kind::Channel ? "#" : "", e.who[0] ? e.who : t.name);
  if (t.kind == otp::Kind::Channel) snprintf(msg, sizeof msg, "OTP #%s: %s", t.name, e.who);
  toast(msg, 2500);
  if (OtpMessenger::core().sound && g_lv.task && !g_lv.task->isBuzzerQuiet()) uiPlaySlot(TOUCH_SND_MSG);
}

}  // namespace otpui
