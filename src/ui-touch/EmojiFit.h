// SPDX-License-Identifier: GPL-3.0-or-later
// Colour emoji that sit in a line of text the way a phone draws them.
//
// The baked emoji (emoji_data.c) are 16x16 images served through an LVGL
// imgfont at the end of each text size's fallback chain. LVGL 8.3's imgfont
// places every image with no offsets: its bottom on the text baseline, its
// width as its advance. In the 12 px chat text (a 15 px line, 3 px below the
// baseline) that put each emoji a third taller than the letters, rising 4 px
// above its own line into the one before, and touching the next character.
//
// This fits them to the text they sit in:
//   - size: one px under the line height (at most the baked 16), downscaled
//     once per codepoint and size and kept, so the 12 px chain gets 14 px emoji;
//   - place: centred in the text's line box instead of standing on the baseline;
//   - space: a pixel of side bearing each side, as a font glyph has.
// Measuring (lv_txt_get_size) only reads the text font's line height and the
// advance, so wrapped heights stay what the chat's virtual list expects.
//
// Header-only so the host render test (test/emoji_fit_render) draws the same.
#pragma once

#include "lvgl.h"
#include "emoji_data.h"
#include "FlagText.h"

#include <stdlib.h>
#include <string.h>

namespace emojifit {

struct Slot {
  lv_font_t* imgfont;        // the lv_imgfont in a text size's chain
  const lv_font_t* text;     // the text font it falls back from
};
static Slot s_slots[4];
static int s_slot_n = 0;
static bool (*s_imgfont_dsc)(const lv_font_t*, lv_font_glyph_dsc_t*, uint32_t, uint32_t) = nullptr;

static const Slot* slotFor(const lv_font_t* f) {
  for (int i = 0; i < s_slot_n; ++i) if (s_slots[i].imgfont == f) return &s_slots[i];
  return nullptr;
}

// The emoji size for a text font: one px under its line height, 10..16.
static inline int pxFor(const lv_font_t* text) {
  if (!text) return 16;
  const int px = (int)text->line_height - 1;
  return px > 16 ? 16 : px < 10 ? 10 : px;
}

// ---- downscaled copies ---------------------------------------------------------------
// RGB565+alpha (LV_IMG_CF_TRUE_COLOR_ALPHA, no byte swap): 3 bytes a pixel, colour
// little-endian then alpha. Area-averaged with alpha weighting, so edges stay clean.
struct Scaled { uint32_t cp; uint8_t px; lv_img_dsc_t dsc; };
static constexpr int kMaxScaled = 512;
static Scaled* s_scaled = nullptr;
static int s_scaled_n = 0;

static bool downscale(const lv_img_dsc_t* src, int px, uint8_t* out) {
  const int sw = (int)src->header.w, sh = (int)src->header.h;
  if (sw <= 0 || sh <= 0 || !src->data) return false;
  for (int dy = 0; dy < px; ++dy) {
    for (int dx = 0; dx < px; ++dx) {
      // The source rectangle this pixel covers, in 1/px source pixels.
      const int x0 = dx * sw, x1 = (dx + 1) * sw, y0 = dy * sh, y1 = (dy + 1) * sh;
      uint32_t r = 0, g = 0, b = 0, a = 0, wsum = 0;
      for (int sy = y0 / px; sy * px < y1 && sy < sh; ++sy) {
        const int wy = (sy + 1) * px < y1 ? (sy + 1) * px - (sy * px > y0 ? sy * px : y0)
                                          : y1 - (sy * px > y0 ? sy * px : y0);
        for (int sx = x0 / px; sx * px < x1 && sx < sw; ++sx) {
          const int wx = (sx + 1) * px < x1 ? (sx + 1) * px - (sx * px > x0 ? sx * px : x0)
                                            : x1 - (sx * px > x0 ? sx * px : x0);
          const uint32_t w = (uint32_t)(wx * wy);
          const uint8_t* p = src->data + 3 * (sy * sw + sx);
          const uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
          const uint32_t pa = p[2];
          const uint32_t pr = ((v >> 11) & 0x1F) * 255 / 31, pg = ((v >> 5) & 0x3F) * 255 / 63, pb = (v & 0x1F) * 255 / 31;
          r += pr * pa * w; g += pg * pa * w; b += pb * pa * w; a += pa * w; wsum += w;
        }
      }
      uint8_t* o = out + 3 * (dy * px + dx);
      if (!a || !wsum) { o[0] = o[1] = o[2] = 0; continue; }
      const uint32_t cr = r / a, cg = g / a, cb = b / a;
      const uint32_t v = ((cr >> 3) << 11) | ((cg >> 2) << 5) | (cb >> 3);
      o[0] = (uint8_t)(v & 0xFF);
      o[1] = (uint8_t)(v >> 8);
      o[2] = (uint8_t)((a + wsum / 2) / wsum);
    }
  }
  return true;
}

// The emoji at px, made once and kept; the baked 16 px one when px is not smaller
// or memory is short.
static const lv_img_dsc_t* sized(uint32_t cp, const lv_img_dsc_t* base, int px) {
  if (!base || px >= (int)base->header.w || base->header.cf != LV_IMG_CF_TRUE_COLOR_ALPHA) return base;
  for (int i = 0; i < s_scaled_n; ++i)
    if (s_scaled[i].cp == cp && s_scaled[i].px == px) return &s_scaled[i].dsc;
  if (!s_scaled) s_scaled = (Scaled*)calloc(kMaxScaled, sizeof(Scaled));
  if (!s_scaled || s_scaled_n >= kMaxScaled) return base;
  uint8_t* data = (uint8_t*)malloc((size_t)px * px * 3);
  if (!data) return base;
  if (!downscale(base, px, data)) { free(data); return base; }
  Scaled& s = s_scaled[s_scaled_n++];
  s.cp = cp;
  s.px = (uint8_t)px;
  s.dsc = *base;
  s.dsc.header.w = (uint32_t)px;
  s.dsc.header.h = (uint32_t)px;
  s.dsc.data_size = (uint32_t)px * px * 3;
  s.dsc.data = data;
  return &s.dsc;
}

// ---- the imgfont hooks ---------------------------------------------------------------
// Path callback: the image for an emoji codepoint, at the size its text calls for,
// copied into the imgfont's scratch dsc; false for everything else, so LVGL keeps
// walking the chain.
// The emoji pack's own image for a codepoint, past the baked set (EmojiPackUI.inc
// sets it once the pack is in use): a lone regional-indicator letter draws as its
// letter, not as the baked Lesotho flag keyed on L.
static const lv_img_dsc_t* (*s_pack_only)(uint32_t) = nullptr;

// The image for a letter LVGL hands the font: a codepoint, or a flag (FlagText.h).
static const lv_img_dsc_t* imageFor(uint32_t unicode) {
  if (flagtext::isKey(unicode)) {
    const lv_img_dsc_t* d = emojiGlyphLookup(unicode);   // the pack's flags
    // Lesotho is baked (on its L, with S drawn as nothing), so it shows without the pack.
    if (!d && unicode == flagtext::key(0x1F1F1, 0x1F1F8)) d = emojiGlyphLookup(0x1F1F1);
    return d;
  }
  if (flagtext::isRi(unicode)) {   // a letter on its own, not in a pair
    if (s_pack_only) {
      if (const lv_img_dsc_t* d = s_pack_only(unicode)) return d;
    }
    if (unicode == 0x1F1F1) return nullptr;   // not the baked Lesotho flag: that is LS only
  }
  return emojiGlyphLookup(unicode);
}

static bool pathCb(const lv_font_t* font, void* img_src, uint16_t /*len*/, uint32_t unicode, uint32_t /*next*/) {
  const lv_img_dsc_t* d = imageFor(unicode);
  // Skin-tone modifiers (U+1F3FB-1F3FF) have no baked image: draw them as the
  // invisible zero glyph (the same one U+FE0F maps to), so a toned thumbs-up
  // shows as the plain one (upstream #673).
  if (!d && unicode >= 0x1F3FB && unicode <= 0x1F3FF) d = emojiGlyphLookup(0xFE0F);
  if (!d) return false;
  const Slot* s = slotFor(font);
  d = sized(unicode, d, s ? pxFor(s->text) : 16);
  memcpy(img_src, d, sizeof(lv_img_dsc_t));
  // The imgfont reuses ONE scratch buffer as the image source for every glyph, so
  // LVGL's image cache (keyed on the src pointer) hands back the first-decoded
  // emoji for all of them. Drop the stale entry so this one is decoded fresh.
  lv_img_cache_invalidate_src(img_src);
  return true;
}

// Glyph metrics: the imgfont's, then centred in the text's line box with a pixel
// of space either side.
static bool glyphDsc(const lv_font_t* font, lv_font_glyph_dsc_t* dsc, uint32_t unicode, uint32_t next) {
  if (!s_imgfont_dsc || !s_imgfont_dsc(font, dsc, unicode, next)) return false;
  const Slot* s = slotFor(font);
  if (!s || !s->text) return true;
  const int lh = s->text->line_height, bl = s->text->base_line, h = dsc->box_h;
  // The letter is drawn at top = (lh - bl) - box_h - ofs_y; this makes top = (lh - h) / 2.
  dsc->ofs_y = (int16_t)((lh - bl) - h - (lh - h) / 2);
  dsc->ofs_x = 1;
  dsc->adv_w = (uint16_t)(dsc->box_w + 2);
  return true;
}

// Fits `imgfont` (made by lv_imgfont_create with pathCb) to `text`, the font whose
// chain it ends. Call once per text size after creating it.
static void attach(lv_font_t* imgfont, const lv_font_t* text) {
  if (!imgfont || s_slot_n >= (int)(sizeof s_slots / sizeof s_slots[0])) return;
  if (!s_imgfont_dsc) s_imgfont_dsc = imgfont->get_glyph_dsc;
  s_slots[s_slot_n++] = Slot{imgfont, text};
  imgfont->get_glyph_dsc = glyphDsc;
}

}  // namespace emojifit
