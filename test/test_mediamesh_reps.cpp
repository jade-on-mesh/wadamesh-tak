// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which repeaters run MediaMesh (src/helpers/MediaMeshReps.h): noting
// DISCOVER_RESP trailers, clearing, expiry, eviction and save/load.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "helpers/MediaMeshReps.h"

using namespace mmreps;

static const uint32_t kNow = 1800000000u;

static void key(uint8_t* k, uint8_t a) {
  memset(k, 0, 32);
  k[0] = 0x20;
  k[1] = 0xDE;
  k[2] = a;
}

int main() {
  uint8_t r2[mediamesh::kDiscoverRadio2Len];
  mediamesh::DiscoverRadio2 p;
  p.configured = true;
  p.mode = 2;
  p.freq_khz = 916000;
  p.bw_hz = 250000;
  p.sf = 7;
  p.cr = 5;
  const size_t n = mediamesh::encodeDiscoverRadio2(p, r2);

  uint8_t a[32], b[32];
  key(a, 1);
  key(b, 2);

  {   // radio2 on marks it; off clears it
    Table t;
    assert(!t.isMediaMesh(a, kNow));
    const uint32_t v0 = t.version();
    t.noteDiscover(a, r2, n, kNow);
    assert(t.isMediaMesh(a, kNow));
    assert(!t.isMediaMesh(b, kNow));
    assert(t.version() != v0);
    Info i;
    assert(t.info(a, kNow, &i) && i.mode == 2 && i.bw_hz == 250000 && i.sf == 7);
    char d[24];
    describe(i, d, sizeof d);
    assert(strcmp(d, "250 kHz SF7") == 0);
    assert(t.count(kNow) == 1);

    // The same answer again: no change for the map.
    const uint32_t v1 = t.version();
    t.noteDiscover(a, r2, n, kNow + 60);
    assert(t.version() == v1);

    uint8_t off[2];
    mediamesh::DiscoverRadio2 o;
    assert(mediamesh::encodeDiscoverRadio2(o, off) == 2);
    t.noteDiscover(a, off, 2, kNow + 120);
    assert(!t.isMediaMesh(a, kNow + 120));
    assert(t.version() != v1);
    assert(t.count(kNow) == 0);

    // No trailer (stock firmware): nothing noted.
    t.noteDiscover(b, nullptr, 0, kNow);
    assert(!t.info(b, kNow, nullptr));
  }

  {   // 62.5 kHz listen-only
    Table t;
    t.note(a, true, 1, 62500, 9, kNow);
    Info i;
    assert(t.info(a, kNow, &i) && i.mode == 1);
    char d[24];
    describe(i, d, sizeof d);
    assert(strcmp(d, "62.5 kHz SF9") == 0);
  }

  {   // 30 days, by the RTC; before the clock was set it is kept
    Table t;
    t.noteDiscover(a, r2, n, kNow);
    assert(t.isMediaMesh(a, kNow + kKeepSecs));
    assert(!t.isMediaMesh(a, kNow + kKeepSecs + 1));
    t.noteDiscover(b, r2, n, 1000);   // clock not set yet
    assert(t.isMediaMesh(b, kNow + 10 * kKeepSecs));
  }

  {   // full: an inactive one goes first, then the longest unheard
    Table t;
    uint8_t k[32];
    for (int i = 0; i < kMax; i++) {
      key(k, (uint8_t)i);
      k[3] = (uint8_t)(i >> 8);
      t.note(k, i != 5, 2, 250000, 7, kNow + (uint32_t)i);
    }
    uint8_t extra[32];
    key(extra, 0);
    extra[3] = 0x77;
    t.note(extra, true, 2, 250000, 7, kNow + 500);
    key(k, 5);
    assert(!t.info(k, kNow + 500, nullptr));   // the inactive one went
    key(k, 0);
    assert(t.isMediaMesh(k, kNow + 500));
    uint8_t extra2[32];
    key(extra2, 1);
    extra2[3] = 0x78;
    t.note(extra2, true, 2, 250000, 7, kNow + 501);
    assert(!t.info(k, kNow + 501, nullptr));   // then the oldest
    assert(t.isMediaMesh(extra, kNow + 501) && t.isMediaMesh(extra2, kNow + 501));
  }

  {   // save and load, leaving out expired entries
    Table t;
    t.noteDiscover(a, r2, n, kNow);
    t.note(b, true, 1, 125000, 8, kNow - kKeepSecs - 10);
    assert(t.takeDirty() && !t.takeDirty());
    uint8_t buf[4 + kMax * kRecLen];
    const size_t len = t.save(buf, sizeof buf);
    assert(len == 4 + 2 * (size_t)kRecLen);
    assert(t.save(buf, 10) == 0);
    Table u;
    assert(u.load(buf, len, kNow));
    assert(u.isMediaMesh(a, kNow));
    assert(!u.info(b, kNow, nullptr));
    Info i;
    assert(u.info(a, kNow, &i) && i.bw_hz == 250000 && i.sf == 7 && i.heard == kNow);
    assert(!u.load(buf, 3, kNow));
    buf[0] = 'X';
    assert(!u.load(buf, len, kNow));
  }

  printf("mediamesh reps: ok\n");
  return 0;
}
