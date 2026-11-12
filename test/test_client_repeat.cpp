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
  // US main mesh -> 918.000, 250 kHz, SF11, CR 4/8 (the app's Off-Grid 918).
  Radio r = offGrid(910.525f, kDefault, 3);
  assert(same(r, R(918.0f, 250.0f, 11, 8)));
  assert(same(offGrid(927.875f, kDefault, 3), R(918.0f, 250.0f, 11, 8)));
  // EU868 and 433.
  assert(same(offGrid(869.618f, kDefault, 3), R(869.495f, 250.0f, 11, 8)));
  assert(same(offGrid(433.875f, kDefault, 3), R(433.0f, 250.0f, 11, 8)));
  // Already there: unchanged frequency.
  assert(same(offGrid(918.0f, kDefault, 3), R(918.0f, 250.0f, 11, 8)));
  // A board's own range clamps instead.
  const Range wide[] = {{902000, 928000}};
  assert(same(offGrid(910.525f, wide, 1), R(910.525f, 250.0f, 11, 8)));
  assert(same(offGrid(869.0f, wide, 1), R(902.0f, 250.0f, 11, 8)));
  // No ranges: the frequency stays.
  assert(offGrid(910.525f, nullptr, 0).freq == 910.525f);

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
