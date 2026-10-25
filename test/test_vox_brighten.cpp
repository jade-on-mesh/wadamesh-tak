// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoxBrighten ("Brighter voice") against its reference,
// scripts/voice/brighten_ref.py: test/data/vox/brighten_in.raw is 1.5 s of
// Codec2 1200 speech as VoxPlayOut plays it at 16 kHz, brighten_out.raw what
// the reference makes of it (`brighten_ref.py golden`). Plus streaming in odd
// chunk sizes, silence, and other output rates.
// Built a second time with -I test/data/vox/net, it tests the trained-net path
// instead: test/data/vox/net/VoxBrightNet.h is a fixture in the format
// MeshCore Open TAK's tools/voice-training writes, brighten_net_out.raw what
// that kit's `python -m kit.golden` makes of the input with it.
// Build: g++ -std=c++17 -I src [-I test/data/vox/net] test/test_vox_brighten.cpp src/ui-touch/VoxBrighten.cpp
// Run:   test_vox_brighten test/data/vox

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "ui-touch/VoxBrighten.h"

static int failures = 0;
#define CHECK(c, ...)                                   \
  do {                                                  \
    if (!(c)) {                                         \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
      printf(__VA_ARGS__);                              \
      printf("\n");                                     \
      failures++;                                       \
    }                                                   \
  } while (0)

static std::vector<int16_t> readRaw(const std::string& path) {
  std::vector<int16_t> v;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return v;
  int16_t buf[1024];
  size_t n;
  while ((n = fread(buf, 2, 1024, f)) > 0) v.insert(v.end(), buf, buf + n);
  fclose(f);
  return v;
}

// Whole clip through, in chunks of `chunk`, then flushed. Output is a frame
// late, so drop the first frame to line up with the input.
static std::vector<int16_t> brighten(const std::vector<int16_t>& in, uint32_t rate, size_t chunk) {
  VoxBrighten b;
  if (!b.begin(rate)) return {};
  std::vector<int16_t> out(in.size() + b.frameSamples());
  size_t i = 0;
  while (i < in.size()) {
    const size_t n = in.size() - i < chunk ? in.size() - i : chunk;
    b.process(&in[i], &out[i], n);
    i += n;
  }
  b.flush(&out[i], out.size() - i);
  out.erase(out.begin(), out.begin() + b.frameSamples());
  return out;
}

// Energy above 6 kHz (VoxPlayOut's gentle 3.6 kHz low-pass leaves some 4-5 kHz
// in already): four RBJ high-pass biquads (8th order) at `rate`.
static double energyAbove6k(const std::vector<int16_t>& x, uint32_t rate = 16000) {
  const double w0 = 2 * M_PI * 6000.0 / rate, c = cos(w0), al = sin(w0) / (2 * 0.70710678);
  const double a0 = 1 + al, b0 = (1 + c) / 2 / a0, b1 = -(1 + c) / a0, a1 = -2 * c / a0, a2 = (1 - al) / a0;
  double st[4][4] = {};
  double e = 0;
  for (int16_t s : x) {
    double v = s;
    for (auto& q : st) {
      const double y = b0 * v + b1 * q[0] + b0 * q[1] - a1 * q[2] - a2 * q[3];
      q[1] = q[0]; q[0] = v; q[3] = q[2]; q[2] = y;
      v = y;
    }
    e += v * v;
  }
  return e;
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "test/data/vox";
  const auto in = readRaw(dir + "/brighten_in.raw");
  const auto want = readRaw(dir + (VoxBrighten::kUsesNet ? "/brighten_net_out.raw" : "/brighten_out.raw"));
  printf("levels from the %s\n", VoxBrighten::kUsesNet ? "trained net (VoxBrightNet.h)" : "rule");
  CHECK(!in.empty() && in.size() == want.size(), "golden files missing (%zu, %zu)", in.size(), want.size());

  // Against the reference: float here, double there, so close, not exact.
  const size_t whole = in.size() / 160 * 160;   // the reference keeps whole frames
  for (size_t chunk : {(size_t)1, (size_t)37, (size_t)160, (size_t)4096}) {
    const auto got = brighten(in, 16000, chunk);
    CHECK(got.size() == in.size(), "chunk %zu: %zu samples out", chunk, got.size());
    double se = 0, sd = 0;
    int worst = 0;
    for (size_t i = 0; i < whole && i < got.size(); i++) {
      const int d = abs((int)got[i] - (int)want[i]);
      if (d > worst) worst = d;
      sd += (double)d * d;
      se += (double)want[i] * want[i];
    }
    const double snr = 10 * log10(se / (sd + 1e-9));
    printf("chunk %4zu: %.1f dB from the reference, worst sample %d\n", chunk, snr, worst);
    CHECK(snr > 50, "chunk %zu: %.1f dB", chunk, snr);
    CHECK(worst < 200, "chunk %zu: worst %d", chunk, worst);
  }

  // It adds treble above 6 kHz (VoxPlayOut already leaves some there: mirror
  // images of the voice its gentle low-pass lets through).
  {
    const auto got = brighten(in, 16000, 160);
    const double before = energyAbove6k(in), after = energyAbove6k(got);
    const double db = 10 * log10(after / (before + 1e-9));
    printf("above 6 kHz: +%.1f dB\n", db);
    CHECK(db > 5, "treble +%.1f dB", db);
  }

  // Silence stays silent.
  {
    std::vector<int16_t> z(16000, 0);
    const auto got = brighten(z, 16000, 160);
    bool all0 = true;
    for (int16_t s : got) all0 &= s == 0;
    CHECK(all0, "silence made sound");
  }

  // Other rates work and keep the voice; under 16 kHz it declines.
  {
    VoxBrighten b;
    CHECK(!b.begin(8000), "8 kHz accepted");
    CHECK(!b.active(), "active after a refused begin");
    for (uint32_t rate : {22050u, 44100u, 48000u}) {
      std::vector<int16_t> tone(rate);
      for (size_t i = 0; i < tone.size(); i++)
        tone[i] = (int16_t)(8000 * sin(2 * M_PI * 3000.0 * i / rate) + 3000 * sin(2 * M_PI * 400.0 * i / rate));
      const auto got = brighten(tone, rate, 512);
      CHECK(got.size() == tone.size(), "%u Hz: size", rate);
      CHECK(energyAbove6k(got, rate) > 100 * energyAbove6k(tone, rate), "%u Hz: no treble added", rate);
    }
  }

  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("VoxBrighten: all good\n");
  return 0;
}
