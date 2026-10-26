// SPDX-License-Identifier: GPL-3.0-or-later
// VoxRestoreSmall against MeshCore Open TAK's kit/small.py numpy reference: a
// tiny random model (test/data/restore_small/tiny.bin) run on a buzzy voice-like
// signal, 3000 samples (several tiles) and 100 (fewer frames than a tile), must
// give the reference's output (out3000.f32, out100.f32, float64 there).
//   g++ -std=c++17 -I src test/test_restore_small.cpp src/ui-touch/VoxRestoreSmall.cpp -lm
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "ui-touch/VoxRestoreSmall.h"

static std::vector<uint8_t> readFile(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) { fprintf(stderr, "missing %s\n", p.c_str()); exit(1); }
  std::vector<uint8_t> b;
  uint8_t buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
  fclose(f);
  return b;
}

static std::vector<float> floats(const std::vector<uint8_t>& b) {
  std::vector<float> v(b.size() / 4);
  memcpy(v.data(), b.data(), v.size() * 4);
  return v;
}

struct Collect {
  std::vector<float> y;
  size_t stop_after = 0;
};
static bool sink(void* ctx, const float* y, size_t n) {
  Collect* c = static_cast<Collect*>(ctx);
  c->y.insert(c->y.end(), y, y + n);
  return !c->stop_after || c->y.size() < c->stop_after;
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "test/data/restore_small";
  // FFT round trip and a known bin.
  {
    float re[16], im[16];
    for (int i = 0; i < 16; i++) { re[i] = cosf(2 * 3.14159265f * 3 * i / 16); im[i] = 0; }
    voxrs::fft(re, im, 16, false);
    assert(fabsf(re[3] - 8) < 1e-4f && fabsf(re[13] - 8) < 1e-4f && fabsf(re[2]) < 1e-4f);
    voxrs::fft(re, im, 16, true);
    for (int i = 0; i < 16; i++) assert(fabsf(re[i] / 16 - cosf(2 * 3.14159265f * 3 * i / 16)) < 1e-5f);
  }

  const std::vector<uint8_t> model = readFile(dir + "/tiny.bin");
  VoxRestoreSmall rs;
  const char* err = nullptr;
  assert(rs.load(model.data(), model.size(), &err));
  assert(rs.dim() == 16 && rs.layers() == 2 && rs.macsPerSecond() > 0);

  const std::vector<float> x = floats(readFile(dir + "/in.f32"));
  for (size_t n : {(size_t)3000, (size_t)100}) {
    const std::vector<float> want = floats(readFile(dir + "/out" + std::to_string(n) + ".f32"));
    Collect c;
    assert(rs.run(x.data(), n, sink, &c));
    assert(c.y.size() == 2 * n && want.size() == 2 * n);
    float peak = 0, worst = 0;
    for (size_t i = 0; i < want.size(); i++) {
      peak = fmaxf(peak, fabsf(want[i]));
      worst = fmaxf(worst, fabsf(want[i] - c.y[i]));
    }
    printf("restore_small: %zu samples, peak %.4f, worst error %.2e\n", n, peak, worst);
    assert(peak > 1e-3f && worst <= 1e-3f * peak);
  }

  // Stopping: by the sink, and by the flag.
  {
    Collect c;
    c.stop_after = 256;
    assert(!rs.run(x.data(), 3000, sink, &c));
    volatile bool stop = true;
    Collect d;
    assert(!rs.run(x.data(), 3000, sink, &d, &stop));
  }

  // Damaged and foreign files are refused, and leave nothing loaded.
  {
    std::vector<uint8_t> bad = model;
    bad[bad.size() / 2] ^= 0x40;
    VoxRestoreSmall r2;
    assert(!r2.load(bad.data(), bad.size(), &err) && !r2.loaded() && strstr(err, "damaged"));
    assert(!r2.load(model.data(), model.size() - 10, &err) && strstr(err, "cut short"));
    bad = model;
    bad[0] = 'X';
    assert(!r2.load(bad.data(), bad.size(), &err) && strstr(err, "not a small"));
    assert(!r2.load(nullptr, 0, &err));
  }
  puts("restore_small: all passed");
  return 0;
}
