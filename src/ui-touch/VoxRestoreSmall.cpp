// SPDX-License-Identifier: GPL-3.0-or-later
// The small AI voice restorer (VoxRestoreSmall.h). Follows kit/small.py's
// numpy reference in MeshCore Open TAK's voice training kit step for step.

#include "VoxRestoreSmall.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
static void* vrsMalloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void vrsFree(void* p) { heap_caps_free(p); }
#else
static void* vrsMalloc(size_t n) { return malloc(n); }
static void vrsFree(void* p) { free(p); }
#endif

namespace {

constexpr int kInSr = 8000, kOutSr = 16000;
constexpr int kInNfft = 256, kInHop = 64, kOutNfft = 512, kOutHop = 128;
constexpr int kInBins = kInNfft / 2 + 1;     // 129
constexpr int kOutBins = kOutNfft / 2 + 1;   // 257
constexpr int kK = 7;                        // conv width
constexpr int kHalfK = kK / 2;
constexpr int kTile = 16;                    // frames per pass over a weight matrix
constexpr int kHeader = 64;

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

float half(uint16_t h) {
  const uint32_t s = (uint32_t)(h & 0x8000) << 16;
  int e = (h >> 10) & 0x1F;
  uint32_t m = h & 0x3FF;
  uint32_t bits;
  if (e == 0) {
    if (m == 0) {
      bits = s;
    } else {   // subnormal: normalise
      e = 127 - 15 + 1;
      while (!(m & 0x400)) { m <<= 1; e--; }
      m &= 0x3FF;
      bits = s | ((uint32_t)e << 23) | (m << 13);
    }
  } else if (e == 31) {
    bits = s | 0x7F800000u | (m << 13);
  } else {
    bits = s | ((uint32_t)(e - 15 + 127) << 23) | (m << 13);
  }
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

uint32_t fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
  return h;
}

// Periodic Hann, as numpy's 0.5 - 0.5 cos(2 pi i / n).
float hannAt(int i, int n) { return 0.5f - 0.5f * cosf(6.2831853071795864f * (float)i / (float)n); }

float dot(const float* a, const float* b, int n) {
  float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  int i = 0;
  for (; i + 4 <= n; i += 4) {
    s0 += a[i] * b[i];
    s1 += a[i + 1] * b[i + 1];
    s2 += a[i + 2] * b[i + 2];
    s3 += a[i + 3] * b[i + 3];
  }
  for (; i < n; i++) s0 += a[i] * b[i];
  return (s0 + s1) + (s2 + s3);
}

void layerNorm(const float* x, float* y, const float* w, const float* b, int n) {
  float mu = 0;
  for (int i = 0; i < n; i++) mu += x[i];
  mu /= (float)n;
  float v = 0;
  for (int i = 0; i < n; i++) { const float d = x[i] - mu; v += d * d; }
  const float r = 1.0f / sqrtf(v / (float)n + 1e-6f);
  for (int i = 0; i < n; i++) y[i] = (x[i] - mu) * r * w[i] + b[i];
}

float gelu(float x) { return 0.5f * x * (1.0f + erff(x * 0.70710678118654752f)); }

}  // namespace

namespace voxrs {

void fft(float* re, float* im, int n, bool inverse) {
  for (int i = 1, j = 0; i < n; i++) {   // bit reversal
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const float ang = (inverse ? 6.2831853071795864f : -6.2831853071795864f) / (float)len;
    const float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        const int a = i + k, b = i + k + len / 2;
        const float tr = re[b] * cr - im[b] * ci;
        const float ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr;        im[a] += ti;
        const float nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
}

}  // namespace voxrs

bool VoxRestoreSmall::alloc(void** p, size_t bytes) {
  *p = vrsMalloc(bytes ? bytes : 1);
  return *p != nullptr;
}

void VoxRestoreSmall::unload() {
  if (w_) vrsFree(w_);
  if (embed_w_) vrsFree(embed_w_);
  if (blocks_) vrsFree(blocks_);
  w_ = nullptr;
  embed_w_ = nullptr;
  blocks_ = nullptr;
  dim_ = inter_ = layers_ = n_mels_ = 0;
}

uint32_t VoxRestoreSmall::macsPerSecond() const {
  const uint32_t per = (uint32_t)(n_mels_ * dim_ * kK + layers_ * (dim_ * kK + 2 * dim_ * inter_) + dim_ * (kOutNfft + 2));
  return per * (kInSr / kInHop);
}

bool VoxRestoreSmall::load(const uint8_t* f, size_t len, const char** err) {
  unload();
  const char* e = nullptr;
  if (!f || len < (size_t)kHeader || memcmp(f, "VRS1", 4) != 0) e = "not a small restorer model";
  const uint32_t hb = e ? 0 : rd32(f + 4);
  uint16_t h[12] = {0};
  if (!e) {
    for (int i = 0; i < 12; i++) h[i] = rd16(f + 8 + 2 * i);
    if (hb < (uint32_t)kHeader || h[0] != kInSr || h[1] != kOutSr || h[2] != kInNfft || h[3] != kInHop ||
        h[4] != kOutNfft || h[5] != kOutHop || h[10] != kK)
      e = "a model for another shape (newer kit?)";
  }
  const int n_mels = h[6], dim = h[7], inter = h[8], layers = h[9];
  uint32_t n = 0;
  float lmm = 5.f;
  if (!e) {
    memcpy(&lmm, f + 32, 4);
    n = rd32(f + 36);
    if (n_mels < 1 || dim < 1 || inter < 1 || layers < 0 || n_mels > 256 || dim > 1024 || inter > 4096 || layers > 32)
      e = "a model for another shape (newer kit?)";
  }
  if (!e) {
    const size_t want = (size_t)n_mels * kInBins + (size_t)dim * n_mels * kK + 3 * (size_t)dim +
                        (size_t)layers * ((size_t)dim * kK + 4 * (size_t)dim + 2 * (size_t)inter * dim + inter + dim) +
                        2 * (size_t)dim + (size_t)(kOutNfft + 2) * dim + (kOutNfft + 2);
    if (n != want) e = "the model's size doesn't match its shape";
    else if (len < (size_t)hb + 2 * (size_t)n) e = "the model file is cut short";
    else if (fnv1a(f + hb, 2 * (size_t)n) != rd32(f + 40)) e = "the model file is damaged";
  }
  if (!e && (!alloc((void**)&w_, sizeof(float) * n) ||
             !alloc((void**)&embed_w_, sizeof(float) * (size_t)dim * kK * n_mels) ||
             !alloc((void**)&blocks_, sizeof(Block) * (size_t)(layers ? layers : 1)))) {
    e = "not enough memory for the model";
  }
  if (e) {
    unload();
    if (err) *err = e;
    return false;
  }
  const uint8_t* p = f + hb;
  for (uint32_t i = 0; i < n; i++) w_[i] = half(rd16(p + 2 * i));
  n_mels_ = n_mels; dim_ = dim; inter_ = inter; layers_ = layers; log_mag_max_ = lmm;
  const float* q = w_;
  auto take = [&](size_t c) { const float* r = q; q += c; return r; };
  fb_ = take((size_t)n_mels * kInBins);
  const float* ew = take((size_t)dim * n_mels * kK);   // [dim][n_mels][k] -> [dim][k][n_mels]
  for (int d = 0; d < dim; d++)
    for (int c = 0; c < n_mels; c++)
      for (int j = 0; j < kK; j++) embed_w_[((size_t)d * kK + j) * n_mels + c] = ew[((size_t)d * n_mels + c) * kK + j];
  embed_b_ = take(dim);
  norm_w_ = take(dim);
  norm_b_ = take(dim);
  for (int l = 0; l < layers; l++) {
    Block& b = blocks_[l];
    b.dw_w = take((size_t)dim * kK);
    b.dw_b = take(dim);
    b.ln_w = take(dim);
    b.ln_b = take(dim);
    b.w1 = take((size_t)inter * dim);
    b.b1 = take(inter);
    b.w2 = take((size_t)dim * inter);
    b.b2 = take(dim);
    b.gamma = take(dim);
  }
  fln_w_ = take(dim);
  fln_b_ = take(dim);
  out_w_ = take((size_t)(kOutNfft + 2) * dim);
  out_b_ = take(kOutNfft + 2);
  return true;
}

bool VoxRestoreSmall::run(const float* x8, size_t n, Sink sink, void* ctx, volatile bool* stop) {
  if (!loaded() || !x8 || !n || !sink) return false;
  const int D = dim_, I = inter_, M = n_mels_;
  const size_t npad = (n + kInHop - 1) / kInHop * kInHop;
  const int T = (int)(npad / kInHop);
  const int p_in = (kInNfft - kInHop) / 2;   // 96
  const long q_out = (kOutNfft - kOutHop) / 2;   // 192

  float* mel = nullptr;
  float* x = nullptr;
  float* scr = nullptr;
  // Scratch: FFT (2 x 512), tile buffers d/orig [kTile][D], h1 [kTile][I], prev [3][D],
  // head [kTile][kOutNfft+2], window tables, OLA (kOutNfft), the output piece (kOutHop).
  const size_t scr_n = 2 * kOutNfft + 2 * (size_t)kTile * D + (size_t)kTile * I + 3 * (size_t)D +
                       (size_t)kTile * (kOutNfft + 2) + kInNfft + 2 * kOutNfft + kOutNfft + kOutHop;
  if (!alloc((void**)&mel, sizeof(float) * (size_t)T * M) || !alloc((void**)&x, sizeof(float) * (size_t)T * D) ||
      !alloc((void**)&scr, sizeof(float) * scr_n)) {
    if (mel) vrsFree(mel);
    if (x) vrsFree(x);
    if (scr) vrsFree(scr);
    return false;
  }
  float* re = scr;
  float* im = re + kOutNfft;
  float* tile_d = im + kOutNfft;
  float* tile_o = tile_d + (size_t)kTile * D;
  float* tile_h = tile_o + (size_t)kTile * D;
  float* prev = tile_h + (size_t)kTile * I;
  float* head = prev + 3 * (size_t)D;
  float* win_in = head + (size_t)kTile * (kOutNfft + 2);
  float* win_out = win_in + kInNfft;
  float* win2 = win_out + kOutNfft;
  float* ola = win2 + kOutNfft;
  float* piece = ola + kOutNfft;
  for (int i = 0; i < kInNfft; i++) win_in[i] = hannAt(i, kInNfft);
  for (int i = 0; i < kOutNfft; i++) { win_out[i] = hannAt(i, kOutNfft); win2[i] = win_out[i] * win_out[i]; }
  bool ok = true;

  // ---- log-mel of each frame
  auto xin = [&](long i) -> float {   // the input, zero-padded to npad, reflected at both ends
    long s = i - p_in;
    if (s < 0) s = -s;
    if (s >= (long)npad) s = 2 * ((long)npad - 1) - s;
    return s < (long)n ? x8[s] : 0.f;
  };
  for (int t = 0; t < T; t++) {
    if (yield_ && t % 64 == 63) yield_();
    for (int k = 0; k < kInNfft; k++) { re[k] = xin((long)t * kInHop + k) * win_in[k]; im[k] = 0; }
    voxrs::fft(re, im, kInNfft, false);
    for (int k = 0; k < kInBins; k++) re[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
    for (int m = 0; m < M; m++) {
      const float s = dot(fb_ + (size_t)m * kInBins, re, kInBins);
      mel[(size_t)t * M + m] = logf(s > 1e-5f ? s : 1e-5f);
    }
  }
  if (stop && *stop) ok = false;

  // ---- embed (conv 7 over time, zero-padded) and LayerNorm
  for (int t = 0; ok && t < T; t++) {
    if (yield_ && t % 16 == 15) yield_();
    float* row = x + (size_t)t * D;
    for (int d = 0; d < D; d++) {
      float s = embed_b_[d];
      for (int j = 0; j < kK; j++) {
        const int tt = t + j - kHalfK;
        if (tt >= 0 && tt < T) s += dot(embed_w_ + ((size_t)d * kK + j) * M, mel + (size_t)tt * M, M);
      }
      row[d] = s;
    }
    layerNorm(row, row, norm_w_, norm_b_, D);
  }
  vrsFree(mel);
  mel = nullptr;

  // ---- ConvNeXt blocks, in place, kTile frames at a time
  for (int l = 0; ok && l < layers_; l++) {
    const Block& B = blocks_[l];
    for (int t0 = 0; ok && t0 < T; t0 += kTile) {
      const int tn = T - t0 < kTile ? T - t0 : kTile;
      memcpy(tile_o, x + (size_t)t0 * D, sizeof(float) * (size_t)tn * D);   // this tile, as it was
      auto orig = [&](int t) -> const float* {   // row t before this block touched it; null past the ends
        if (t < 0 || t >= T) return nullptr;
        if (t < t0) return prev + (size_t)(t - (t0 - 3)) * D;
        if (t < t0 + tn) return tile_o + (size_t)(t - t0) * D;
        return x + (size_t)t * D;
      };
      for (int i = 0; i < tn; i++) {   // depthwise conv, then LayerNorm
        float* dd = tile_d + (size_t)i * D;
        for (int d = 0; d < D; d++) dd[d] = B.dw_b[d];
        for (int j = 0; j < kK; j++) {
          const float* src = orig(t0 + i + j - kHalfK);
          if (!src) continue;
          for (int d = 0; d < D; d++) dd[d] += B.dw_w[(size_t)d * kK + j] * src[d];
        }
        layerNorm(dd, dd, B.ln_w, B.ln_b, D);
      }
      for (int o = 0; o < I; o++) {   // pwconv1 + GELU
        const float* w = B.w1 + (size_t)o * D;
        for (int i = 0; i < tn; i++) tile_h[(size_t)i * I + o] = gelu(B.b1[o] + dot(w, tile_d + (size_t)i * D, D));
      }
      for (int o = 0; o < D; o++) {   // pwconv2, scaled, added back
        const float* w = B.w2 + (size_t)o * I;
        for (int i = 0; i < tn; i++)
          x[(size_t)(t0 + i) * D + o] = tile_o[(size_t)i * D + o] + B.gamma[o] * (B.b2[o] + dot(w, tile_h + (size_t)i * I, I));
      }
      // The last three rows as they were, for the next tile's look back
      // (gathered in tile_d, free now, as some may come from prev itself).
      for (int r = 0; r < 3; r++) {
        const float* src = orig(t0 + tn - 3 + r);
        if (src) memcpy(tile_d + (size_t)r * D, src, sizeof(float) * D);
        else memset(tile_d + (size_t)r * D, 0, sizeof(float) * D);
      }
      memcpy(prev, tile_d, sizeof(float) * 3 * (size_t)D);
      if (yield_) yield_();
      if (stop && *stop) ok = false;
    }
  }

  // ---- head, inverse STFT, overlap-add, out in order
  memset(ola, 0, sizeof(float) * kOutNfft);
  long out_pos = 0;               // index of ola[0] in the untrimmed output
  const long out_end = q_out + 2 * (long)n;
  auto envAt = [&](long i) -> float {   // sum of squared windows over the frames covering i
    float s = 0;
    for (long t = i / kOutHop; t >= 0 && t > i / kOutHop - kOutNfft / kOutHop; t--)
      if (t < T) s += win2[i - t * kOutHop];
    return s;
  };
  auto emit = [&](long count) -> bool {   // the first `count` samples of ola are done
    size_t m = 0;
    for (long k = 0; k < count; k++) {
      const long i = out_pos + k;
      if (i < q_out || i >= out_end) continue;
      const float e = envAt(i);
      piece[m++] = ola[k] / (e > 1e-8f ? e : 1e-8f);
      if (m == (size_t)kOutHop) { if (!sink(ctx, piece, m)) return false; m = 0; }
    }
    return m == 0 || sink(ctx, piece, m);
  };
  for (int t0 = 0; ok && t0 < T; t0 += kTile) {
    const int tn = T - t0 < kTile ? T - t0 : kTile;
    for (int i = 0; i < tn; i++) layerNorm(x + (size_t)(t0 + i) * D, tile_d + (size_t)i * D, fln_w_, fln_b_, D);
    for (int o = 0; o < kOutNfft + 2; o++) {
      const float* w = out_w_ + (size_t)o * D;
      for (int i = 0; i < tn; i++) head[(size_t)i * (kOutNfft + 2) + o] = out_b_[o] + dot(w, tile_d + (size_t)i * D, D);
    }
    for (int i = 0; ok && i < tn; i++) {
      const float* h = head + (size_t)i * (kOutNfft + 2);
      for (int k = 0; k < kOutBins; k++) {
        const float lm = h[k] < log_mag_max_ ? h[k] : log_mag_max_;
        const float mag = expf(lm);
        re[k] = mag * cosf(h[kOutBins + k]);
        im[k] = mag * sinf(h[kOutBins + k]);
      }
      im[0] = 0;                    // as irfft: the DC and Nyquist bins are real
      im[kOutBins - 1] = 0;
      for (int k = 1; k < kOutBins - 1; k++) { re[kOutNfft - k] = re[k]; im[kOutNfft - k] = -im[k]; }
      voxrs::fft(re, im, kOutNfft, true);
      for (int k = 0; k < kOutNfft; k++) ola[k] += re[k] * (1.0f / kOutNfft) * win_out[k];
      if (!emit(kOutHop)) { ok = false; break; }   // nothing later reaches these
      memmove(ola, ola + kOutHop, sizeof(float) * (kOutNfft - kOutHop));
      memset(ola + kOutNfft - kOutHop, 0, sizeof(float) * kOutHop);
      out_pos += kOutHop;
    }
    if (yield_) yield_();
    if (stop && *stop) ok = false;
  }
  if (ok && !emit(kOutNfft - kOutHop)) ok = false;   // the last frame's tail

  vrsFree(x);
  vrsFree(scr);
  return ok;
}
