// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which repeaters run MediaMesh (radio2), as last heard: what each repeater's
// NODE_DISCOVER_RESP said when the request asked for its radio2 profile
// (MediaMeshCore.h: DiscoverRadio2). Find MediaMesh, the coverage map, the
// Discover app, the Regions scan and a quiet ask every half hour all ask; the
// map marks the repeaters that answered with radio2 on. Radio2 off clears the
// mark. Kept 30 days (by the RTC; an entry heard before the clock was set is
// kept until it is heard again), saved to internal storage by the UI.
// MeshCore Open TAK does the same (lib/services/mediamesh_repeaters.dart).
//
// No Arduino here: test/test_mediamesh_reps.cpp runs it on the host.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "helpers/MediaMeshCore.h"

namespace mmreps {

static const int kMax = 96;                      // repeaters remembered
static const int kKeyLen = 6;                    // pubkey prefix kept
static const uint32_t kKeepSecs = 30u * 86400u;  // 30 days
static const uint32_t kValidEpoch = 1600000000u; // an RTC below this isn't set
static const int kRecLen = 14;                   // saved record
static const uint8_t kMagic[3] = {'M', 'R', '1'};

/** What a repeater last said about its radio2. */
struct Info {
  bool active;      // radio2 on
  uint8_t mode;     // 1 listens, 2 listens and sends (0 off)
  uint32_t bw_hz;
  uint8_t sf;
  uint32_t heard;   // RTC seconds
};

/** "250 kHz SF7", "62.5 kHz SF9" (empty when there's no profile). */
inline void describe(const Info& i, char* out, size_t cap) {
  if (!out || !cap) return;
  out[0] = '\0';
  if (!i.active || i.bw_hz == 0) return;
  const uint32_t whole = i.bw_hz / 1000, tenth = (i.bw_hz % 1000) / 100;
  if (tenth)
    snprintf(out, cap, i.sf ? "%lu.%lu kHz SF%u" : "%lu.%lu kHz", (unsigned long)whole, (unsigned long)tenth,
             (unsigned)i.sf);
  else
    snprintf(out, cap, i.sf ? "%lu kHz SF%u" : "%lu kHz", (unsigned long)whole, (unsigned)i.sf);
}

class Table {
 public:
  Table() : n_(0), ver_(0), dirty_(false) { memset(e_, 0, sizeof e_); }

  /** A DISCOVER_RESP trailer (what follows the key) from the repeater `pub`. */
  void noteDiscover(const uint8_t* pub, const uint8_t* r2, size_t len, uint32_t now) {
    mediamesh::DiscoverRadio2 p;
    if (!pub || !r2 || !len || !mediamesh::parseDiscoverRadio2(r2, len, p)) return;
    note(pub, p.active(), p.active() ? p.mode : 0, p.bw_hz, p.sf, now);
  }

  void note(const uint8_t* pub, bool active, uint8_t mode, uint32_t bw_hz, uint8_t sf, uint32_t now) {
    if (!pub) return;
    Entry* x = find(pub);
    if (!x) {
      if (n_ < kMax) {
        x = &e_[n_++];
      } else {   // full: an inactive entry goes first, then the longest unheard
        x = &e_[0];
        for (int i = 1; i < n_; i++) {
          const bool a = (e_[i].flags & 1) != 0, b = (x->flags & 1) != 0;
          if ((!a && b) || (a == b && e_[i].heard < x->heard)) x = &e_[i];
        }
      }
      memset(x, 0, sizeof *x);
      memcpy(x->key, pub, kKeyLen);
    }
    const uint8_t flags = (uint8_t)((active ? 1 : 0) | ((active ? mode & 3 : 0) << 1));
    const uint16_t bw = (uint16_t)(bw_hz / 100 > 0xFFFF ? 0xFFFF : bw_hz / 100);
    if (x->flags != flags || x->bw_100hz != bw || x->sf != sf) ++ver_;
    x->flags = flags;
    x->bw_100hz = bw;
    x->sf = sf;
    x->heard = now;
    dirty_ = true;
  }

  /** What `pub` last said, if not too long ago. */
  bool info(const uint8_t* pub, uint32_t now, Info* out) const {
    const Entry* x = find(pub);
    if (!x || expired(x->heard, now)) return false;
    if (out) {
      out->active = (x->flags & 1) != 0;
      out->mode = (uint8_t)((x->flags >> 1) & 3);
      out->bw_hz = (uint32_t)x->bw_100hz * 100u;
      out->sf = x->sf;
      out->heard = x->heard;
    }
    return true;
  }

  bool isMediaMesh(const uint8_t* pub, uint32_t now) const {
    Info i;
    return info(pub, now, &i) && i.active;
  }

  int count(uint32_t now) const {
    int c = 0;
    for (int i = 0; i < n_; i++)
      if ((e_[i].flags & 1) && !expired(e_[i].heard, now)) c++;
    return c;
  }

  /** Changes when a mark is added, changed or cleared (the map redraws). */
  uint32_t version() const { return ver_; }

  /** True once after a change worth saving. */
  bool takeDirty() {
    const bool d = dirty_;
    dirty_ = false;
    return d;
  }

  size_t saveSize() const { return 4 + (size_t)n_ * kRecLen; }

  /** The table as bytes: "MR1", a count, then the records. 0 if `cap` is short. */
  size_t save(uint8_t* buf, size_t cap) const {
    if (!buf || cap < saveSize()) return 0;
    memcpy(buf, kMagic, 3);
    buf[3] = (uint8_t)n_;
    uint8_t* p = buf + 4;
    for (int i = 0; i < n_; i++, p += kRecLen) {
      const Entry& x = e_[i];
      memcpy(p, x.key, kKeyLen);
      p[6] = x.flags;
      p[7] = x.sf;
      p[8] = (uint8_t)x.bw_100hz;
      p[9] = (uint8_t)(x.bw_100hz >> 8);
      for (int k = 0; k < 4; k++) p[10 + k] = (uint8_t)(x.heard >> (8 * k));
    }
    return saveSize();
  }

  /** Replaces the table with saved bytes, leaving out expired entries. */
  bool load(const uint8_t* buf, size_t len, uint32_t now) {
    if (!buf || len < 4 || memcmp(buf, kMagic, 3) != 0) return false;
    const int n = buf[3];
    if (n > kMax || len < 4 + (size_t)n * kRecLen) return false;
    n_ = 0;
    const uint8_t* p = buf + 4;
    for (int i = 0; i < n; i++, p += kRecLen) {
      Entry x;
      memcpy(x.key, p, kKeyLen);
      x.flags = p[6];
      x.sf = p[7];
      x.bw_100hz = (uint16_t)(p[8] | (p[9] << 8));
      x.heard = (uint32_t)p[10] | ((uint32_t)p[11] << 8) | ((uint32_t)p[12] << 16) | ((uint32_t)p[13] << 24);
      if (!expired(x.heard, now)) e_[n_++] = x;
    }
    ++ver_;
    return true;
  }

 private:
  struct Entry {
    uint8_t key[kKeyLen];
    uint8_t flags;      // bit 0 radio2 on, bits 1-2 mode
    uint8_t sf;
    uint16_t bw_100hz;
    uint32_t heard;
  };

  static bool expired(uint32_t heard, uint32_t now) {
    return now >= kValidEpoch && heard >= kValidEpoch && now > heard && now - heard > kKeepSecs;
  }

  Entry* find(const uint8_t* pub) {
    if (!pub) return nullptr;
    for (int i = 0; i < n_; i++)
      if (memcmp(e_[i].key, pub, kKeyLen) == 0) return &e_[i];
    return nullptr;
  }
  const Entry* find(const uint8_t* pub) const { return const_cast<Table*>(this)->find(pub); }

  Entry e_[kMax];
  int n_;
  uint32_t ver_;
  bool dirty_;
};

/** The radio's one table: the mesh notes answers, the UI reads and saves it. */
inline Table& table() {
  static Table t;
  return t;
}

}  // namespace mmreps
