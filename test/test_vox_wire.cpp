// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <vector>

#include "ui-touch/VoxWire.h"

using namespace VoxWire;

static const uint8_t kKey6[6] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6};

// Packetizes like voxPacketize() in meshcore-open-tak (156-byte slices).
static int packetize(uint32_t sid, const uint8_t* payload, int len, uint8_t pkts[][kPacketHeaderLen + kBytesPerPacket + 1],
                     int* lens, bool salted) {
  int n = 0;
  for (int off = 0; off < len; off += kBytesPerPacket, n++) {
    const int chunk = len - off < kBytesPerPacket ? len - off : kBytesPerPacket;
    pkts[n][0] = salted ? kPacketSaltedMagic : kPacketMagic;
    writeSid(sid, pkts[n] + 1);
    pkts[n][5] = (uint8_t)n;
    memcpy(pkts[n] + kPacketHeaderLen, payload + off, (size_t)chunk);
    lens[n] = kPacketHeaderLen + chunk;
    if (salted) pkts[n][lens[n]++] = (uint8_t)(0x5A ^ n);
  }
  return n;
}

int main() {
  // --- envelope (expected strings computed independently of this code)
  Envelope env;
  assert(parseImageEnvelope("VI1:79e6a5:14:10:46", &env));
  assert(env.sid == 0x1a2b3c4d && env.total == 36 && env.aspect == 150);
  assert(parseImageEnvelope("  VI1:79e6a5:14:10:46\n", &env));
  assert(!parseImageEnvelope("VE3:79e6a5:1:a:5", &env));        // voice, not a photo
  assert(!parseImageEnvelope("VI1:79e6a5:w:10:46", &env));        // Mimi mode id under VI1
  assert(!parseImageEnvelope("VI1:79e6a5:14:0:46", &env));        // zero packets
  assert(!parseImageEnvelope("VI1:79e6a5:14:10", &env));          // missing field
  assert(!parseImageEnvelope("VI1:79e6a5:14:10:46:x", &env));     // extra field
  assert(!parseImageEnvelope("VI1:79E6A5:14:10:46", &env));       // uppercase isn't base36 here
  assert(!parseImageEnvelope("VI1:79e6a5:14:10:46x", &env));      // bad last field

  // --- caption after the envelope (voice_wire.dart's VoxVoiceEnvelope.caption)
  size_t cl = 99;
  const char* cap = envelopeCaption("VI1:79e6a5:14:10:46", &cl);
  assert(cap && cl == 0);                                         // none
  assert(parseImageEnvelope("VI1:79e6a5:14:10:46 Look at this: T-Deck!", &env));
  assert(env.sid == 0x1a2b3c4d && env.total == 36 && env.aspect == 150);
  cap = envelopeCaption("VI1:79e6a5:14:10:46 Look at this: T-Deck!\n", &cl);
  assert(cap && cl == 21 && strncmp(cap, "Look at this: T-Deck!", cl) == 0);
  cap = envelopeCaption("VE3:79e6a5:1:6:7\ntwo\nlines", &cl);
  assert(cap && cl == 9 && strncmp(cap, "two\nlines", cl) == 0);
  assert(!parseEnvelope("VE3:79e6a5:1:6:7X hi", &env));        // a caption doesn't rescue a bad field
  assert(!envelopeCaption("hello there", &cl) && cl == 0);
  // The plain word for its kind isn't part of the caption; another one is.
  cap = envelopeCaption("VI1:79e6a5:14:10:46 IMAGE", &cl);
  assert(cap && cl == 0);
  cap = envelopeCaption("VI1:79e6a5:14:10:46 IMAGE Look", &cl);
  assert(cap && cl == 4 && strncmp(cap, "Look", cl) == 0);
  cap = envelopeCaption("VI1:79e6a5:14:10:46 IMAGES", &cl);
  assert(cap && cl == 6);
  cap = envelopeCaption("VM1:79e6a5:w:5:5 VOICE", &cl);
  assert(cap && cl == 0);
  cap = envelopeCaption("VE3:79e6a5:1:6:7 VOICE", &cl);   // VE3 has none: kept
  assert(cap && cl == 5);
  cap = envelopeCaption("VE3:79e6a5:1:6:7 PTT", &cl);
  assert(cap && cl == 3 && strncmp(cap, "PTT", 3) == 0);
  assert(!parseImageEnvelope("VI1:zzzzzzzz:14:10:46", &env));     // sid too long
  assert(!parseImageEnvelope(nullptr, &env));

  // --- voice envelopes (VE3 Codec2, VM1 Mimi) and per-mode packet sizes
  assert(parseEnvelope("VE3:79e6a5:1:6:7", &env));            // 1200 bps, 6 packets, 7 s
  assert(env.kind == kCodec2 && env.mode == 1 && env.total == 6 && env.seconds() == 7);
  assert(env.packetBytes() == 156 && codec2Mode(env.mode) == 5);
  assert(parseEnvelope("VM1:79e6a5:w:12:k", &env) && env.kind == kMimi && env.packetBytes() == 154);
  assert(parseEnvelope("VI1:79e6a5:14:10:46", &env) && env.kind == kImage && env.seconds() == 0);
  assert(!parseEnvelope("VE3:79e6a5:7:6:7", &env));            // no Codec2 mode 7
  assert(!parseEnvelope("VE3:79e6a5:w:6:7", &env));            // Mimi id under VE3
  assert(!parseEnvelope("VM1:79e6a5:1:6:7", &env));            // Codec2 id under VM1
  assert(!parseImageEnvelope("VE3:79e6a5:1:6:7", &env));
  const int pb[] = {156, 156, 156, 154, 154, 152, 152};          // voice_wire.dart bytesPerPacket
  const int c2[] = {8, 5, 1, 4, 3, 2, 0};                         // CODEC2_MODE_* per wire id
  for (int m = 0; m <= 6; m++) assert(packetBytes(m) == pb[m] && codec2Mode(m) == c2[m]);
  assert(packetBytes(9) == 0 && codec2Mode(32) == -1);

  // --- text fetch request
  char txt[128];
  const uint8_t miss[] = {35, 2, 1, 3, 2};
  assert(encodeTextFetch(0x1a2b3c4d, kKey6, miss, 5, txt, sizeof txt) > 0);
  assert(strcmp(txt, "VR3:79e6a5:m1-3.z:a1b2c3d4e5f6") == 0);
  assert(encodeTextFetch(0x1a2b3c4d, kKey6, nullptr, 0, txt, sizeof txt) > 0);
  assert(strcmp(txt, "VR3:79e6a5:a:a1b2c3d4e5f6") == 0);
  assert(encodeTextFetch(0x1a2b3c4d, kKey6, nullptr, 0, txt, 10) == 0);

  // --- binary fetch request
  uint8_t req[200];
  int n = encodeFetch(0x1a2b3c4d, kKey6, miss, 5, req, sizeof req);
  const uint8_t want[] = {0x72, 0x1a, 0x2b, 0x3c, 0x4d, 0x01, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 4, 1, 2, 3, 35};
  assert(n == (int)sizeof want && memcmp(req, want, sizeof want) == 0);
  n = encodeFetch(0x1a2b3c4d, kKey6, nullptr, 0, req, sizeof req);
  assert(n == kFetchMinLen && req[5] == 0x00 && req[12] == 0);
  uint8_t many[200];
  for (int i = 0; i < 200; i++) many[i] = (uint8_t)i;
  n = encodeFetch(1, kKey6, many, 200, req, sizeof req);
  assert(n == kFetchMinLen + kMaxFetchIndices && req[12] == kMaxFetchIndices);
  // Salt + reply path, as meshcore-open-tak's VoxVoiceFetchRequest encodes them.
  const uint8_t route[] = {0xAB, 0xCD};
  n = encodeFetch(0x1a2b3c4d, kKey6, miss, 5, req, sizeof req, true, route, 2);
  const uint8_t want2[] = {0x72, 0x1a, 0x2b, 0x3c, 0x4d, 0x07, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6,
                           4, 1, 2, 3, 35, 2, 0xAB, 0xCD};
  assert(n == (int)sizeof want2 && memcmp(req, want2, sizeof want2) == 0);
  // Direct neighbour: an empty reply path still says "reply the way I came".
  n = encodeFetch(0x1a2b3c4d, kKey6, nullptr, 0, req, sizeof req, true, route, 0);
  assert(n == kFetchMinLen + 1 && req[5] == 0x06 && req[13] == 0);
  uint8_t longpath[9] = {0};
  n = encodeFetch(1, kKey6, nullptr, 0, req, sizeof req, true, longpath, 9);   // too long: dropped
  assert(n == kFetchMinLen && req[5] == 0x02);
  assert(isControlText("VR3:abc:a:a1b2c3d4e5f6") && !isControlText("VI1:x") && !isControlText(nullptr));
  assert(isTransferReport("VT1:0a1b2c3d:100:1:17/17") && isTransferReport("Bob: VT1:0a1b2c3d:1:1:1/1"));
  assert(!isTransferReport("Bob: hello VT1:") && !isTransferReport("VR3:x") && !isTransferReport(nullptr));
  assert(encodeFetch(1, kKey6, nullptr, 0, req, 12) == 0);
  // Salted: index (salt >> 4) % n repeated (salt & 15) + 1 times; the parsed set is unchanged.
  {
    uint8_t a[200], b[200];
    const int na = encodeFetch(0x1a2b3c4d, kKey6, miss, 5, a, sizeof a, true, route, 2, 0x00);
    const int nb = encodeFetch(0x1a2b3c4d, kKey6, miss, 5, b, sizeof b, true, route, 2, 0x13);
    assert(na == (int)sizeof want2 + 1 && a[12] == 5 && a[17] == 1);   // index 0 of {1,2,3,35}, once
    assert(nb == (int)sizeof want2 + 4 && b[12] == 8);                  // index 1 (=2), four times
    assert(b[17] == 2 && b[18] == 2 && b[19] == 2 && b[20] == 2 && b[21] == 2 /* path len */ && b[22] == 0xAB);
    assert(na != nb || memcmp(a, b, (size_t)na) != 0);
    FetchRequest ra, rb;
    assert(parseFetch(a, (size_t)na, &ra) && parseFetch(b, (size_t)nb, &rb));
    assert(ra.n_missing == 4 && rb.n_missing == 4 && rb.missing[3] == 35 && rb.has_path && rb.path_len == 2);
    // Room for the salt: at most kSaltedFetchIndices listed, never more than kMaxFetchIndices sent.
    const int nm = encodeFetch(1, kKey6, many, 200, a, sizeof a, true, nullptr, 0, 0xFF);
    assert(nm == kFetchMinLen + kMaxFetchIndices && a[12] == kMaxFetchIndices);
    assert(parseFetch(a, (size_t)nm, &ra) && ra.n_missing == kSaltedFetchIndices);
    // Consecutive salts: distinct requests for one missing packet, 16 in a row.
    std::set<std::vector<uint8_t>> distinct;
    const uint8_t one[] = {214};
    for (int k = 250; k < 250 + 16; k++) {
      const int nk = encodeFetch(1, kKey6, one, 1, a, sizeof a, true, nullptr, 0, k & 0xFF);
      distinct.insert(std::vector<uint8_t>(a, a + nk));
    }
    assert(distinct.size() == 16);
    // Nothing to repeat in an all-packets request.
    assert(encodeFetch(1, kKey6, nullptr, 0, a, sizeof a, true, nullptr, 0, 0x55) == kFetchMinLen);
  }

  // --- packet parsing
  uint8_t raw[] = {0x56, 0, 0, 0, 7, 3, 9, 8, 7};
  Packet p;
  assert(parsePacket(raw, sizeof raw, &p) && p.sid == 7 && p.index == 3 && p.len == 3 && p.data[0] == 9);
  uint8_t salted[] = {0x57, 0, 0, 0, 7, 3, 9, 8, 7, 0xEE};
  assert(parsePacket(salted, sizeof salted, &p) && p.len == 3 && p.data[2] == 7);
  uint8_t empty[] = {0x56, 0, 0, 0, 7, 3};
  assert(!parsePacket(empty, sizeof empty, &p));
  assert(!parsePacket(want, sizeof want, &p));                    // a fetch request isn't data

  // --- reassembly of a 5,350-byte "More detail" payload, out of order, with
  //     duplicates, salted resends and another session's packets mixed in.
  const int kLen = 5350;
  uint8_t* payload = (uint8_t*)malloc(kLen);
  payload[0] = kImagePayloadVersion;
  for (int i = 1; i < kLen; i++) payload[i] = (uint8_t)(i * 31 + 7);
  static uint8_t pk[64][kPacketHeaderLen + kBytesPerPacket + 1];
  static uint8_t pks[64][kPacketHeaderLen + kBytesPerPacket + 1];
  int lens[64], lens_s[64];
  const uint32_t sid = 0xdeadbeef;
  const int total = packetize(sid, payload, kLen, pk, lens, false);
  packetize(sid, payload, kLen, pks, lens_s, true);
  assert(total == 35);

  static uint8_t store[64 * kBytesPerPacket];
  Assembly a;
  assert(!a.begin(sid, 0, store, sizeof store));
  assert(!a.begin(sid, (uint8_t)(kMaxImagePackets + 1), store, sizeof store));
  assert(!a.begin(sid, (uint8_t)total, store, 10));
  assert(a.begin(sid, (uint8_t)total, store, sizeof store));

  for (int i = total - 1; i >= 0; i -= 2) {                       // odd ones, backwards
    assert(parsePacket(pk[i], (size_t)lens[i], &p));
    assert(a.add(p));
    assert(!a.add(p));                                            // duplicate ignored
  }
  assert(!a.complete());
  uint8_t gaps[kMaxPackets];
  int g = a.missing(gaps);
  assert(g == total / 2 && gaps[0] == 1);

  uint8_t other[16] = {0x56, 0, 0, 0, 1, 1, 1};                  // wrong session
  assert(parsePacket(other, 8, &p) && !a.add(p));
  uint8_t shortmid[] = {0x56, 0xde, 0xad, 0xbe, 0xef, 1, 1, 2};   // a middle packet must be full
  assert(parsePacket(shortmid, sizeof shortmid, &p) && !a.add(p));

  for (int i = 0; i < g; i++) {                                   // salted resends fill the gaps
    assert(parsePacket(pks[gaps[i]], (size_t)lens_s[gaps[i]], &p));
    assert(a.add(p));
  }
  assert(a.complete() && a.received() == total);
  assert(a.payloadLen() == (size_t)kLen && memcmp(a.payload(), payload, kLen) == 0);

  size_t obu_len = 0;
  const uint8_t* obus = imageObus(a.payload(), a.payloadLen(), &obu_len);
  assert(obus == a.payload() + 1 && obu_len == (size_t)kLen - 1);
  uint8_t badver[] = {0x09, 0x12};
  assert(!imageObus(badver, 2, &obu_len));
  // Version 2: a thumbnail first, the photo after it.
  {
    uint8_t v2[] = {0x02, 3, 0xA1, 0xA2, 0xA3, 0xB1, 0xB2};
    assert(imageObus(v2, sizeof v2, &obu_len) == v2 + 5 && obu_len == 2);
    size_t tl = 0;
    assert(imageThumbObus(v2, sizeof v2, &tl) == v2 + 2 && tl == 3);
    assert(imageThumbObus(v2, 4, &tl) == nullptr);           // not all of it in yet
    uint8_t none[] = {0x02, 0, 0xB1};
    assert(!imageThumbObus(none, sizeof none, &tl));         // sent without one
    assert(imageObus(none, sizeof none, &obu_len) == none + 2 && obu_len == 1);
    assert(!imageThumbObus(a.payload(), a.payloadLen(), &tl));   // version 1 has none
    uint8_t cut[] = {0x02, 3, 0xA1, 0xA2, 0xA3};
    assert(!imageObus(cut, sizeof cut, &obu_len));           // a thumbnail and no photo
    assert(isImagePayloadVersion(1) && isImagePayloadVersion(2) && !isImagePayloadVersion(3));
  }

  free(payload);

  // --- a 1300 bps clip: 154-byte packets (22 frames of 7 bytes)
  {
    const int kPb = 154, kLen = 154 * 3 + 21;
    uint8_t clip[kLen];
    for (int i = 0; i < kLen; i++) clip[i] = (uint8_t)(i * 7 + 1);
    static uint8_t vstore[4 * 154];
    Assembly v;
    assert(!v.begin(9, 4, vstore, sizeof vstore, 157));
    assert(v.begin(9, 4, vstore, sizeof vstore, kPb));
    for (int i = 3; i >= 0; i--) {
      uint8_t pkt[kPacketHeaderLen + kPb];
      const int n = i == 3 ? 21 : kPb;
      pkt[0] = kPacketMagic; writeSid(9, pkt + 1); pkt[5] = (uint8_t)i;
      memcpy(pkt + kPacketHeaderLen, clip + i * kPb, (size_t)n);
      Packet q;
      assert(parsePacket(pkt, (size_t)(kPacketHeaderLen + n), &q) && v.add(q));
    }
    assert(v.complete() && v.payloadLen() == (size_t)kLen && memcmp(v.payload(), clip, kLen) == 0);
    uint8_t wrong[kPacketHeaderLen + 156] = {kPacketMagic, 0, 0, 0, 9, 0};
    Assembly w;
    assert(w.begin(9, 4, vstore, sizeof vstore, kPb));
    assert(parsePacket(wrong, sizeof wrong, &p) && !w.add(p));   // a 156-byte packet doesn't fit 154
  }
  return 0;
}
