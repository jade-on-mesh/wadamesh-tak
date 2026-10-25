// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxBrighten.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr float kSilenceDb = -60.0f;
constexpr float kMaxGainDb = 40.0f;
constexpr float kFullScale2 = 32768.0f * 32768.0f;
constexpr float kKnee = 24000.0f, kCeiling = 32000.0f;   // as VoxPlayOut's limiter

constexpr float kBandEdges[VoxBrighten::kBands][2] = {
    {4000, 5000}, {5000, 6000}, {6000, 7000}, {7000, 7900}};

// The rule (a, b, c per sub-band), fitted by scripts/voice/brighten_ref.py on
// LibriSpeech 198-209-0000, 3436-172162-0000 and 5703-47212-0000 (CC-BY-4.0)
// through Codec2 1200. Keep in step with COEF there.
constexpr float kRule[VoxBrighten::kBands][3] = {
    {-7.835f, -0.244f, -0.636f},
    {-15.876f, -0.229f, -0.845f},
    {-21.726f, -0.396f, -0.946f},
    {-33.911f, -0.483f, -0.981f},
};

enum Kind { kLow, kHigh, kBand };

// RBJ cookbook biquad, normalised by a0. Designed in double, as the reference.
void design(float* c, Kind kind, double f, double rate, double q = 0.70710678) {
  const double w0 = 2 * M_PI * f / rate;
  const double cw = cos(w0), al = sin(w0) / (2 * q);
  double b0, b1, b2;
  if (kind == kLow) { b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; }
  else if (kind == kHigh) { b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; }
  else { b0 = al; b1 = 0; b2 = -al; }   // band-pass, 0 dB peak
  const double a0 = 1 + al;
  c[0] = (float)(b0 / a0); c[1] = (float)(b1 / a0); c[2] = (float)(b2 / a0);
  c[3] = (float)(-2 * cw / a0); c[4] = (float)((1 - al) / a0);
}

float levelDb(double energy, size_t n) {
  return 10.0f * log10f((float)(energy / (double)n) / kFullScale2 + 1e-12f);
}

float softLimit(float y) {
  const float m = fabsf(y);
  if (m <= kKnee) return y;
  const float e = m - kKnee;
  const float r = kKnee + (kCeiling - kKnee) * e / (e + (kCeiling - kKnee));
  return y < 0 ? -r : r;
}

}  // namespace

bool VoxBrighten::begin(uint32_t rate) {
  end();
  if (rate < kMinRate) return false;
  frame_ = rate / 100;
  buf_ = (float*)calloc(2 * (1 + kBands) * frame_, sizeof(float));
  if (!buf_) { frame_ = 0; return false; }
  auto set = [&](Biquad& q, Kind k, double f, double qv = 0.70710678) {
    float c[5];
    design(c, k, f, rate, qv);
    q = Biquad();
    q.b0 = c[0]; q.b1 = c[1]; q.b2 = c[2]; q.a1 = c[3]; q.a2 = c[4];
  };
  set(src_[0], kHigh, 2000); set(src_[1], kHigh, 2000);
  set(src_[2], kLow, 3900);  set(src_[3], kLow, 3900);
  set(lo_[0], kHigh, 300);   set(lo_[1], kLow, 1000);
  set(dc_, kHigh, 100);
  set(hb_[0], kHigh, 4000);  set(hb_[1], kHigh, 4000);
  for (int k = 0; k < kBands; k++) {
    const double lo = kBandEdges[k][0], hi = kBandEdges[k][1], fc = sqrt(lo * hi);
    set(band_[k][0], kBand, fc, fc / (hi - lo));
    set(band_[k][1], kBand, fc, fc / (hi - lo));
  }
  pos_ = 0;
  fill_ = 0;
  e_src_ = e_lo_ = 0;
  for (int k = 0; k < kBands; k++) { e_band_[k] = 0; g_from_[k] = g_to_[k] = 0; }
  return true;
}

void VoxBrighten::end() {
  free(buf_);
  buf_ = nullptr;
  frame_ = 0;
}

void VoxBrighten::targets(float m, float l, float* t) const {
  for (int k = 0; k < kBands; k++)
    t[k] = m + kRule[k][0] + kRule[k][1] * (m - l) + kRule[k][2] * (l + 30.0f);
}

void VoxBrighten::endFrame() {
  const float m = levelDb(e_src_, frame_);
  const float l = levelDb(e_lo_, frame_);
  float t[kBands];
  targets(m, l, t);
  for (int k = 0; k < kBands; k++) {
    float g = 0;
    if (m >= kSilenceDb) {
      float db = t[k] - levelDb(e_band_[k], frame_);
      if (db > kMaxGainDb) db = kMaxGainDb;
      g = powf(10.0f, db / 20.0f);
    }
    g_from_[k] = g_to_[k];
    g_to_[k] = g;
    e_band_[k] = 0;
  }
  e_src_ = e_lo_ = 0;
  pos_ = 0;
  fill_ ^= 1;
}

float VoxBrighten::push(float x) {
  const size_t plane = frame_;
  float* fill = buf_ + (size_t)fill_ * (1 + kBands) * plane;
  const float* play = buf_ + (size_t)(fill_ ^ 1) * (1 + kBands) * plane;

  // Analysis of the new sample.
  float s = x;
  for (Biquad& q : src_) s = q.run(s);
  const float l = lo_[1].run(lo_[0].run(x));
  const float e = hb_[1].run(hb_[0].run(dc_.run(fabsf(s))));
  e_src_ += (double)s * s;
  e_lo_ += (double)l * l;
  fill[pos_] = x;
  for (int k = 0; k < kBands; k++) {
    const float b = band_[k][1].run(band_[k][0].run(e));
    e_band_[k] += (double)b * b;
    fill[(1 + k) * plane + pos_] = b;
  }

  // The previous frame out, with its gains.
  const float r = (float)(pos_ + 1) / (float)plane;
  float y = play[pos_];
  for (int k = 0; k < kBands; k++)
    y += (g_from_[k] + (g_to_[k] - g_from_[k]) * r) * play[(1 + k) * plane + pos_];

  if (++pos_ == plane) endFrame();
  return softLimit(y);
}

void VoxBrighten::process(const int16_t* in, int16_t* out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!buf_) { out[i] = in[i]; continue; }
    out[i] = (int16_t)lrintf(push((float)in[i]));
  }
}

size_t VoxBrighten::flush(int16_t* out, size_t cap) {
  if (!buf_) return 0;
  size_t n = 0;
  for (size_t i = 0; i < frame_; i++) {
    const float y = push(0.0f);
    if (n < cap) out[n++] = (int16_t)lrintf(y);
  }
  return n;
}
