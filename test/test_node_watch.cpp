// SPDX-License-Identifier: GPL-3.0-or-later
// Watch a node (ui-touch/NodeWatch.h): went quiet / back on the air, and the saved list.
// Build: g++ -std=c++17 -Wall -Wextra -I src -o /tmp/test_node_watch test/test_node_watch.cpp
#include "ui-touch/NodeWatch.h"

#include <cstdio>
#include <cstring>
#include <type_traits>

using namespace nodewatch;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static const uint32_t H = 3600u * 1000u;   // an hour of millis()

static void testQuietThenBack() {
  Entry x{};
  const uint32_t q = 13 * H;
  // First look: heard 1 h ago. Nothing said; not quiet.
  Result r = step(x, 1000, 5000, 3600, q);
  CHECK(r.ev == Event::None && x.seen && !x.quiet);
  // 11 h later, still silent: under the threshold.
  r = step(x, 1000, 5000 + 11 * H, 0, q);
  CHECK(r.ev == Event::None);
  // 12 h later (13 h since heard): went quiet, once.
  r = step(x, 1000, 5000 + 12 * H, 0, q);
  CHECK(r.ev == Event::WentQuiet && r.gap_s == 13u * 3600u);
  r = step(x, 1000, 5000 + 20 * H, 0, q);
  CHECK(r.ev == Event::None);
  // Heard again 24 h after it was last heard: back, quiet for 24 h.
  r = step(x, 99999, 5000 + 23 * H, 0, q);
  CHECK(r.ev == Event::BackOnAir && r.gap_s == 24u * 3600u);
  CHECK(!x.quiet);
  // Heard again soon after: no news.
  r = step(x, 100500, 5000 + 24 * H, 0, q);
  CHECK(r.ev == Event::None);
}

static void testHeardResetsTimer() {
  Entry x{};
  const uint32_t q = 2 * H;
  step(x, 10, 0, 0, q);
  CHECK(step(x, 20, 1 * H, 0, q).ev == Event::None);   // heard: timer restarts
  CHECK(step(x, 20, 2 * H + 1000, 0, q).ev == Event::None);   // 1 h since, not 2
  const Result r = step(x, 20, 3 * H, 0, q);
  CHECK(r.ev == Event::WentQuiet && r.gap_s == 2u * 3600u);
}

static void testBootAlreadyQuiet() {
  // Booted with a node last heard 2 days ago: marked quiet silently; its return is news.
  Entry x{};
  const uint32_t q = 13 * H;
  Result r = step(x, 500, 4000, 2 * 86400, q);
  CHECK(r.ev == Event::None && x.quiet);
  CHECK(step(x, 500, 4000 + H, 0, q).ev == Event::None);
  r = step(x, 600, 4000 + H, 0, q);
  CHECK(r.ev == Event::BackOnAir && r.gap_s == 2u * 86400u + 3600u);
}

static void testUnknownClock() {
  // No trustworthy clock at the first look: the gap is timed from then.
  Entry x{};
  const uint32_t q = 1 * H;
  step(x, 500, 100, 0, q);
  CHECK(!x.quiet);
  const Result r = step(x, 500, 100 + H, 0, q);
  CHECK(r.ev == Event::WentQuiet && r.gap_s == 3600u);
  // An absurd age is treated as unknown too.
  Entry y{};
  step(y, 500, 100, 400u * 86400u, q);
  CHECK(!y.quiet);
}

static void testGoneContact() {
  Entry x{};
  CHECK(step(x, 0, 100, 0, H).ev == Event::None);
  CHECK(!x.seen);   // no baseline until the contact exists
}

static void testMillisWrap() {
  // millis() wraps after 49.7 days; gaps across the wrap still come out right.
  Entry x{};
  const uint32_t q = 2 * H;
  const uint32_t t0 = 0xFFFFFFFFu - H / 2;
  step(x, 7, t0, 0, q);
  CHECK(step(x, 7, t0 + H, 0, q).ev == Event::None);
  const Result r = step(x, 7, t0 + 2 * H, 0, q);
  CHECK(r.ev == Event::WentQuiet && r.gap_s == 7200u);
  // A first look on a freshly booted device for a node heard 5 h ago (heard_ms wraps).
  Entry y{};
  step(y, 9, 1000, 5 * 3600, 6 * H);
  const Result r2 = step(y, 9, 1000 + H, 0, 6 * H);
  CHECK(r2.ev == Event::WentQuiet && r2.gap_s == 6u * 3600u);
}

static void testListAndBlob() {
  List l{};
  l.reset();
  uint8_t k[kMax + 1][kKeyLen];
  for (int i = 0; i <= kMax; ++i) for (int j = 0; j < kKeyLen; ++j) k[i][j] = (uint8_t)(i * 7 + j);
  bool full = true;
  CHECK(l.toggle(k[0], &full) && !full);
  CHECK(l.has(k[0]) && l.n == 1);
  CHECK(!l.toggle(k[0]) && l.n == 0);
  for (int i = 0; i < kMax; ++i) CHECK(l.toggle(k[i]));
  CHECK(!l.toggle(k[kMax], &full) && full && l.n == kMax);
  // Removing from the middle keeps the order of the rest.
  l.toggle(k[3]);
  CHECK(l.n == kMax - 1 && memcmp(l.e[3].key, k[4], kKeyLen) == 0);

  l.quiet_hours = 6;
  uint8_t blob[kBlobMax];
  const size_t n = l.pack(blob, sizeof blob);
  CHECK(n == 2 + (size_t)(kMax - 1) * kKeyLen && blob[0] == kBlobVersion && blob[1] == 6);
  CHECK(l.pack(blob, 3) == 0);
  List m{};
  CHECK(m.unpack(blob, n) && m.n == kMax - 1 && m.quiet_hours == 6);
  for (int i = 0; i < m.n; ++i) CHECK(memcmp(m.e[i].key, l.e[i].key, kKeyLen) == 0 && !m.e[i].seen);

  // Malformed blobs: empty list, default threshold.
  CHECK(!m.unpack(blob, n - 1) && m.n == 0 && m.quiet_hours == kDefaultQuietHours);
  uint8_t bad[2] = { 9, 5 };
  CHECK(!m.unpack(bad, 2) && m.n == 0);
  CHECK(!m.unpack(nullptr, 0));
  // An out-of-range saved threshold is clamped.
  uint8_t hi[2] = { kBlobVersion, 250 };
  CHECK(m.unpack(hi, 2) && m.quiet_hours == kMaxQuietHours && m.n == 0);
  uint8_t lo[2] = { kBlobVersion, 0 };
  CHECK(m.unpack(lo, 2) && m.quiet_hours == kMinQuietHours);
  List d{};
  d.unpack(nullptr, 0);
  CHECK(d.quietMs() == (uint32_t)kDefaultQuietHours * H && d.n == 0);
  static_assert(std::is_trivially_default_constructible<List>::value, "psramStatic needs plain data");
}

static void testFormat() {
  char b[24];
  formatGap(0, b, sizeof b);         CHECK(!strcmp(b, "1 min"));
  formatGap(45 * 60, b, sizeof b);   CHECK(!strcmp(b, "45 min"));
  formatGap(3600, b, sizeof b);      CHECK(!strcmp(b, "1 h"));
  formatGap(3 * 3600 + 20 * 60 + 9, b, sizeof b); CHECK(!strcmp(b, "3 h 20 min"));
  formatGap(86400, b, sizeof b);     CHECK(!strcmp(b, "1 d"));
  formatGap(2 * 86400 + 4 * 3600 + 59 * 60, b, sizeof b); CHECK(!strcmp(b, "2 d 4 h"));
}

int main() {
  testQuietThenBack();
  testHeardResetsTimer();
  testBootAlreadyQuiet();
  testUnknownClock();
  testGoneContact();
  testMillisWrap();
  testListAndBlob();
  testFormat();
  if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
  std::printf("node watch: all passed\n");
  return 0;
}
