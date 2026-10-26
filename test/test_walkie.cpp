// Host test for src/ui-touch/WalkieWire.h (push-to-talk). The golden bytes are
// the ones MeshCore Open TAK's test/vox/walkie_wire_test.dart checks.
//   g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -I src test/test_walkie.cpp -o /tmp/walkie && /tmp/walkie
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui-touch/WalkieWire.h"

static int g_fail = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      g_fail++;                                                     \
    }                                                               \
  } while (0)

static void hex(const uint8_t* b, int n, char* out) {
  for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
  out[2 * n] = 0;
}
static int unhex(const char* s, uint8_t* out) {
  int n = 0;
  for (; s[0] && s[1]; s += 2) {
    unsigned v;
    sscanf(s, "%2x", &v);
    out[n++] = (uint8_t)v;
  }
  return n;
}

static void golden() {
  uint8_t b[200];
  char h[420];
  int n = WalkieWire::encodeStart(0x1a2b3c4d, 1, "Alex", b, sizeof b);
  hex(b, n, h);
  CHECK(strcmp(h, "531a2b3c4d01416c6578") == 0);
  WalkieWire::Start s;
  CHECK(WalkieWire::parseStart(b, (size_t)n, &s));
  CHECK(s.sid == 0x1a2b3c4d && s.mode == 1 && strcmp(s.name, "Alex") == 0);

  uint8_t frames[12];
  for (int i = 0; i < 12; i++) frames[i] = (uint8_t)i;
  n = WalkieWire::encodeAudio(0x1a2b3c4d, 3, 1, true, frames, 12, b, sizeof b);
  hex(b, n, h);
  CHECK(strcmp(h, "541a2b3c4d0381000102030405060708090a0b") == 0);
  WalkieWire::Audio a;
  CHECK(WalkieWire::parseAudio(b, (size_t)n, &a));
  CHECK(a.index == 3 && a.last && a.len == 12 && a.totalIfLast() == 4 && a.mode == 1);

  n = unhex("541a2b3c4d0581", b);
  CHECK(WalkieWire::parseAudio(b, (size_t)n, &a));
  CHECK(a.last && a.len == 0 && a.totalIfLast() == 5);

  // Not talks.
  n = unhex("541a2b3c4d00", b);
  CHECK(!WalkieWire::parseAudio(b, (size_t)n, &a));        // short
  n = unhex("541a2b3c4d00010001", b);
  CHECK(!WalkieWire::parseAudio(b, (size_t)n, &a));        // half a frame
  n = unhex("541a2b3c4d0020000102030405", b);
  CHECK(!WalkieWire::parseAudio(b, (size_t)n, &a));        // Mimi
  n = unhex("531a2b3c4d28", b);
  CHECK(!WalkieWire::parseStart(b, (size_t)n, &s));        // AV1 image
  n = unhex("561a2b3c4d00", b);
  CHECK(!WalkieWire::isLive(b, (size_t)n));

  // A full packet is VE3's size and fills a channel datagram (163 bytes).
  uint8_t full[156] = {0};
  n = WalkieWire::encodeAudio(1, 0, 1, false, full, 156, b, sizeof b);
  CHECK(n == 163);
  CHECK(WalkieWire::parseAudio(b, (size_t)n, &a) && a.len == 156 && !a.last);

  // Long names cut on a character boundary (as the app: 16 x "é").
  char longname[100] = "";
  for (int i = 0; i < 40; i++) strcat(longname, "\xc3\xa9");
  n = WalkieWire::encodeStart(1, 1, longname, b, sizeof b);
  CHECK(n == 6 + 32);
  CHECK(WalkieWire::parseStart(b, (size_t)n, &s) && strlen(s.name) == 32);
}

// The same timelines as the app's jitter tests.
static WalkieWire::Audio pkt(uint8_t* buf, int i, bool last = false, int bytes = 156) {
  memset(buf, i, (size_t)bytes);
  WalkieWire::Audio a;
  a.sid = 1;
  a.index = (uint8_t)i;
  a.last = last;
  a.data = buf;
  a.len = bytes;
  return a;
}

static int drain(WalkieWire::Jitter& j, uint32_t now, int* first_byte = nullptr) {
  const uint8_t* d;
  int len, n = 0, idx;
  while ((idx = j.due(now, &d, &len)) >= 0) {
    if (n == 0 && first_byte) *first_byte = d[0];
    n++;
  }
  return n;
}

static void jitter() {
  static uint8_t buf[200];
  auto* j = (WalkieWire::Jitter*)calloc(1, sizeof(WalkieWire::Jitter));
  int fb = -1;

  j->reset();   // waits a little, then plays in order
  j->add(pkt(buf, 0), 0);
  CHECK(drain(*j, 100) == 0);
  CHECK(drain(*j, 500) == 1);
  j->add(pkt(buf, 1), 1100);
  CHECK(drain(*j, 1100, &fb) == 1 && fb == 1);
  j->add(pkt(buf, 2, true, 60), 2150);
  CHECK(drain(*j, 2150) == 1);
  CHECK(j->finished(2150));

  j->reset();   // starts at once with two in
  j->add(pkt(buf, 0), 0);
  j->add(pkt(buf, 1), 50);
  CHECK(drain(*j, 60) == 2);

  j->reset();   // skips a lost packet once it's too late
  j->add(pkt(buf, 0), 0);
  j->add(pkt(buf, 1), 10);
  CHECK(drain(*j, 10) == 2);
  j->add(pkt(buf, 3), 2100);
  CHECK(drain(*j, 2200) == 0);
  CHECK(drain(*j, 2500, &fb) == 1 && fb == 3);
  CHECK(j->nextIndex() == 4);

  j->reset();   // a late packet already skipped is ignored
  j->add(pkt(buf, 0), 0);
  j->add(pkt(buf, 2), 10);
  drain(*j, 10);
  drain(*j, 1500);
  j->add(pkt(buf, 1), 1600);
  CHECK(drain(*j, 1600) == 0);

  j->reset();   // first packet lost: starts at the next
  j->add(pkt(buf, 1), 0);
  CHECK(drain(*j, 600, &fb) == 1 && fb == 1);

  j->reset();   // a talk that goes quiet ends
  j->add(pkt(buf, 0), 0);
  drain(*j, 600);
  CHECK(!j->finished(2000));
  CHECK(j->finished(5000));

  // millis() wrapping during a talk.
  j->reset();
  const uint32_t t0 = 0xFFFFFF00u;
  j->add(pkt(buf, 0), t0);
  j->add(pkt(buf, 1), t0 + 10);
  CHECK(drain(*j, t0 + 10) == 2);
  j->add(pkt(buf, 2, true, 6), t0 + 1100);
  CHECK(drain(*j, t0 + 1100) == 1);
  CHECK(j->finished(t0 + 1100));
  free(j);
}

// DM talks: the same bytes as MeshCore Open TAK's walkie_wire_test.dart.
static void dm() {
  uint8_t to[8], from[6], codec[6], b[64], self2[4];
  char h[140];
  unhex("0102030405060708", to);
  unhex("a0a1a2a3a4a5", from);
  unhex("c0ffee112233", codec);
  const int n = WalkieWire::encodeDmAudio(0x1a2b3c4d, 2, 1, true, to, from, codec, 6, b, sizeof b);
  hex(b, n, h);
  CHECK(strcmp(h, "56cd8afc93028101020304a0a1a2a3c0ffee112233") == 0);
  WalkieWire::DmAudio d;
  CHECK(WalkieWire::parseDmAudio(b, (size_t)n, to, &d));
  CHECK(d.audio.sid == 0x1a2b3c4d && d.audio.index == 2 && d.audio.last && d.audio.len == 6);
  CHECK(memcmp(d.from, from, 4) == 0 && memcmp(d.audio.data, codec, 6) == 0);
  unhex("01020305", self2);
  CHECK(!WalkieWire::parseDmAudio(b, (size_t)n, self2, &d));   // not to us
  CHECK(!WalkieWire::parseDmAudio(b, (size_t)n - 2, to, &d));   // a partial frame
  uint8_t ve3[32];
  const int m = unhex("561a2b3c4d00c0ffee11223344556677", ve3);
  CHECK(!WalkieWire::parseDmAudio(ve3, (size_t)m, to, &d));     // an ordinary VE3 packet
}

int main() {
  golden();
  dm();
  jitter();
  if (g_fail) {
    printf("%d failure(s)\n", g_fail);
    return 1;
  }
  printf("walkie: all passed\n");
  return 0;
}
