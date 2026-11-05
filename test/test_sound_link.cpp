// Sound link on the host: Wavest's message shapes, share links, and ggwave
// with the radios' own settings (SoundLinkUI.inc): 16 kHz in from the mic,
// 24 kHz / 512 inside, the recording capped at GGWAVE_MAX_RECORDED_FRAMES.
//
// Built and run by test/build_sound_link_test.sh (ggwave compiled apart, without -Werror).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "ggwave/ggwave.h"
#include "ui-touch/SoundLinkCore.h"

void soundLinkOnChannelText(int, const char*) {}

static int g_fail = 0;
#define CHECK(c)                                                   \
  do {                                                             \
    if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } \
  } while (0)

using namespace soundlink;

static void testBase64() {
  char out[64];
  const uint8_t in[] = {'h', 'i', '!', 0xFF, 0x00};
  CHECK(b64Encode(in, 5, out, sizeof out) && !strcmp(out, "aGkh/wA="));
  uint8_t back[8];
  size_t n = 0;
  CHECK(b64Decode(out, strlen(out), back, sizeof back, &n) && n == 5 && !memcmp(back, in, 5));
  CHECK(!b64Decode("a$==", 4, back, sizeof back, &n));
  CHECK(!b64Encode(in, 5, out, 8));
}

static void testPlain() {
  char out[200];
  CHECK(buildPlain("Ja|de:", "hello mesh", out, sizeof out) && !strcmp(out, "U:Jade|hello mesh"));
  Heard h = parse(out);
  CHECK(h.kind == Kind::Plain && !strcmp(h.sender, "Jade") && !strcmp(h.body, "hello mesh"));
  // Cut to one message, never inside a character.
  std::string lng;
  for (int i = 0; i < 80; i++) lng += "\xC3\xA9";   // 160 bytes of é
  const size_t n = buildPlain("Jade", lng.c_str(), out, sizeof out);
  CHECK(n <= kMaxBytes && n == strlen(out));
  CHECK(((uint8_t)out[n - 1] & 0xC0) == 0x80 && (uint8_t)out[n - 2] == 0xC3);
  // Not Wavest's: shown as heard.
  h = parse("meshcore://channel/add?name=x&secret=00");
  CHECK(h.kind == Kind::Raw);
  h = parse("U:no bar here");
  CHECK(h.kind == Kind::Raw);
}

static void testAes() {
  // Wavest's own output for key "mesh", IV 00..0b, "hello mesh 🌲" (also in
  // MeshCore Open TAK's test/sound/wavest_format_test.dart).
  const char* v = "E:Jade|AES:AAECAwQFBgcICQoL0F2Mxz3x1NNXhFUv0mL8INKtS7ye7t16eEcjR5p45A==";
  Heard h = parse(v);
  CHECK(h.kind == Kind::Aes && !strcmp(h.sender, "Jade"));
  uint8_t iv[12], ct[140], tag[16];
  size_t n = 0;
  CHECK(aesSplit(h.body, iv, ct, sizeof ct, &n, tag));
  CHECK(n == strlen("hello mesh \xF0\x9F\x8C\xB2"));
  for (int i = 0; i < 12; i++) CHECK(iv[i] == i);
  // Sealing the same bytes again gives the same message.
  uint8_t sealed[140];
  memcpy(sealed, iv, 12);
  memcpy(sealed + 12, ct, n);
  memcpy(sealed + 12 + n, tag, 16);
  char out[200];
  CHECK(buildAes("Jade", sealed, n + 28, out, sizeof out) && !strcmp(out, v));
  // The biggest text that fits does fit, one more byte doesn't.
  const size_t m = aesMaxText("Jade");
  CHECK(m > 60 && m < 90);
  uint8_t big[200] = {};
  CHECK(buildAes("Jade", big, m + 28, out, sizeof out) > 0);
  CHECK(buildAes("Jade", big, m + 28 + 3, out, sizeof out) == 0);
  CHECK(!aesSplit("AAEC", iv, ct, sizeof ct, &n, tag));
}

static void testXor() {
  // "hi" XOR "k" = 0x03 0x02 -> "AwI="
  Heard h = parse("E:Bob|XOR:AwI=");
  CHECK(h.kind == Kind::Xor && !strcmp(h.sender, "Bob"));
  char out[16];
  CHECK(xorOpen(h.body, "k", out, sizeof out) && !strcmp(out, "hi"));
  CHECK(!xorOpen(h.body, "", out, sizeof out));
}

static void testLinks() {
  uint8_t s[16], back[16];
  for (int i = 0; i < 16; i++) s[i] = (uint8_t)(i * 17);
  char link[200], name[40];
  CHECK(buildChannelLink("Night ops", s, link, sizeof link));
  CHECK(!strcmp(link, "meshcore://channel/add?name=Night%20ops&secret=00112233445566778899aabbccddeeff"));
  CHECK(parseChannelLink(link, name, sizeof name, back) && !strcmp(name, "Night ops") && !memcmp(s, back, 16));
  // As the app writes it (Dart's Uri: '+' for a space), upper-case hex too.
  CHECK(parseChannelLink("meshcore://channel/add?name=Night+ops&secret=00112233445566778899AABBCCDDEEFF", name,
                         sizeof name, back) && !strcmp(name, "Night ops") && !memcmp(s, back, 16));
  CHECK(!parseChannelLink("meshcore://channel/add?name=x&secret=0011", name, sizeof name, back));
  CHECK(!parseChannelLink("meshcore://contact/add?name=x&secret=00112233445566778899aabbccddeeff", name, sizeof name, back));

  uint8_t pub[32], pback[32], type = 0;
  for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(255 - i);
  CHECK(buildContactLink("Jade \xF0\x9F\x8C\xB2", pub, link, sizeof link));
  CHECK(strlen(link) < kMaxBytes);
  CHECK(parseContactLink(link, name, sizeof name, pback, &type) && !strcmp(name, "Jade \xF0\x9F\x8C\xB2") &&
        !memcmp(pub, pback, 32) && type == 1);
  char hex[65], l2[200];
  hexLower(pub, 32, hex);
  snprintf(l2, sizeof l2, "MESHCORE://contact/add?public_key=%s&name=Rpt&type=2", hex);
  CHECK(parseContactLink(l2, name, sizeof name, pback, &type) && !strcmp(name, "Rpt") && type == 2 &&
        !memcmp(pub, pback, 32));
}

static void testBridge() {
  char who[32];
  const char* t = splitSender("Alice: hi there: all", who, sizeof who);
  CHECK(!strcmp(who, "Alice") && !strcmp(t, "hi there: all"));
  t = splitSender("no sender", who, sizeof who);
  CHECK(!who[0] && !strcmp(t, "no sender"));
  CHECK(isBridgePost("[Wavest] Bob: yo") && !isBridgePost("Bob: yo"));
}

// ---- ggwave with the radios' settings ------------------------------------------------------

static GGWave::Parameters params(int op_rate, int spf, int in_rate, int out_rate, int mode) {
  GGWave::Parameters p = GGWave::getDefaultParameters();
  p.payloadLength = -1;
  p.sampleRateInp = (float)in_rate;
  p.sampleRateOut = (float)out_rate;
  p.sampleRate = (float)op_rate;
  p.samplesPerFrame = spf;
  p.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
  p.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;
  p.operatingMode = mode;
  return p;
}

static std::vector<int16_t> synth(const std::string& msg, int proto, int op, int spf, int out) {
  GGWave tx(params(op, spf, out, out, GGWAVE_OPERATING_MODE_TX));
  std::vector<int16_t> wav;
  if (!tx.init((int)msg.size(), msg.data(), (GGWave::ProtocolId)proto, 50)) return wav;
  const int bytes = tx.encode();
  wav.resize((size_t)bytes / 2);
  memcpy(wav.data(), tx.txWaveform(), (size_t)bytes);
  return wav;
}

// What the radio's listener makes of `wav` (16 kHz), fed as the mic delivers it.
static std::vector<std::string> listen(const std::vector<int16_t>& wav16) {
  GGWave::setLogFile(nullptr);
  GGWave::Protocols::rx().disableAll();
  for (int p = GGWAVE_PROTOCOL_AUDIBLE_NORMAL; p <= GGWAVE_PROTOCOL_AUDIBLE_FASTEST; p++)
    GGWave::Protocols::rx().toggle((GGWave::ProtocolId)p, true);
  // As the radio does it: ggwave at 24 kHz throughout, the mic's 16 kHz
  // brought up by soundlink::Resampler in the blocks the mic delivers.
  GGWave rx(params(24000, 512, 24000, 24000, GGWAVE_OPERATING_MODE_RX));
  CHECK(rx.heapSize() < 1600 * 1024);   // PSRAM, while listening
  Resampler up;
  CHECK(up.begin(16000, 24000));
  std::vector<int16_t> all(16000, 0);   // a second of quiet, the clip, another second
  all.insert(all.end(), wav16.begin(), wav16.end());
  all.insert(all.end(), 16000, 0);
  uint32_t seed = 1;
  for (auto& s : all) {   // room noise
    seed = seed * 1103515245u + 12345u;
    s = (int16_t)(s + (int)((seed >> 16) % 801) - 400);
  }
  std::vector<std::string> got;
  std::vector<int16_t> pend;
  int16_t buf[256 * 3 / 2 + 2];
  for (size_t i = 0; i + 256 <= all.size(); i += 256) {   // 16 ms mic reads
    const size_t n = up.process(all.data() + i, 256, buf, sizeof buf / sizeof buf[0]);
    pend.insert(pend.end(), buf, buf + n);
    while (pend.size() >= 1024) {   // whole frames to ggwave
      if (rx.decode(pend.data(), 1024 * sizeof(int16_t))) {
        GGWave::TxRxData data;
        const int got_n = rx.rxTakeData(data);
        if (got_n > 0) got.emplace_back((const char*)data.data(), (size_t)got_n);
      }
      pend.erase(pend.begin(), pend.begin() + 1024);
    }
  }
  return got;
}

static std::vector<int16_t> resample(const std::vector<int16_t>& w, int from, int to) {
  Resampler r;
  CHECK(r.begin(from, to));
  std::vector<int16_t> o(w.size() * (size_t)to / (size_t)from + 4);
  o.resize(r.process(w.data(), w.size(), o.data(), o.size()));
  return o;
}

static void testResampler() {
  // A 6 kHz tone (near the top of the audible protocols) keeps its level 16 -> 24 kHz.
  std::vector<int16_t> t(16000);
  for (size_t i = 0; i < t.size(); i++) t[i] = (int16_t)(10000 * sin(2 * M_PI * 6000 * i / 16000.0));
  std::vector<int16_t> u = resample(t, 16000, 24000);
  CHECK(u.size() >= 23990 && u.size() <= 24004);
  double peak = 0;
  for (size_t i = 200; i < u.size() - 200; i++) peak = fmax(peak, fabs((double)u[i]));
  CHECK(peak > 8500 && peak < 11500);
  Resampler bad;
  CHECK(!bad.begin(44100, 16000));
}

static void testGgwave() {
  std::string longest(kMaxBytes, 'x');
  for (size_t i = 0; i < longest.size(); i++) longest[i] = (char)('a' + i % 26);
  for (int proto = GGWAVE_PROTOCOL_AUDIBLE_NORMAL; proto <= GGWAVE_PROTOCOL_AUDIBLE_FASTEST; proto++) {
    // The radio playing (24 kHz, brought down to the P4's 16 kHz speaker) to a radio.
    std::vector<int16_t> w = resample(synth(longest, proto, 24000, 512, 24000), 24000, 16000);
    CHECK(!w.empty());
    std::vector<std::string> got = listen(w);
    CHECK(got.size() == 1 && got[0] == longest);
    // The app or Wavest playing at 48 kHz, heard by the radio's 16 kHz mic.
    w = synth("U:Jade|hello from the app", proto, 48000, 1024, 48000);
    got = listen(resample(w, 48000, 16000));
    CHECK(got.size() == 1 && got[0] == "U:Jade|hello from the app");
    printf("protocol %d: ok\n", proto);
  }
  // A team setup as the app plays it: two parts, the first a full 138 bytes,
  // 0.7 s apart, at 48 kHz (TeamSetup.soundParts, team_setup_ui.dart).
  {
    const std::string a = "MCT:abcd:1/2:meshcore://team/join?n=Ops%20Team&s=0123456789abcdef0123456789abcdef"
                          "&w=%23icewatch,%23transit,Logistics:fedcba9876543210fe";
    const std::string b = "MCT:abcd:2/2:dcba9876543210&m=910525,62500,7,5";
    CHECK(a.size() <= kMaxBytes);
    for (int proto = GGWAVE_PROTOCOL_AUDIBLE_NORMAL; proto <= GGWAVE_PROTOCOL_AUDIBLE_FASTEST; proto++) {
      std::vector<int16_t> both = synth(a, proto, 48000, 1024, 48000);
      both.insert(both.end(), 48000 * 7 / 10, 0);
      std::vector<int16_t> wb = synth(b, proto, 48000, 1024, 48000);
      both.insert(both.end(), wb.begin(), wb.end());
      std::vector<std::string> got = listen(resample(both, 48000, 16000));
      CHECK(got.size() == 2 && got[0] == a && got[1] == b);
    }
  }
  GGWave tx(params(24000, 512, 24000, 24000, GGWAVE_OPERATING_MODE_TX));
  printf("player %d KB\n", tx.heapSize() / 1024);
  CHECK(tx.heapSize() < 2048 * 1024);   // PSRAM, only while playing
}

int main() {
  testBase64();
  testPlain();
  testAes();
  testXor();
  testLinks();
  testBridge();
  testResampler();
  testGgwave();
  if (g_fail) { printf("%d failed\n", g_fail); return 1; }
  printf("sound link: all passed\n");
  return 0;
}
