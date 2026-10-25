// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoxFec against the shared vectors (test/data/vox/fec_vectors.txt, made by
// MeshCore Open TAK's scripts/vox/fec_ref.py; the app's test reads the same
// file): parity byte for byte, and every loss within a block's parity count
// rebuilt exactly.
// Build: g++ -std=c++17 -I src test/test_vox_fec.cpp
// Run:   test_vox_fec test/data/vox

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "ui-touch/VoxFec.h"

static int failures = 0;
#define CHECK(c, ...)                              \
  do {                                             \
    if (!(c)) {                                    \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      printf(__VA_ARGS__);                         \
      printf("\n");                                \
      failures++;                                  \
    }                                              \
  } while (0)

struct Case {
  uint32_t sid;
  int n, size, last;
  bool image;
  std::vector<std::string> parity;
};

static std::vector<uint8_t> lcg(uint64_t seed, int n) {
  std::vector<uint8_t> out(n);
  uint64_t s = seed;
  for (int i = 0; i < n; i++) {
    s = (s * 1103515245ull + 12345ull) & 0x7FFFFFFFull;
    out[i] = (uint8_t)((s >> 16) & 0xFF);
  }
  return out;
}

static std::string hex(const uint8_t* b, int n) {
  std::string s;
  char t[3];
  for (int i = 0; i < n; i++) { snprintf(t, sizeof t, "%02x", b[i]); s += t; }
  return s;
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "test/data/vox";
  FILE* f = fopen((dir + "/fec_vectors.txt").c_str(), "r");
  CHECK(f, "no vectors");
  if (!f) return 1;
  std::vector<Case> cases;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    if (!strncmp(line, "case", 4)) {
      Case c{};
      unsigned sid; int idx, img;
      sscanf(line, "case %d sid %x packets %d size %d last %d image %d", &idx, &sid, &c.n, &c.size, &c.last, &img);
      c.sid = sid;
      c.image = img != 0;
      cases.push_back(c);
    } else {
      int idx;
      char h[512];
      sscanf(line, "%d %511s", &idx, h);
      cases[idx].parity.push_back(h);
    }
  }
  fclose(f);
  CHECK(cases.size() == 3, "%zu cases", cases.size());

  std::mt19937 rng(7);
  for (const Case& c : cases) {
    // The clip, contiguous as VoxWire::Assembly holds it.
    std::vector<uint8_t> buf((size_t)c.n * c.size, 0);
    std::vector<std::vector<uint8_t>> pk;
    for (int i = 0; i < c.n; i++) {
      pk.push_back(lcg((uint64_t)c.sid + i, i < c.n - 1 ? c.size : c.last));
      memcpy(&buf[(size_t)i * c.size], pk[i].data(), pk[i].size());
    }
    // Parity, byte for byte.
    const int np = VoxFec::totalParity(c.n, c.image);
    CHECK(np == (int)c.parity.size(), "%08x: %d parity, vectors have %zu", c.sid, np, c.parity.size());
    std::vector<std::vector<uint8_t>> par;
    for (int k = 0; k < np; k++) {
      int b, j;
      CHECK(VoxFec::parityAt(c.n, c.image, k, &b, &j), "parityAt %d", k);
      uint8_t out[VoxFec::kMaxParityPacket];
      const int len = VoxFec::encodeParity(c.sid, buf.data(), c.n, c.size, c.last, b, j, out, sizeof out);
      CHECK(len > 0 && len <= 163, "len %d", len);
      CHECK(k < (int)c.parity.size() && hex(out, len) == c.parity[k], "%08x parity %d differs", c.sid, k);
      par.emplace_back(out, out + len);
    }
    // Rebuild every loss pattern within the parity counts.
    std::vector<VoxFec::Parity> pv;
    for (auto& p : par) pv.push_back(VoxFec::Parity{p[5], (uint8_t)(p.size() - 6), p.data() + 6});
    for (int trial = 0; trial < 300; trial++) {
      std::set<int> lost;
      for (int lo = 0; lo < c.n; lo += VoxFec::kBlock) {
        const int hi = lo + VoxFec::kBlock < c.n ? lo + VoxFec::kBlock : c.n;
        const int m = VoxFec::parityCount(hi - lo, c.image);
        std::vector<int> pool;
        for (int i = lo; i < hi; i++) pool.push_back(i);
        std::shuffle(pool.begin(), pool.end(), rng);
        const int k = (int)(rng() % (unsigned)(m + 1));
        for (int i = 0; i < k; i++) lost.insert(pool[i]);
      }
      std::vector<uint8_t> work = buf;
      for (int i : lost) memset(&work[(size_t)i * c.size], 0xEE, c.size);   // garbage where lost
      std::set<int> got;
      bool exact = true;
      VoxFec::rebuild(c.n, c.size, work.data(), [&](int i) { return !lost.count(i); }, pv.data(), (int)pv.size(),
                      [&](int i, const uint8_t* d, int len) {
                        got.insert(i);
                        if (len != (int)pk[i].size() || memcmp(d, pk[i].data(), (size_t)len)) exact = false;
                      });
      CHECK(got == lost && exact, "%08x trial %d: rebuilt %zu of %zu lost, exact %d", c.sid, trial, got.size(),
            lost.size(), (int)exact);
    }
    printf("%08x: %d packets, %d parity, ok\n", c.sid, c.n, np);
  }

  // More lost in a block than its parity: nothing made up.
  {
    const Case& c = cases[0];   // 11 packets, 3 parity
    std::vector<uint8_t> buf((size_t)c.n * c.size, 0);
    for (int i = 0; i < c.n; i++) {
      auto d = lcg((uint64_t)c.sid + i, i < c.n - 1 ? c.size : c.last);
      memcpy(&buf[(size_t)i * c.size], d.data(), d.size());
    }
    std::vector<std::vector<uint8_t>> par;
    std::vector<VoxFec::Parity> pv;
    for (int k = 0; k < VoxFec::totalParity(c.n, false); k++) {
      int b, j;
      VoxFec::parityAt(c.n, false, k, &b, &j);
      uint8_t out[VoxFec::kMaxParityPacket];
      const int len = VoxFec::encodeParity(c.sid, buf.data(), c.n, c.size, c.last, b, j, out, sizeof out);
      par.emplace_back(out, out + len);
    }
    for (auto& p : par) pv.push_back(VoxFec::Parity{p[5], (uint8_t)(p.size() - 6), p.data() + 6});
    const int n = VoxFec::rebuild(c.n, c.size, buf.data(), [](int i) { return i >= 4; }, pv.data(), (int)pv.size(),
                                  [](int, const uint8_t*, int) {});
    CHECK(n == 0, "rebuilt %d with 4 lost and 3 parity", n);
  }
  CHECK(VoxFec::totalParity(230, true) == 24, "230-packet photo");
  CHECK(VoxFec::paritySid(VoxFec::paritySid(0x12345678)) == 0x12345678, "xor");

  if (failures) { printf("%d failure(s)\n", failures); return 1; }
  printf("VoxFec: all good\n");
  return 0;
}
