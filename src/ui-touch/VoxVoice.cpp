// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxVoice.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "VoxWire.h"

#if VOX_CODEC2_AVAILABLE

#include <codec2/codec2.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

// lib/codec2 is built with __EMBEDDED__, which routes its allocations here.
// Its state is a few KB; PSRAM first, internal RAM if PSRAM is short.
extern "C" {
void* codec2_malloc(size_t size) {
#ifdef ESP_PLATFORM
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
#else
  return malloc(size);
#endif
}
void* codec2_calloc(size_t nmemb, size_t size) {
  if (size && nmemb > (size_t)-1 / size) return nullptr;
  void* p = codec2_malloc(nmemb * size);
  if (p) memset(p, 0, nmemb * size);
  return p;
}
void codec2_free(void* ptr) {
#ifdef ESP_PLATFORM
  heap_caps_free(ptr);
#else
  free(ptr);
#endif
}
}

bool voxCodec2CanDecode(int wire_mode) {
  const int m = VoxWire::codec2Mode(wire_mode);
  return m >= 0 && m != CODEC2_MODE_700C;   // 700C: compiled out (CODEC2_MODE_700C_EN=0)
}

// Frame geometry per mode, without starting a codec: 40 ms frames except 2400
// and 3200 (20 ms), as in voice_wire.dart.
static int samplesPerFrame(int wire_mode) {
  return (wire_mode == 2 || wire_mode == 6) ? 160 : 320;
}

size_t voxCodec2Samples(int wire_mode, size_t len) {
  const int bpf = VoxWire::bytesPerFrame(wire_mode);
  if (!voxCodec2CanDecode(wire_mode) || bpf <= 0) return 0;
  return (len / (size_t)bpf) * (size_t)samplesPerFrame(wire_mode);
}

long voxCodec2Decode(int wire_mode, const uint8_t* bits, size_t len, int16_t* out, size_t out_cap) {
  if (!voxCodec2CanDecode(wire_mode) || !bits || !out) return -1;
  struct CODEC2* c2 = codec2_create(VoxWire::codec2Mode(wire_mode));
  if (!c2) return -1;
  const int bpf = codec2_bytes_per_frame(c2);
  const int spf = codec2_samples_per_frame(c2);
  const size_t frames = len / (size_t)bpf;
  if (bpf != VoxWire::bytesPerFrame(wire_mode) || spf != samplesPerFrame(wire_mode) ||
      frames * (size_t)spf > out_cap) {
    codec2_destroy(c2);
    return -1;
  }
  for (size_t i = 0; i < frames; i++) {
    // codec2_decode takes a non-const pointer but only reads the frame.
    codec2_decode(c2, out + i * (size_t)spf, const_cast<unsigned char*>(bits + i * (size_t)bpf));
  }
  codec2_destroy(c2);
  return (long)(frames * (size_t)spf);
}

// Bars from per-bar sums of squares: RMS, square-rooted against the loudest
// (voxWaveform in the app's walkie_wire.dart).
static void waveFinish(const double* sum, const uint32_t* cnt, uint8_t* bars, int n_bars) {
  double top = 0;
  for (int b = 0; b < n_bars; b++) {
    const double r = cnt[b] ? sqrt(sum[b] / cnt[b]) : 0;
    if (r > top) top = r;
  }
  for (int b = 0; b < n_bars; b++) {
    if (top < 200) { bars[b] = 0; continue; }   // silence
    const double r = cnt[b] ? sqrt(sum[b] / cnt[b]) : 0;
    const double v = sqrt(r / top);
    bars[b] = (uint8_t)lrint((v > 1 ? 1 : v) * 100);
  }
}

bool voxCodec2Waveform(int wire_mode, const uint8_t* bits, size_t len, uint8_t* bars, int n_bars) {
  if (!bits || !bars || n_bars <= 0 || n_bars > 64) return false;
  VoxCodec2Stream c;
  if (!c.begin(wire_mode)) return false;
  const size_t bpf = (size_t)c.bytesPerFrame();
  const int spf = c.samplesPerFrame();
  const size_t frames = bpf ? len / bpf : 0;
  if (!frames || spf > 320) { c.end(); return false; }
  double sum[64] = {0};
  uint32_t cnt[64] = {0};
  const size_t total = frames * (size_t)spf;
  int16_t pcm[320];
  for (size_t f = 0; f < frames; f++) {
    c.decode(bits + f * bpf, pcm);
    for (int k = 0; k < spf; k++) {
      const size_t i = f * (size_t)spf + (size_t)k;
      const int b = (int)(i * (size_t)n_bars / total);
      sum[b] += (double)pcm[k] * pcm[k];
      cnt[b]++;
    }
  }
  c.end();
  waveFinish(sum, cnt, bars, n_bars);
  return true;
}

bool VoxCodec2Stream::begin(int wire_mode) {
  end();
  if (!voxCodec2CanDecode(wire_mode)) return false;
  struct CODEC2* c2 = codec2_create(VoxWire::codec2Mode(wire_mode));
  if (!c2) return false;
  bpf_ = codec2_bytes_per_frame(c2);
  spf_ = codec2_samples_per_frame(c2);
  c2_ = c2;
  return true;
}

void VoxCodec2Stream::decode(const uint8_t* frame, int16_t* out) {
  if (c2_) codec2_decode((struct CODEC2*)c2_, out, const_cast<unsigned char*>(frame));
}

void VoxCodec2Stream::end() {
  if (c2_) codec2_destroy((struct CODEC2*)c2_);
  c2_ = nullptr;
  bpf_ = spf_ = 0;
}

bool VoxCodec2Encoder::begin(int wire_mode) {
  end();
  if (!voxCodec2CanDecode(wire_mode)) return false;   // same set of modes both ways
  struct CODEC2* c2 = codec2_create(VoxWire::codec2Mode(wire_mode));
  if (!c2) return false;
  bpf_ = codec2_bytes_per_frame(c2);
  spf_ = codec2_samples_per_frame(c2);
  if (bpf_ != VoxWire::bytesPerFrame(wire_mode) || spf_ != ::samplesPerFrame(wire_mode)) {
    codec2_destroy(c2);
    bpf_ = spf_ = 0;
    return false;
  }
  c2_ = c2;
  return true;
}

void VoxCodec2Encoder::encode(int16_t* pcm, uint8_t* out) {
  if (c2_) codec2_encode((struct CODEC2*)c2_, out, pcm);
}

void VoxCodec2Encoder::end() {
  if (c2_) codec2_destroy((struct CODEC2*)c2_);
  c2_ = nullptr;
  bpf_ = spf_ = 0;
}

#else  // !VOX_CODEC2_AVAILABLE

bool VoxCodec2Stream::begin(int) { return false; }
void VoxCodec2Stream::decode(const uint8_t*, int16_t*) {}
void VoxCodec2Stream::end() {}

bool VoxCodec2Encoder::begin(int) { return false; }
void VoxCodec2Encoder::encode(int16_t*, uint8_t*) {}
void VoxCodec2Encoder::end() {}

bool voxCodec2CanDecode(int) { return false; }
size_t voxCodec2Samples(int, size_t) { return 0; }
long voxCodec2Decode(int, const uint8_t*, size_t, int16_t*, size_t) { return -1; }
bool voxCodec2Waveform(int, const uint8_t*, size_t, uint8_t*, int) { return false; }

#endif

// ---- VoxMicPrep ----------------------------------------------------------------
//
// Codec2 wants 8 kHz speech at a healthy level; the mic gives 16 kHz with a DC
// offset and a level that depends on how far away the talker is.
//
//   mix      (L + R) / 2. If one capsule is silent the AGC makes up the 6 dB.
//   DC block one-pole high-pass at ~13 Hz.
//   decimate 31-tap Hamming-windowed sinc low-pass, cutoff 3.4 kHz (well under
//            the 4 kHz Nyquist of the output), every other output kept.
//   AGC      peak follower (fast attack, slow release). Gain heads for a peak of
//            about -8 dBFS; it drops quickly when speech gets loud and rises
//            slowly, and never rises while the input is down at the noise floor,
//            so pauses don't get pumped up into hiss.
//   limiter  soft knee from 24000 towards 32000, so the occasional overshoot
//            before the AGC catches up rounds off instead of clipping.

namespace {

constexpr float kDcPole      = 0.995f;
constexpr float kTargetPeak  = 13000.0f;
constexpr float kNoiseFloor  = 250.0f;    // input peak below this: hold the gain
constexpr float kMinGain     = 0.5f;
constexpr float kMaxGain     = 24.0f;
constexpr float kEnvAttack   = 0.3f;      // per 8 kHz sample
constexpr float kEnvRelease  = 1.0f / (0.3f * 8000.0f);
constexpr float kGainDown    = 1.0f / (0.015f * 8000.0f);
constexpr float kGainUp      = 1.0f / (0.4f * 8000.0f);
constexpr float kKnee        = 24000.0f;
constexpr float kCeiling     = 32000.0f;
// Noise gate (as GridDown's): while the input sits at the noise floor (the
// mic's hiss between words), it is taken down 16 dB instead of encoded, so
// pauses come out quiet rather than as Codec2's rendering of hiss. Opens in
// ~2 ms, closes over ~60 ms once the envelope (which itself lingers ~0.3 s)
// has dropped, so words and their tails are never clipped.
constexpr float kGateOpen    = 180.0f;    // input peak envelope that opens the gate
constexpr float kGateFloor   = 0.16f;     // closed: -16 dB
constexpr float kGateAttack  = 1.0f / (0.002f * 8000.0f);
constexpr float kGateRelease = 1.0f / (0.060f * 8000.0f);

float g_fir[VoxMicPrep::kTaps];
bool  g_fir_ready = false;

void makeFir() {
  if (g_fir_ready) return;
  const int n = VoxMicPrep::kTaps;
  const double fc = 3400.0 / VoxMicPrep::kInputRate;   // cycles per input sample
  const double pi = 3.14159265358979323846;
  double sum = 0;
  double h[VoxMicPrep::kTaps];
  for (int i = 0; i < n; i++) {
    const double m = i - (n - 1) / 2.0;
    const double sinc = m == 0 ? 2 * fc : sin(2 * pi * fc * m) / (pi * m);
    const double win = 0.54 - 0.46 * cos(2 * pi * i / (n - 1));
    h[i] = sinc * win;
    sum += h[i];
  }
  for (int i = 0; i < n; i++) g_fir[i] = (float)(h[i] / sum);   // unity gain at DC
  g_fir_ready = true;
}

}  // namespace

void VoxMicPrep::reset() {
  makeFir();
  for (float& v : hist_) v = 0;
  pos_ = 0;
  odd_ = false;
  dc_x_ = dc_y_ = 0;
  env_ = 0;
  gain_ = 4.0f;   // a sensible start: speech at arm's length on the T-Deck's mics
  gate_ = 1.0f;   // open: the first syllable isn't faded in
  peak_ = 0;
}

int16_t VoxMicPrep::level(float s) {
  const float a = fabsf(s);
  env_ += (a - env_) * (a > env_ ? kEnvAttack : kEnvRelease);
  float want = kTargetPeak / (env_ > kNoiseFloor ? env_ : kNoiseFloor);
  if (want < kMinGain) want = kMinGain;
  if (want > kMaxGain) want = kMaxGain;
  if (want < gain_) gain_ += (want - gain_) * kGainDown;
  else if (env_ > kNoiseFloor) gain_ += (want - gain_) * kGainUp;
  const float gate_to = env_ > kGateOpen ? 1.0f : kGateFloor;
  gate_ += (gate_to - gate_) * (gate_to > gate_ ? kGateAttack : kGateRelease);
  float v = s * gain_ * gate_;
  const float m = fabsf(v);
  if (m > kKnee) {
    const float e = m - kKnee;
    const float r = kKnee + (kCeiling - kKnee) * e / (e + (kCeiling - kKnee));
    v = v < 0 ? -r : r;
  }
  const int out = (int)lrintf(v);
  const int mag = out < 0 ? -out : out;
  if (mag > peak_) peak_ = mag;
  return (int16_t)(out > 32767 ? 32767 : (out < -32768 ? -32768 : out));
}

void VoxMicPrep::push(float x, int16_t* out, size_t cap, size_t* n) {
  // DC block, then into the FIR's history.
  const float y = x - dc_x_ + kDcPole * dc_y_;
  dc_x_ = x;
  dc_y_ = y;
  hist_[pos_] = y;
  pos_ = (pos_ + 1) % kTaps;
  odd_ = !odd_;
  if (odd_) return;   // keep every second filtered sample
  float acc = 0;
  int k = pos_;   // oldest sample
  for (int i = 0; i < kTaps; i++) {
    acc += g_fir[i] * hist_[k];
    if (++k == kTaps) k = 0;
  }
  if (*n < cap) out[(*n)++] = level(acc);
}

size_t VoxMicPrep::process(const int16_t* stereo, size_t frames, int16_t* out, size_t cap) {
  size_t n = 0;
  if (!stereo || !out) return 0;
  for (size_t i = 0; i < frames; i++)
    push(0.5f * ((float)stereo[2 * i] + (float)stereo[2 * i + 1]), out, cap, &n);
  return n;
}

size_t VoxMicPrep::processMono(const int16_t* mono, size_t count, int16_t* out, size_t cap) {
  size_t n = 0;
  if (!mono || !out) return 0;
  for (size_t i = 0; i < count; i++) push((float)mono[i], out, cap, &n);
  return n;
}

int VoxMicPrep::takePeak() {
  const int p = peak_;
  peak_ = 0;
  return p;
}


// ---- VoxPlayOut -----------------------------------------------------------------

void VoxPlayOut::begin(uint32_t out_rate, float gain) {
  gain_ = gain;
  step_ = out_rate ? (float)kVoxVoiceSampleRate / (float)out_rate : 1.0f;
  rel_ = 1.0f;
  prev_ = 0;
  x1_ = x2_ = y1_ = y2_ = 0;
  env_ = 0;
  gate_ = 1.0f;   // open: the first syllable isn't faded in
  lpf_ = out_rate > (uint32_t)kVoxVoiceSampleRate;
  if (lpf_) {   // RBJ low-pass, fc 3.6 kHz, Q 0.7071, at the output rate
    const double pi = 3.14159265358979323846;
    const double w0 = 2 * pi * 3600.0 / (double)out_rate;
    const double alpha = sin(w0) / (2 * 0.70710678);
    const double c = cos(w0);
    const double a0 = 1 + alpha;
    b0_ = (float)(((1 - c) / 2) / a0);
    b1_ = (float)((1 - c) / a0);
    b2_ = b0_;
    a1_ = (float)((-2 * c) / a0);
    a2_ = (float)((1 - alpha) / a0);
  }
}

// The pause gate, per 8 kHz sample: a peak envelope (30 ms decay), opening in
// 2 ms and closing over 120 ms (longer than the sender's 60 ms: a word's quiet
// tail isn't clipped), down to -16 dB.
constexpr float kPlayGateFloor   = 0.16f;
constexpr float kPlayEnvDecay    = 0.99584f;                  // exp(-1 / (0.030 * 8000))
constexpr float kPlayGateAttack  = 1.0f / (0.002f * 8000.0f);
constexpr float kPlayGateRelease = 1.0f / (0.120f * 8000.0f);

void VoxPlayOut::push(int16_t s, int16_t* out, size_t cap, size_t* n) {
  float cur = (float)s * gain_;
  if (gate_open_ > 0) {
    const float a = fabsf((float)s);
    env_ = a > env_ ? a : env_ * kPlayEnvDecay;
    const float to = env_ > gate_open_ ? 1.0f : kPlayGateFloor;
    gate_ += (to - gate_) * (to > gate_ ? kPlayGateAttack : kPlayGateRelease);
    cur *= gate_;
  }
  while (rel_ <= 1.0f) {
    float y = prev_ + (cur - prev_) * rel_;
    if (lpf_) {
      const float f = b0_ * y + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
      x2_ = x1_; x1_ = y;
      y2_ = y1_; y1_ = f;
      y = f;
    }
    const float m = fabsf(y);   // soft limiter, as the mic path's
    if (m > kKnee) {
      const float e = m - kKnee;
      const float r = kKnee + (kCeiling - kKnee) * e / (e + (kCeiling - kKnee));
      y = y < 0 ? -r : r;
    }
    if (*n < cap) out[(*n)++] = (int16_t)lrintf(y);
    rel_ += step_;
  }
  rel_ -= 1.0f;
  prev_ = cur;
}

float voxPlayNormGain(int peak) {
  if (peak <= 0) return 1.0f;
  float g = 23000.0f / (float)peak;   // ~-3 dBFS
  if (g < 1.0f) g = 1.0f;
  if (g > 6.0f) g = 6.0f;              // a near-silent clip isn't turned into hiss
  return g;
}

float voxPlayGateLevel(int peak) {
  return peak > 0 ? (float)peak * 0.015849f : 0.0f;   // -36 dB
}

int voxCodec2Peak(int wire_mode, const uint8_t* bits, size_t len, size_t max_samples) {
  if (!bits) return -1;
  VoxCodec2Stream c;
  if (!c.begin(wire_mode)) return -1;
  const size_t bpf = (size_t)c.bytesPerFrame();
  const int spf = c.samplesPerFrame();
  if (!bpf || spf > 320) { c.end(); return -1; }
  int16_t pcm[320];
  int peak = 0;
  size_t done = 0;
  for (size_t i = 0; i + bpf <= len && done < max_samples; i += bpf, done += (size_t)spf) {
    c.decode(bits + i, pcm);
    for (int k = 0; k < spf; k++) {
      const int a = pcm[k] < 0 ? -pcm[k] : pcm[k];
      if (a > peak) peak = a;
    }
  }
  c.end();
  return peak;
}
