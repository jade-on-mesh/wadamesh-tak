// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Forward error correction for MeshCore Open TAK photos and voice, as the app
// does it (lib/vox/vox_fec.dart in jade-on-mesh/meshcore-open-tak; its
// scripts/vox/fec_ref.py is the reference both are tested against, with the
// shared vectors in test/data/vox/fec_vectors.txt).
//
// A whole clip sent at once (a push, or a request for all of it) is followed
// by parity packets, Reed-Solomon over GF(256) in blocks of 20 data packets:
// a listener that lost up to that many packets of a block rebuilds them
// itself, with no request.
//
// A parity packet is an ordinary VE3 voice packet
//     [0x56][psid:4][info][lastLen][parity: P bytes]
// - psid = the clip's session id XOR 0xFEC0FEC0. Radios and apps without FEC
//   see an unknown clip and drop it; companions route it as voice (radio2).
// - info = block * 8 + j: parity j of block `block` (packets [20b, 20b + 20)).
// - lastLen = the length of the clip's last packet (the only short one).
// - P = the clip's full packet length; data packets are zero-padded to it.
// Parity j = sum_i c(j, i) * d_i with c(j, i) = 1 / ((0x80 | j) ^ i): a Cauchy
// matrix, so any e lost packets of a block come back from any e of its parity.
//
// Header-only and platform-neutral; host-tested (test/test_vox_fec.cpp).

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace VoxFec {

constexpr int kBlock = 20;
constexpr uint32_t kSidXor = 0xFEC0FEC0u;
constexpr int kMaxParityPerBlock = 8;
constexpr int kMaxP = 156;                       // VoxWire::kBytesPerPacket
constexpr int kParityHeader = 7;                 // magic, psid, info, lastLen
constexpr int kMaxParityPacket = kParityHeader + kMaxP;   // 163: fits GRP_DATA

inline uint32_t paritySid(uint32_t sid) { return sid ^ kSidXor; }

// Parity packets for a block of k data packets (voice gets more: a clip is
// short and every packet is a second of speech; photos about 10%).
inline int parityCount(int k, bool image) {
  if (k <= 1) return 0;
  if (image) return k >= 6 ? 2 : 1;
  return k >= 8 ? 3 : (k >= 4 ? 2 : 1);
}

inline int totalParity(int total, bool image) {
  int n = 0;
  for (int lo = 0; lo < total; lo += kBlock) n += parityCount(total - lo < kBlock ? total - lo : kBlock, image);
  return n;
}

// The block and parity number of the k-th parity packet of a clip, in the
// order they're sent (block by block). False past the last.
inline bool parityAt(int total, bool image, int k, int* block, int* j) {
  for (int b = 0, lo = 0; lo < total; b++, lo += kBlock) {
    const int m = parityCount(total - lo < kBlock ? total - lo : kBlock, image);
    if (k < m) { *block = b; *j = k; return true; }
    k -= m;
  }
  return false;
}

// ---- GF(256), polynomial 0x11d --------------------------------------------------

struct Gf {
  uint8_t exp[512];
  uint8_t log[256];
  Gf() {
    int x = 1;
    for (int i = 0; i < 255; i++) {
      exp[i] = (uint8_t)x;
      log[x] = (uint8_t)i;
      x <<= 1;
      if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
    log[0] = 0;
  }
  uint8_t mul(uint8_t a, uint8_t b) const { return a && b ? exp[log[a] + log[b]] : 0; }
  uint8_t inv(uint8_t a) const { return exp[255 - log[a]]; }
  uint8_t coef(int j, int i) const { return inv((uint8_t)((0x80 | j) ^ i)); }
  // acc ^= c * src (src shorter than n: zero padding).
  void addScaled(uint8_t* acc, int n, uint8_t c, const uint8_t* src, int src_len) const {
    if (!c) return;
    const int lc = log[c];
    const int m = src_len < n ? src_len : n;
    for (int t = 0; t < m; t++)
      if (src[t]) acc[t] ^= exp[lc + log[src[t]]];
  }
};

inline const Gf& gf() {
  static const Gf g;
  return g;
}

// Writes parity packet `j` of block `block` into `out`: the clip has `total`
// packets of `p` bytes each (the last `last_len`), packet i at data + i * p.
// Returns its length, or 0 if `cap` is too small or the arguments are off.
inline int encodeParity(uint32_t sid, const uint8_t* data, int total, int p, int last_len, int block, int j,
                        uint8_t* out, size_t cap) {
  const int lo = block * kBlock;
  if (p < 1 || p > kMaxP || lo >= total || j < 0 || j >= kMaxParityPerBlock) return 0;
  if (cap < (size_t)(kParityHeader + p)) return 0;
  const int hi = lo + kBlock < total ? lo + kBlock : total;
  const uint32_t ps = paritySid(sid);
  out[0] = 0x56;
  out[1] = (uint8_t)(ps >> 24); out[2] = (uint8_t)(ps >> 16); out[3] = (uint8_t)(ps >> 8); out[4] = (uint8_t)ps;
  out[5] = (uint8_t)(block * 8 + j);
  out[6] = (uint8_t)last_len;
  uint8_t* par = out + kParityHeader;
  memset(par, 0, (size_t)p);
  const Gf& g = gf();
  for (int i = lo; i < hi; i++) {
    const int len = i == total - 1 ? last_len : p;
    g.addScaled(par, p, g.coef(j, i - lo), data + (size_t)i * (size_t)p, len);
  }
  return kParityHeader + p;
}

// One parity packet heard: its info byte and codec data ([lastLen][P bytes]).
struct Parity {
  uint8_t info;
  uint8_t len;   // 1 + P
  const uint8_t* data;
};

// Rebuilds what the parity can of a `total`-packet clip held at `buf` (packet
// i at buf + i * p, `have(i)` saying which are here). For each packet that
// comes back, calls put(index, bytes, len). Returns how many.
template <class Have, class Put>
int rebuild(int total, int p, const uint8_t* buf, Have have, const Parity* par, int n_par, Put put) {
  if (p < 1 || p > kMaxP || n_par <= 0) return 0;
  const Gf& g = gf();
  int rebuilt = 0;
  for (int b = 0; b * kBlock < total; b++) {
    const int lo = b * kBlock, hi = lo + kBlock < total ? lo + kBlock : total;
    int miss[kMaxParityPerBlock];
    int e = 0;
    bool too_many = false;
    for (int i = lo; i < hi; i++) {
      if (have(i)) continue;
      if (e == kMaxParityPerBlock) { too_many = true; break; }
      miss[e++] = i;
    }
    if (!e || too_many) continue;
    // The block's parity, lowest j first.
    const Parity* use[kMaxParityPerBlock];
    int u = 0;
    for (int j = 0; j < kMaxParityPerBlock && u < e; j++)
      for (int k = 0; k < n_par; k++)
        if (par[k].info == b * 8 + j && par[k].len == 1 + p) { use[u++] = &par[k]; break; }
    if (u < e) continue;
    const int last_len = use[0]->data[0];
    // rhs_r = parity_r minus what the packets here put in it. On the heap
    // (PSRAM on the boards that have it), not the caller's stack.
    uint8_t* rhs_mem = (uint8_t*)malloc((size_t)(e + 1) * (size_t)p);
    if (!rhs_mem) return rebuilt;
    uint8_t* rhs[kMaxParityPerBlock];
    for (int r = 0; r < e; r++) rhs[r] = rhs_mem + (size_t)r * (size_t)p;
    uint8_t* scratch = rhs_mem + (size_t)e * (size_t)p;
    uint8_t a[kMaxParityPerBlock][kMaxParityPerBlock];
    for (int r = 0; r < e; r++) {
      const int j = use[r]->info & 7;
      memcpy(rhs[r], use[r]->data + 1, (size_t)p);
      for (int i = lo; i < hi; i++)
        if (have(i)) g.addScaled(rhs[r], p, g.coef(j, i - lo), buf + (size_t)i * (size_t)p, i == total - 1 ? last_len : p);
      for (int c = 0; c < e; c++) a[r][c] = g.coef(j, miss[c] - lo);
    }
    // Gauss-Jordan (every square part of a Cauchy matrix is invertible).
    bool ok = true;
    for (int c = 0; c < e && ok; c++) {
      int piv = -1;
      for (int r = c; r < e; r++) if (a[r][c]) { piv = r; break; }
      if (piv < 0) { ok = false; break; }
      if (piv != c) {
        for (int k = 0; k < e; k++) { const uint8_t x = a[c][k]; a[c][k] = a[piv][k]; a[piv][k] = x; }
        uint8_t* t = rhs[c]; rhs[c] = rhs[piv]; rhs[piv] = t;
      }
      const uint8_t iv = g.inv(a[c][c]);
      for (int k = 0; k < e; k++) a[c][k] = g.mul(a[c][k], iv);
      memset(scratch, 0, (size_t)p);
      g.addScaled(scratch, p, iv, rhs[c], p);
      memcpy(rhs[c], scratch, (size_t)p);
      for (int r = 0; r < e; r++) {
        if (r == c || !a[r][c]) continue;
        const uint8_t f = a[r][c];
        for (int k = 0; k < e; k++) a[r][k] ^= g.mul(f, a[c][k]);
        g.addScaled(rhs[r], p, f, rhs[c], p);
      }
    }
    if (ok) {
      for (int k = 0; k < e; k++) {
        const int i = miss[k];
        put(i, rhs[k], i == total - 1 && last_len >= 1 && last_len <= p ? last_len : p);
        rebuilt++;
      }
    }
    free(rhs_mem);
  }
  return rebuilt;
}

}  // namespace VoxFec
