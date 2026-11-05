// SPDX-License-Identifier: GPL-3.0-or-later
// Flags in text. A flag emoji is two regional-indicator letters (🇺🇸 = U+1F1FA
// U+1F1F8), but LVGL draws text one codepoint at a time, so a flag came out as two
// separate glyphs: the first letter's and the second's (the baked Lesotho flag was
// keyed on its L, with S drawn as nothing, so every L flag showed Lesotho's and
// every S vanished).
//
// These replace LVGL's UTF-8 "next / previous letter / letter index to byte" helpers
// (the _lv_txt_encoded_* pointers, installed by install() in UITask) with ones that
// read a PAIR of regional indicators as one letter: key(a, b), a value above the
// last Unicode codepoint, so no font has it and the emoji image font draws the pair's
// flag (EmojiFit.h; the images are in the emoji pack, Lesotho's also baked). Every
// width, wrap, cursor step and delete then treats a flag as one letter, as a phone
// does. Pairs are made from the start of a run of indicators, two at a time; an odd
// one left over is a letter on its own.
//
// Only next, prev and get_byte_id change; LVGL's get_char_id and get_length go
// through next, so they follow. The byte-size helper stays plain UTF-8 (a flag's
// first letter is 4 bytes): LVGL copies one letter into an 8-byte buffer for the
// textarea cursor, which a flag's 8 bytes would leave unterminated.
//
// The UTF-8 decoding is LVGL 8.3's lv_txt_utf8_next/prev (MIT), unchanged.
// Header-only and free of LVGL: test/test_flag_text.cpp.
#pragma once

#include <stdint.h>

namespace flagtext {

static constexpr uint32_t kRiFirst = 0x1F1E6;   // 🇦
static constexpr uint32_t kRiLast  = 0x1F1FF;   // 🇿
static constexpr uint32_t kKeyBase = 0x110000;  // past Unicode: never a real letter

inline bool isRi(uint32_t c) { return c >= kRiFirst && c <= kRiLast; }
inline uint32_t key(uint32_t a, uint32_t b) { return kKeyBase + (a - kRiFirst) * 26u + (b - kRiFirst); }
inline bool isKey(uint32_t c) { return c >= kKeyBase && c < kKeyBase + 26u * 26u; }
// The ISO letters of a key ("US"), for logs and tests.
inline void keyLetters(uint32_t k, char out[3]) {
  out[0] = (char)('A' + (k - kKeyBase) / 26u);
  out[1] = (char)('A' + (k - kKeyBase) % 26u);
  out[2] = '\0';
}

namespace detail {
inline bool isAscii(uint8_t v) { return (v & 0x80u) == 0x00u; }
inline bool is2(uint8_t v) { return (v & 0xE0u) == 0xC0u; }
inline bool is3(uint8_t v) { return (v & 0xF0u) == 0xE0u; }
inline bool is4(uint8_t v) { return (v & 0xF8u) == 0xF0u; }
inline bool bad(uint8_t v) { return (v & 0xC0u) != 0x80u; }

// lv_txt_utf8_size
inline uint8_t size(const char* s) {
  const uint8_t c = (uint8_t)s[0];
  if (isAscii(c)) return 1;
  if (is2(c)) return 2;
  if (is3(c)) return 3;
  if (is4(c)) return 4;
  return 0;
}

// lv_txt_utf8_next
inline uint32_t next(const char* txt, uint32_t* i) {
  uint32_t r = 0;
  const uint8_t* t = (const uint8_t*)txt;
  if (isAscii(t[*i])) {
    r = t[*i];
    (*i)++;
  } else if (is2(t[*i])) {
    r = (uint32_t)(t[*i] & 0x1F) << 6; (*i)++;
    if (bad(t[*i])) return 0;
    r += (t[*i] & 0x3F); (*i)++;
  } else if (is3(t[*i])) {
    r = (uint32_t)(t[*i] & 0x0F) << 12; (*i)++;
    if (bad(t[*i])) return 0;
    r += (uint32_t)(t[*i] & 0x3F) << 6; (*i)++;
    if (bad(t[*i])) return 0;
    r += (t[*i] & 0x3F); (*i)++;
  } else if (is4(t[*i])) {
    r = (uint32_t)(t[*i] & 0x07) << 18; (*i)++;
    if (bad(t[*i])) return 0;
    r += (uint32_t)(t[*i] & 0x3F) << 12; (*i)++;
    if (bad(t[*i])) return 0;
    r += (uint32_t)(t[*i] & 0x3F) << 6; (*i)++;
    if (bad(t[*i])) return 0;
    r += t[*i] & 0x3F; (*i)++;
  } else {
    (*i)++;   // not UTF-8: go to the next byte
  }
  return r;
}

// lv_txt_utf8_prev: *i moves to the start of the codepoint before it.
inline uint32_t prev(const char* txt, uint32_t* i) {
  uint8_t c_size;
  uint8_t cnt = 0;
  (*i)--;
  do {
    if (cnt >= 4) return 0;
    c_size = size(&txt[*i]);
    if (c_size == 0) {
      if (*i != 0) (*i)--;
      else return 0;
    }
    cnt++;
  } while (c_size == 0);
  uint32_t tmp = *i;
  return next(txt, &tmp);
}

// The 4 bytes at p are a regional indicator (F0 9F 87 A6..BF).
inline bool riAt(const char* txt, uint32_t p) {
  const uint8_t* t = (const uint8_t*)txt + p;
  return t[0] == 0xF0 && t[1] == 0x9F && t[2] == 0x87 && t[3] >= 0xA6 && t[3] <= 0xBF;
}
}  // namespace detail

// _lv_txt_encoded_next: a letter, or a flag when two regional indicators start here.
inline uint32_t next(const char* txt, uint32_t* i) {
  uint32_t tmp = 0;
  if (!i) i = &tmp;
  const uint32_t a = detail::next(txt, i);
  if (!isRi(a) || txt[*i] == '\0') return a;
  uint32_t j = *i;
  const uint32_t b = detail::next(txt, &j);
  if (!isRi(b)) return a;
  *i = j;
  return key(a, b);
}

// _lv_txt_encoded_prev: the letter ending at *i (a flag's pair when it ends one).
inline uint32_t prev(const char* txt, uint32_t* i) {
  if (!i || *i == 0) return 0;
  const uint32_t b = detail::prev(txt, i);
  if (!isRi(b)) return b;
  // How many indicators run back to back before *i's end: pairs are counted from
  // the run's start, so an even count means this one closes a pair.
  const uint32_t p = *i;   // b's start
  uint32_t k = 1, q = p;
  while (q >= 4 && detail::riAt(txt, q - 4)) {
    ++k;
    q -= 4;
  }
  if (k % 2 == 0) {
    uint32_t s = p - 4;
    uint32_t tmp = s;
    const uint32_t a = detail::next(txt, &tmp);
    *i = s;
    return key(a, b);
  }
  return b;
}

// _lv_txt_encoded_get_byte_id: the byte where letter number `id` starts.
inline uint32_t getByteId(const char* txt, uint32_t id) {
  uint32_t b = 0;
  for (uint32_t n = 0; n < id && txt[b] != '\0'; ++n) {
    const uint32_t before = b;
    next(txt, &b);
    if (b == before) ++b;   // never stall
  }
  return b;
}

// _lv_txt_encoded_get_char_id / get_length, as LVGL makes them from next (for the tests).
inline uint32_t getCharId(const char* txt, uint32_t byte_id) {
  uint32_t i = 0, n = 0;
  while (i < byte_id) {
    next(txt, &i);
    ++n;
  }
  return n;
}
inline uint32_t getLength(const char* txt) {
  uint32_t i = 0, n = 0;
  while (txt[i] != '\0') {
    next(txt, &i);
    ++n;
  }
  return n;
}

}  // namespace flagtext
