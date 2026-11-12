// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Client Repeat: a standalone forwards other nodes' packets on the off-grid
// channel: the allowed client-repeat frequency nearest the current one
// (918.000 MHz in the US, 869.495 in EU868, 433.000 at 433) in the standard
// shape, 62.5 kHz, SF7, CR 4/5, as MeshCore Open TAK's Off-Grid Repeat sets it.
// Turning it on remembers the radio settings it replaced; turning it off puts
// them back. In the US band radio2 runs MediaMesh beside it, 250 kHz above at
// 250 kHz, SF7, CR 4/5 (radio2For); elsewhere radio2 is held off
// (MediaMeshCore.h, holdForClientRepeat). Pure, so the host test runs it
// (test/test_client_repeat.cpp); MyMesh keeps the remembered settings.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace clientrepeat {

struct Radio {
  float freq = 0;   // MHz
  float bw = 0;     // kHz
  uint8_t sf = 0;
  uint8_t cr = 0;   // 5..8 = 4/5..4/8
};

// The off-grid main radio's settings apart from the frequency.
constexpr float kBw = 62.5f;
constexpr uint8_t kSf = 7;
constexpr uint8_t kCr = 5;

// radio2 beside it: MediaMesh's fast preset, its centre this far from the
// main channel's. Clear of a 62.5 kHz main channel with room to spare
// (keymindCascade warns under (62.5 + 250) / 2 + 6 = 162.25 kHz).
constexpr float kRadio2Bw = 250.0f;
constexpr uint8_t kRadio2Sf = 7;
constexpr uint8_t kRadio2Cr = 5;
constexpr float kRadio2OffsetMhz = 0.250f;

struct Range { uint32_t lo_khz, hi_khz; };

/** The off-grid settings for a radio now on `freq_mhz`: the allowed frequency
 *  nearest it (each range clamps it; the closest wins, the first on a tie). */
inline Radio offGrid(float freq_mhz, const Range* ranges, int n) {
  Radio r;
  r.bw = kBw; r.sf = kSf; r.cr = kCr;
  const int64_t cur = (int64_t)lround((double)freq_mhz * 1000.0);
  int64_t best = -1, best_d = 0;
  for (int i = 0; i < n; ++i) {
    const int64_t lo = ranges[i].lo_khz, hi = ranges[i].hi_khz;
    const int64_t c = cur < lo ? lo : cur > hi ? hi : cur;
    const int64_t d = c > cur ? c - cur : cur - c;
    if (best < 0 || d < best_d) { best = c; best_d = d; }
  }
  r.freq = best < 0 ? freq_mhz : (float)((double)best / 1000.0);
  return r;
}

/** The radio2 Client Repeat runs beside an off-grid main radio on `main`: in
 *  the US band (902-928 MHz), 250 kHz above it (below, if above leaves the
 *  band). False elsewhere, where radio2 stays off: EU868's off-grid channel
 *  has no room beside it in its sub-band. */
inline bool radio2For(const Radio& main, Radio& out) {
  if (!(main.freq >= 902.0f && main.freq <= 928.0f)) return false;
  const float half = kRadio2Bw / 2000.0f;
  float f = main.freq + kRadio2OffsetMhz;
  if (f + half > 928.0f) f = main.freq - kRadio2OffsetMhz;
  if (f - half < 902.0f) return false;
  out.freq = (float)((double)lround((double)f * 1000.0) / 1000.0);
  out.bw = kRadio2Bw;
  out.sf = kRadio2Sf;
  out.cr = kRadio2Cr;
  return true;
}

inline bool same(const Radio& a, const Radio& b) {
  return lround(a.freq * 1000.0f) == lround(b.freq * 1000.0f) && lround(a.bw * 10.0f) == lround(b.bw * 10.0f)
         && a.sf == b.sf && a.cr == b.cr;
}

// The remembered settings: "CR", version 1, freq and bw (floats), sf, cr.
constexpr size_t kBlobSize = 13;

inline void encode(const Radio& r, uint8_t out[kBlobSize]) {
  out[0] = 'C'; out[1] = 'R'; out[2] = 1;
  memcpy(&out[3], &r.freq, 4);
  memcpy(&out[7], &r.bw, 4);
  out[11] = r.sf;
  out[12] = r.cr;
}

inline bool decode(const uint8_t* in, size_t len, Radio& out) {
  if (!in || len < kBlobSize || in[0] != 'C' || in[1] != 'R' || in[2] != 1) return false;
  Radio r;
  memcpy(&r.freq, &in[3], 4);
  memcpy(&r.bw, &in[7], 4);
  r.sf = in[11];
  r.cr = in[12];
  if (!(r.freq >= 300.0f && r.freq <= 2500.0f) || !(r.bw >= 7.0f && r.bw <= 1700.0f) || r.sf < 5 || r.sf > 12
      || r.cr < 5 || r.cr > 8)
    return false;
  out = r;
  return true;
}

}  // namespace clientrepeat
