// SPDX-License-Identifier: GPL-3.0-or-later
//
// ClientRepeat.h: the off-grid settings Client Repeat moves to, and the
// remembered settings it puts back.
// g++ -std=c++17 -I src test/test_client_repeat.cpp -o /tmp/t && /tmp/t

#include <assert.h>
#include <stdio.h>

#include "helpers/ClientRepeat.h"

using namespace clientrepeat;

static const Range kDefault[] = {{433000, 433000}, {869495, 869495}, {918000, 918000}};

static Radio R(float f, float bw, uint8_t sf, uint8_t cr) {
  Radio r;
  r.freq = f; r.bw = bw; r.sf = sf; r.cr = cr;
  return r;
}

int main() {
  // US main mesh -> 918.000, 62.5 kHz, SF7, CR 4/5 (the app's Off-Grid Repeat).
  Radio r = offGrid(910.525f, kDefault, 3);
  assert(same(r, R(918.0f, 62.5f, 7, 5)));
  assert(same(offGrid(927.875f, kDefault, 3), R(918.0f, 62.5f, 7, 5)));
  // EU868 and 433.
  assert(same(offGrid(869.618f, kDefault, 3), R(869.495f, 62.5f, 7, 5)));
  assert(same(offGrid(433.875f, kDefault, 3), R(433.0f, 62.5f, 7, 5)));
  // Already there: unchanged frequency.
  assert(same(offGrid(918.0f, kDefault, 3), R(918.0f, 62.5f, 7, 5)));
  // A board's own range clamps instead.
  const Range wide[] = {{902000, 928000}};
  assert(same(offGrid(910.525f, wide, 1), R(910.525f, 62.5f, 7, 5)));
  assert(same(offGrid(869.0f, wide, 1), R(902.0f, 62.5f, 7, 5)));
  // No ranges: the frequency stays.
  assert(offGrid(910.525f, nullptr, 0).freq == 910.525f);

  // radio2 beside it: 918.800, 250 kHz, SF7, CR 4/5; below near the band's top;
  // none outside the US band.
  Radio r2;
  assert(radio2For(R(918.0f, 62.5f, 7, 5), r2) && same(r2, R(918.8f, 250.0f, 7, 5)));
  assert(radio2For(R(927.8f, 62.5f, 7, 5), r2) && same(r2, R(927.0f, 250.0f, 7, 5)));
  assert(!radio2For(R(869.495f, 62.5f, 7, 5), r2));
  assert(!radio2For(R(433.0f, 62.5f, 7, 5), r2));
  // Clear of the main channel by Keymind's rule.
  assert(r2.freq - 918.0f > 0);
  radio2For(R(918.0f, 62.5f, 7, 5), r2);
  assert((r2.freq - 918.0f) * 1000.0f > (62.5f + 250.0f) / 2.0f + 6.0f);

  // The remembered settings round-trip; damaged ones are refused.
  uint8_t b[kBlobSize];
  const Radio main = R(910.525f, 62.5f, 7, 5);
  encode(main, b);
  Radio back;
  assert(decode(b, sizeof b, back) && same(back, main));
  assert(!decode(b, sizeof b - 1, back));
  assert(!decode(nullptr, 0, back));
  b[2] = 2;
  assert(!decode(b, sizeof b, back));
  encode(R(910.525f, 62.5f, 4, 5), b);
  assert(!decode(b, sizeof b, back));
  encode(R(0.0f, 62.5f, 7, 5), b);
  assert(!decode(b, sizeof b, back));

  printf("test_client_repeat: ok\n");
  return 0;
}
