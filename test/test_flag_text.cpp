// SPDX-License-Identifier: GPL-3.0-or-later
//
// FlagText.h: a pair of regional indicators reads as one letter, forwards and back,
// and plain text reads exactly as LVGL's UTF-8 helpers read it.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/FlagText.h"

using namespace flagtext;

#define US "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"   // 🇺🇸
#define LS "\xF0\x9F\x87\xB1\xF0\x9F\x87\xB8"   // 🇱🇸 (the baked Lesotho)
#define FR "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7"   // 🇫🇷
#define RI_A "\xF0\x9F\x87\xA6"                  // 🇦 on its own

static const uint32_t kUS = key(0x1F1FA, 0x1F1F8), kLS = key(0x1F1F1, 0x1F1F8), kFR = key(0x1F1EB, 0x1F1F7);

// Every letter forwards, then the same backwards from the end.
static int lettersFwd(const char* s, uint32_t* out, uint32_t* at, int cap) {
  uint32_t i = 0;
  int n = 0;
  while (s[i] && n < cap) {
    at[n] = i;
    out[n++] = next(s, &i);
  }
  return n;
}
static void checkBack(const char* s, const uint32_t* fwd, const uint32_t* at, int n) {
  uint32_t i = (uint32_t)strlen(s);
  for (int k = n - 1; k >= 0; --k) {
    const uint32_t c = prev(s, &i);
    if (c != fwd[k] || i != at[k]) {
      fprintf(stderr, "prev mismatch in \"%s\" at %d: got %X@%u want %X@%u\n", s, k, c, i, fwd[k], at[k]);
      assert(0);
    }
  }
  assert(i == 0);
}

static void check(const char* s, const uint32_t* want, int wn) {
  uint32_t got[32], at[32];
  const int n = lettersFwd(s, got, at, 32);
  if (n != wn) { fprintf(stderr, "\"%s\": %d letters, want %d\n", s, n, wn); assert(0); }
  for (int k = 0; k < n; ++k) {
    if (got[k] != want[k]) { fprintf(stderr, "\"%s\" letter %d: %X want %X\n", s, k, got[k], want[k]); assert(0); }
    assert(getByteId(s, (uint32_t)k) == at[k]);
    assert(getCharId(s, at[k]) == (uint32_t)k);
  }
  assert(getLength(s) == (uint32_t)n);
  assert(getByteId(s, (uint32_t)n) == strlen(s));
  checkBack(s, got, at, n);
}

int main() {
  assert(isKey(kUS) && !isKey(0x1F1FA) && !isKey(0x10FFFF));
  char iso[3];
  keyLetters(kUS, iso);
  assert(strcmp(iso, "US") == 0);
  keyLetters(kLS, iso);
  assert(strcmp(iso, "LS") == 0);

  { const uint32_t w[] = {'h', 'i', ' ', kUS, '!'}; check("hi " US "!", w, 5); }
  { const uint32_t w[] = {kLS}; check(LS, w, 1); }
  { const uint32_t w[] = {kUS, kFR, kLS}; check(US FR LS, w, 3); }     // back to back: paired from the start
  { const uint32_t w[] = {kUS, 0x1F1E6}; check(US RI_A, w, 2); }       // an odd one left over
  { const uint32_t w[] = {0x1F1E6}; check(RI_A, w, 1); }
  { const uint32_t w[] = {'a', 0x1F1E6, 'b'}; check("a" RI_A "b", w, 3); }
  // Plain text: as LVGL reads it (2-, 3- and 4-byte letters, other emoji).
  { const uint32_t w[] = {'A', 0xC1, 'R', 0x20AC, 0x1F600}; check("A\xC3\x81R\xE2\x82\xAC\xF0\x9F\x98\x80", w, 5); }
  // An emoji next to a flag stays its own letter.
  { const uint32_t w[] = {0x1F600, kFR, 0x1F600}; check("\xF0\x9F\x98\x80" FR "\xF0\x9F\x98\x80", w, 3); }
  // Ends of text are safe.
  { uint32_t i = 0; assert(next("", &i) == 0); }
  { uint32_t i = 0; assert(prev("abc", &i) == 0 && i == 0); }
  assert(next(US, nullptr) == kUS);
  // A flag cut in half (a buffer ending after its first letter) is one letter.
  { const uint32_t w[] = {'x', 0x1F1FA}; check("x\xF0\x9F\x87\xBA", w, 2); }
  puts("test_flag_text: ok");
  return 0;
}
