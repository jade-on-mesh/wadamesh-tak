// SPDX-License-Identifier: GPL-3.0-or-later
// Watch a node: say when a watched contact goes quiet, and when it is back.
//
// "Heard" is the contact's lastmod, which the mesh sets to our clock whenever a
// packet from that node reaches us (its advert, a message, a path or a reply).
// The watcher never compares lastmod with the clock after the first look: it only
// notices that lastmod CHANGED, and times the gaps with millis(). A clock that
// jumps when it syncs therefore can't make a node look quiet or back.
//
// Saved as one NVS blob: [version][quiet hours][count x 6-byte key prefix].
// Header-only so the host test (test/test_node_watch.cpp) runs the same code.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace nodewatch {

constexpr int      kMax           = 16;
constexpr int      kKeyLen        = 6;
constexpr uint8_t  kBlobVersion   = 1;
constexpr uint8_t  kMinQuietHours = 1;
constexpr uint8_t  kMaxQuietHours = 72;
// Repeaters flood an advert every 12 hours by default, and one that is a few hops
// away may be heard by nothing else. An hour past that keeps a healthy repeater
// from being called quiet.
constexpr uint8_t  kDefaultQuietHours = 13;
// Older than this at the first look counts as "unknown": the gap is timed from then.
constexpr uint32_t kMaxBaselineAgeS = 30u * 24u * 3600u;
constexpr size_t   kBlobMax = 2 + (size_t)kMax * kKeyLen;
constexpr const char* kBlobKey = "ndwatch";

struct Entry {
  uint8_t  key[kKeyLen];
  uint32_t lastmod;    // the contact's lastmod at the last look
  uint32_t heard_ms;   // millis() when it was last heard (may be "before boot", wrapping)
  bool     seen;       // false until the first look sets the baseline
  bool     quiet;      // "went quiet" was said (or it was already quiet at the first look)
};

// Plain data, so it can live zero-filled in PSRAM; unpack() (or reset()) sets it up.
struct List {
  uint8_t quiet_hours;
  int     n;
  Entry   e[kMax];

  void reset() { n = 0; quiet_hours = kDefaultQuietHours; }

  int find(const uint8_t* key) const {
    for (int i = 0; i < n; ++i)
      if (memcmp(e[i].key, key, kKeyLen) == 0) return i;
    return -1;
  }
  bool has(const uint8_t* key) const { return find(key) >= 0; }

  // Adds or removes [key]. Returns whether it is watched afterwards; a full list
  // refuses an add (and *full is set so the caller can say so).
  bool toggle(const uint8_t* key, bool* full = nullptr) {
    if (full) *full = false;
    const int i = find(key);
    if (i >= 0) {
      for (int j = i; j + 1 < n; ++j) e[j] = e[j + 1];
      --n;
      return false;
    }
    if (n >= kMax) { if (full) *full = true; return false; }
    Entry& x = e[n++];
    memset(&x, 0, sizeof x);
    memcpy(x.key, key, kKeyLen);
    return true;
  }

  uint32_t quietMs() const { return (uint32_t)quiet_hours * 3600u * 1000u; }

  size_t pack(uint8_t* out, size_t cap) const {
    const size_t need = 2 + (size_t)n * kKeyLen;
    if (cap < need) return 0;
    out[0] = kBlobVersion;
    out[1] = quiet_hours;
    for (int i = 0; i < n; ++i) memcpy(out + 2 + (size_t)i * kKeyLen, e[i].key, kKeyLen);
    return need;
  }

  // Reads a saved blob; anything malformed leaves an empty list with the default
  // threshold. The run-time state (baseline, quiet) starts fresh either way.
  bool unpack(const uint8_t* in, size_t len) {
    reset();
    if (!in || len < 2 || in[0] != kBlobVersion || (len - 2) % kKeyLen != 0) return false;
    quiet_hours = clampHours(in[1]);
    const int cnt = (int)((len - 2) / kKeyLen);
    for (int i = 0; i < cnt && n < kMax; ++i) {
      Entry& x = e[n++];
      memset(&x, 0, sizeof x);
      memcpy(x.key, in + 2 + (size_t)i * kKeyLen, kKeyLen);
    }
    return true;
  }

  static uint8_t clampHours(int h) {
    if (h < kMinQuietHours) return kMinQuietHours;
    if (h > kMaxQuietHours) return kMaxQuietHours;
    return (uint8_t)h;
  }
};

enum class Event : uint8_t { None, WentQuiet, BackOnAir };

struct Result {
  Event    ev;
  uint32_t gap_s;   // WentQuiet: since last heard. BackOnAir: how long it was quiet.
};

// One look at a watched contact.
//   lastmod     the contact's lastmod now; 0 when it isn't in Contacts (no event)
//   now_ms      millis()
//   age_s       first look only: how long ago lastmod was, by a clock known to be
//               right; 0 when the clock isn't (the gap is then timed from now)
//   quiet_ms    the threshold
// The first look sets the baseline and never says anything: on boot, a node that
// has been quiet all along is marked quiet silently, so only its return is news.
inline Result step(Entry& x, uint32_t lastmod, uint32_t now_ms, uint32_t age_s, uint32_t quiet_ms) {
  if (!lastmod) return { Event::None, 0 };
  if (!x.seen) {
    if (age_s > kMaxBaselineAgeS) age_s = 0;
    x.seen = true;
    x.lastmod = lastmod;
    x.heard_ms = now_ms - age_s * 1000u;   // wraps below zero on purpose
    x.quiet = (uint64_t)age_s * 1000u >= quiet_ms;
    return { Event::None, 0 };
  }
  const uint32_t since_ms = now_ms - x.heard_ms;
  if (lastmod != x.lastmod) {
    x.lastmod = lastmod;
    x.heard_ms = now_ms;
    if (x.quiet) {
      x.quiet = false;
      return { Event::BackOnAir, since_ms / 1000u };
    }
    return { Event::None, 0 };
  }
  if (!x.quiet && since_ms >= quiet_ms) {
    x.quiet = true;
    return { Event::WentQuiet, since_ms / 1000u };
  }
  return { Event::None, 0 };
}

// "45 min", "3 h 20 min", "2 d 4 h". Under a minute reads "1 min".
inline void formatGap(uint32_t s, char* out, size_t cap) {
  if (!out || !cap) return;
  const uint32_t m = s / 60u, h = m / 60u, d = h / 24u;
  if (h == 0)       snprintf(out, cap, "%u min", (unsigned)(m ? m : 1));
  else if (d == 0)  { if (m % 60u) snprintf(out, cap, "%u h %u min", (unsigned)h, (unsigned)(m % 60u));
                      else         snprintf(out, cap, "%u h", (unsigned)h); }
  else              { if (h % 24u) snprintf(out, cap, "%u d %u h", (unsigned)d, (unsigned)(h % 24u));
                      else         snprintf(out, cap, "%u d", (unsigned)d); }
}

}  // namespace nodewatch
