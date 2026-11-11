// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A contact's photo, shown in place of its glyph or chat icon: a circle cut from
// one of the photos on this radio (Photos), kept as a small RGB565 + alpha image so
// the circle's edge is in the picture itself and draws at no extra cost. Pure, so
// the host tests can run it (test/test_contact_photo.cpp); ContactPhotoUI.inc keeps
// the files and draws it.
//
// The photo is keyed on the name the contact's chat icon is keyed on (its
// advertised name), so it follows the same chat everywhere the icon does.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace ContactPhoto {

// The stored circle: kSide x kSide pixels, each [RGB565 low byte][high byte][alpha],
// the layout LVGL's TRUE_COLOR_ALPHA wants at 16-bit colour.
constexpr int kSide = 64;
constexpr size_t kPixels = (size_t)kSide * kSide * 3u;
// The file: "CP1" and the side, then the pixels.
constexpr size_t kHeader = 4;
constexpr size_t kFileBytes = kHeader + kPixels;

// The zoom steps the crop screen offers; 1 fits the circle to the photo's short side.
constexpr float kZooms[] = {1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f};
constexpr int kZoomCount = (int)(sizeof kZooms / sizeof kZooms[0]);

// FNV-1a of the name: the file is /cph_<8 hex>.cp1.
inline uint32_t keyOf(const char* name) {
  uint32_t h = 2166136261u;
  for (const unsigned char* p = (const unsigned char*)(name ? name : ""); *p; ++p) { h ^= *p; h *= 16777619u; }
  return h ? h : 1u;   // 0 means "none" to the caller
}
inline void fileName(uint32_t key, char* out, size_t cap) { snprintf(out, cap, "cph_%08lx.cp1", (unsigned long)key); }
// The key in a file name ("cph_0123abcd.cp1", with or without a folder); 0 if it isn't one.
inline uint32_t keyOfFile(const char* path) {
  if (!path) return 0;
  const char* b = strrchr(path, '/');
  b = b ? b + 1 : path;
  if (strlen(b) != 16 || strncmp(b, "cph_", 4) || strcmp(b + 12, ".cp1")) return 0;
  uint32_t k = 0;
  for (int i = 4; i < 12; ++i) {
    const char c = b[i];
    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (v < 0) return 0;
    k = (k << 4) | (uint32_t)v;
  }
  return k;
}

inline void header(uint8_t out[kHeader]) { out[0] = 'C'; out[1] = 'P'; out[2] = '1'; out[3] = (uint8_t)kSide; }
inline bool headerOk(const uint8_t* h, size_t file_len) {
  return h && file_len == kFileBytes && h[0] == 'C' && h[1] == 'P' && h[2] == '1' && h[3] == kSide;
}

// The circle's radius in source pixels at a zoom step: half the short side, over the zoom.
inline float radiusAt(int sw, int sh, int zoom_idx) {
  if (zoom_idx < 0) zoom_idx = 0;
  if (zoom_idx >= kZoomCount) zoom_idx = kZoomCount - 1;
  const int s = sw < sh ? sw : sh;
  return (float)s * 0.5f / kZooms[zoom_idx];
}

// Keeps the circle (centre cx, cy; radius r, source pixels) inside the photo.
inline void clampCentre(int sw, int sh, float r, float& cx, float& cy) {
  const float lox = r, hix = (float)sw - r, loy = r, hiy = (float)sh - r;
  cx = lox > hix ? (float)sw * 0.5f : cx < lox ? lox : cx > hix ? hix : cx;
  cy = loy > hiy ? (float)sh * 0.5f : cy < loy ? loy : cy > hiy ? hiy : cy;
}

namespace detail {
inline void rgb(uint16_t p, float& r, float& g, float& b) {
  r = (float)(p >> 11); g = (float)((p >> 5) & 0x3F); b = (float)(p & 0x1F);
}
// Bilinear at (x, y), pixel centres at +0.5, edges held.
inline void sample(const uint16_t* src, int sw, int sh, float x, float y, float& r, float& g, float& b) {
  x -= 0.5f; y -= 0.5f;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x > (float)(sw - 1)) x = (float)(sw - 1);
  if (y > (float)(sh - 1)) y = (float)(sh - 1);
  const int x0 = (int)x, y0 = (int)y;
  const int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
  const float fx = x - (float)x0, fy = y - (float)y0;
  float r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
  rgb(src[(size_t)y0 * sw + x0], r00, g00, b00);
  rgb(src[(size_t)y0 * sw + x1], r10, g10, b10);
  rgb(src[(size_t)y1 * sw + x0], r01, g01, b01);
  rgb(src[(size_t)y1 * sw + x1], r11, g11, b11);
  const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
  r = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11;
  g = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11;
  b = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;
}
}  // namespace detail

// Cuts the circle (centre cx, cy; radius r, in source pixels) out of `src` (RGB565,
// sw x sh) into `out` (kPixels): each pixel the mean of 2 x 2 bilinear samples, and
// outside the circle transparent, with a one-pixel soft edge.
inline bool crop(const uint16_t* src, int sw, int sh, float cx, float cy, float r, uint8_t* out) {
  if (!src || !out || sw <= 0 || sh <= 0 || !(r > 0)) return false;
  const float half = kSide * 0.5f;
  const float step = 2.0f * r / (float)kSide;   // source pixels per output pixel
  for (int y = 0; y < kSide; ++y) {
    for (int x = 0; x < kSide; ++x) {
      uint8_t* o = out + ((size_t)y * kSide + x) * 3u;
      const float dx = (float)x + 0.5f - half, dy = (float)y + 0.5f - half;
      float a = half - sqrtf(dx * dx + dy * dy) + 0.5f;   // 1 inside, 0 outside, a pixel's ramp
      a = a < 0 ? 0 : a > 1 ? 1 : a;
      if (a <= 0) { o[0] = o[1] = o[2] = 0; continue; }
      float rs = 0, gs = 0, bs = 0;
      for (int sy = 0; sy < 2; ++sy) {
        for (int sx = 0; sx < 2; ++sx) {
          const float ox = ((float)x + 0.25f + 0.5f * (float)sx - half) * step;
          const float oy = ((float)y + 0.25f + 0.5f * (float)sy - half) * step;
          float r1, g1, b1;
          detail::sample(src, sw, sh, cx + ox, cy + oy, r1, g1, b1);
          rs += r1; gs += g1; bs += b1;
        }
      }
      const uint16_t p = (uint16_t)(((uint16_t)(rs * 0.25f + 0.5f) << 11) | ((uint16_t)(gs * 0.25f + 0.5f) << 5) |
                                    (uint16_t)(bs * 0.25f + 0.5f));
      o[0] = (uint8_t)(p & 0xFF);
      o[1] = (uint8_t)(p >> 8);
      o[2] = (uint8_t)(a * 255.0f + 0.5f);
    }
  }
  return true;
}

}  // namespace ContactPhoto
