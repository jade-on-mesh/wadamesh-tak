// SPDX-License-Identifier: GPL-3.0-or-later
//
// ContactPhoto.h: the key and file name, the crop's clamping, and the circle it cuts.
// g++ -std=c++17 -I src test/test_contact_photo.cpp -o /tmp/t && /tmp/t

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui-touch/ContactPhoto.h"

using namespace ContactPhoto;

static uint16_t px(int r, int g, int b) { return (uint16_t)((r << 11) | (g << 5) | b); }
static uint16_t at(const uint8_t* out, int x, int y) {
  const uint8_t* o = out + ((size_t)y * kSide + x) * 3u;
  return (uint16_t)(o[0] | (o[1] << 8));
}
static uint8_t alpha(const uint8_t* out, int x, int y) { return out[((size_t)y * kSide + x) * 3u + 2]; }

static void keys() {
  assert(keyOf("Bob") == keyOf("Bob"));
  assert(keyOf("Bob") != keyOf("bob"));
  assert(keyOf("") != 0 && keyOf(nullptr) == keyOf(""));
  char n[24];
  fileName(0x0123abcdu, n, sizeof n);
  assert(!strcmp(n, "cph_0123abcd.cp1"));
  assert(keyOfFile(n) == 0x0123abcdu);
  assert(keyOfFile("/meshcomod/cph_0123abcd.cp1") == 0x0123abcdu);
  assert(keyOfFile("vox_0123abcd.vi1") == 0);
  assert(keyOfFile("cph_0123ABCD.cp1") == 0);   // ours are lower case
  assert(keyOfFile("cph_0123abcd.cp2") == 0);
  uint8_t h[kHeader];
  header(h);
  assert(headerOk(h, kFileBytes));
  assert(!headerOk(h, kFileBytes - 1));
  h[3] = 48;
  assert(!headerOk(h, kFileBytes));
}

static void clamping() {
  // A 96 x 64 photo: zoom 1 is a 32 px radius, the circle fills the height.
  assert(radiusAt(96, 64, 0) == 32.0f);
  assert(radiusAt(96, 64, 3) == 16.0f);     // 2x
  assert(radiusAt(96, 64, 99) == 8.0f);     // held at the last step, 4x
  float cx = 0, cy = 0;
  clampCentre(96, 64, 32, cx, cy);
  assert(cx == 32 && cy == 32);
  cx = 200; cy = -5;
  clampCentre(96, 64, 16, cx, cy);
  assert(cx == 80 && cy == 16);
  cx = 50; cy = 30;
  clampCentre(96, 64, 16, cx, cy);
  assert(cx == 50 && cy == 30);             // inside: kept
}

static void cutting() {
  // Left half red, right half blue.
  const int sw = 80, sh = 60;
  uint16_t* src = (uint16_t*)malloc(sizeof(uint16_t) * sw * sh);
  for (int y = 0; y < sh; ++y)
    for (int x = 0; x < sw; ++x) src[y * sw + x] = x < sw / 2 ? px(31, 0, 0) : px(0, 0, 31);
  uint8_t* out = (uint8_t*)malloc(kPixels);
  assert(crop(src, sw, sh, 40, 30, 30, out));
  // The corners are outside the circle, the middle inside, the edge in between.
  assert(alpha(out, 0, 0) == 0 && alpha(out, kSide - 1, kSide - 1) == 0);
  assert(alpha(out, kSide / 2, kSide / 2) == 255);
  assert(alpha(out, kSide / 2, 0) > 0 && alpha(out, kSide / 2, 0) < 255);
  int soft = 0;
  for (int y = 0; y < kSide; ++y)
    for (int x = 0; x < kSide; ++x) soft += alpha(out, x, y) > 0 && alpha(out, x, y) < 255;
  assert(soft > 100 && soft < 400);         // about the circumference
  // Red on the left, blue on the right, a blend only at the seam.
  assert(at(out, 8, kSide / 2) == px(31, 0, 0));
  assert(at(out, kSide - 9, kSide / 2) == px(0, 0, 31));
  // Moved right, the circle sees only blue.
  assert(crop(src, sw, sh, 65, 30, 12, out));
  assert(at(out, 4, kSide / 2) == px(0, 0, 31) && at(out, kSide / 2, kSide / 2) == px(0, 0, 31));
  // Bad input is refused.
  assert(!crop(nullptr, sw, sh, 40, 30, 30, out));
  assert(!crop(src, sw, sh, 40, 30, 0, out));
  free(out);
  free(src);
}

int main() {
  keys();
  clamping();
  cutting();
  printf("test_contact_photo: ok\n");
  return 0;
}
