// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// "Brighter voice": fills in the 4-8 kHz band Codec2 leaves out, on the
// standalone itself (Settings > Sound). Codec2 stops at 4 kHz, so "s", "f" and
// "t" and the air in a voice are missing and clips sound muffled.
//
// The band is made from the decoded voice: its 2-3.9 kHz band, full-wave
// rectified (which puts energy at the sums of its frequencies, 4-7.8 kHz),
// high-passed at 4 kHz and split into four sub-bands. Every 10 ms each sub-band
// gets the gain that brings it to the level a fitted rule says real speech
// would have there, from two levels of the decoded voice:
//   target_k = m + a_k + b_k (m - l) + c_k (l + 30)   [dBFS]
// m = the 2-3.9 kHz band, l = 300-1000 Hz. The rule's 12 numbers were fitted on
// wideband speech through Codec2 1200 (scripts/voice/brighten_ref.py, the
// reference this is tested against).
//
// With VoxBrightNet.h next to this file (made by MeshCore Open TAK's
// tools/voice-training), a tiny net sets the targets instead: from m, l and
// eight bands of 300-3900 Hz over the last 40 ms (40 numbers), a 40-32-32-4
// MLP, once per 10 ms. Its input is the same arithmetic as the kit's
// kit/dsp.py, so it hears here what it learnt from.
//
// Cheap: about 20 biquads per output sample, an abs() and four gains per
// 10 ms, so a few percent of one ESP32-S3 core at 16 kHz. Works at any output
// rate from 16 kHz up (filters are designed at begin()). One frame (10 ms) of
// delay: a frame's gains are known when it ends.
//
// Platform-neutral and host-tested (test/test_vox_brighten.cpp).

#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include("VoxBrightNet.h")
#include "VoxBrightNet.h"
#endif
#endif
#ifndef VOX_BRIGHT_NET
#define VOX_BRIGHT_NET 0
#endif

class VoxBrighten {
 public:
  static constexpr int kBands = 4;
  static constexpr uint32_t kMinRate = 16000;
  // Whether the trained net sets the levels (else the rule).
  static constexpr bool kUsesNet = VOX_BRIGHT_NET != 0;

  VoxBrighten() {}
  ~VoxBrighten() { end(); }
  VoxBrighten(const VoxBrighten&) = delete;
  VoxBrighten& operator=(const VoxBrighten&) = delete;

  // false: rate under kMinRate (no room above 4 kHz) or no memory.
  bool begin(uint32_t rate);
  void end();
  bool active() const { return buf_ != nullptr; }

  // Samples per 10 ms frame, the delay.
  size_t frameSamples() const { return frame_; }

  // n samples of played-out voice (VoxPlayOut's output, at the rate given to
  // begin()) in, n samples out, a frame later: the first frame out is silence.
  // `out` may be `in`.
  void process(const int16_t* in, int16_t* out, size_t n);

  // At the end of a clip: what is still held, a frame (frameSamples()
  // samples, the rest of the clip followed by silence). Returns samples written.
  size_t flush(int16_t* out, size_t cap);

 private:
  struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    float run(float x) {
      const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1; x1 = x;
      y2 = y1; y1 = y;
      return y;
    }
  };

  float push(float x);
  void endFrame();
  void targets(float m, float l, float* t) const;

  size_t frame_ = 0, pos_ = 0;
  int fill_ = 0;         // which half of buf_ is filling; the other plays
  float* buf_ = nullptr; // 2 frames x (1 + kBands) planes
  Biquad src_[4], lo_[2], dc_, hb_[2], band_[kBands][2];
  double e_src_ = 0, e_lo_ = 0, e_band_[kBands] = {};
#if VOX_BRIGHT_NET
  static constexpr int kAna = 8, kFeat = 2 + kAna;
  static_assert(kVoxBrightNetIn == kFeat * kVoxBrightNetContext, "VoxBrightNet.h doesn't match");
  Biquad ana_[kAna][2];
  double e_ana_[kAna] = {};
  float hist_[kVoxBrightNetIn];   // features, this frame first
#endif
  // The frame being played ramps from the gains of the frame before it to its own.
  float g_from_[kBands] = {}, g_to_[kBands] = {};
};
