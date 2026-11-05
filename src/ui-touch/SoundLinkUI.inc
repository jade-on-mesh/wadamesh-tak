// SPDX-License-Identifier: GPL-3.0-or-later
// Sound link on this radio: channels, contacts and short messages over sound
// (ggwave, lib/ggwave), in Wavest's format, so MeshCore Open TAK and Wavest's
// web page hear it too; and a bridge between what is heard and a channel.
// Included by UITask.cpp after FlockYouUI.inc (it borrows Team's page helpers).
// The rules with no hardware in them are in SoundLinkCore.h; SOUND_LINK.md has
// the user's side.
//
// How the audio runs. ggwave works at 24 kHz with 512-sample frames (46.875 Hz
// bins, the same as the 48 kHz / 1024 the app and Wavest use, so they hear each
// other); the mic and the P4's speaker run at 16 kHz and soundlink::Resampler
// converts, far cheaper than ggwave's own resampler. Listening and playing are
// one at a time (the P4's mic and speaker share one lock, and nobody wants the
// radio decoding itself): the loop task stops the mic, plays, then starts it
// again. The mic is started and stopped on the loop task (the T-Deck's ADC is
// on the I2C bus); decoding and playing run on the audio worker (VoxMic.h).

#include "SoundLinkCore.h"

#if CAP_SOUND_LINK

#define GGWAVE_MAX_RECORDED_FRAMES 640   // as lib/ggwave/library.json
#include "ggwave/ggwave.h"

#include <new>

#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

namespace soundui {

constexpr int kOpRate = 24000;              // ggwave inside
constexpr int kSpf = 512;
constexpr int kMicRate = 16000;
constexpr size_t kNeedPsram = 1800 * 1024;   // one ggwave instance, listening or playing
constexpr int kHeardMax = 16;
constexpr int kQueueMax = 4;

// ---- settings ----------------------------------------------------------------------------
struct Cfg {
  char callsign[32] = "";
  char passkey[64] = "";
  uint8_t proto = 1;     // 0 Normal, 1 Fast (Wavest's default), 2 Fastest
  uint8_t volume = 50;   // ggwave's, 0..100; the speaker follows Settings > Sound too
  bool listen = false;
  char bridge[32] = "";  // the channel heard messages go to / are played from
  bool forward = false;  // post what is heard there
  bool play = false;     // play that channel's new messages
};
static Cfg s_cfg;
static bool s_cfg_loaded = false;

static void saveCfg() {
  char buf[320];
  const int n = snprintf(buf, sizeof buf, "callsign=%s\npasskey=%s\nproto=%u\nvolume=%u\nlisten=%d\nbridge=%s\nforward=%d\nplay=%d\n",
                         s_cfg.callsign, s_cfg.passkey, s_cfg.proto, s_cfg.volume, s_cfg.listen ? 1 : 0, s_cfg.bridge,
                         s_cfg.forward ? 1 : 0, s_cfg.play ? 1 : 0);
  if (n > 0) teamui::s_io.writeFile("soundlink", (const uint8_t*)buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static void loadCfg() {
  if (s_cfg_loaded) return;
  s_cfg_loaded = true;
  char buf[320];
  size_t n = 0;
  if (!teamui::s_io.readFile("soundlink", (uint8_t*)buf, sizeof buf - 1, &n)) return;
  buf[n] = 0;
  for (char* line = strtok(buf, "\n"); line; line = strtok(nullptr, "\n")) {
    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    const char* v = eq + 1;
    if (!strcmp(line, "callsign")) snprintf(s_cfg.callsign, sizeof s_cfg.callsign, "%s", v);
    else if (!strcmp(line, "passkey")) snprintf(s_cfg.passkey, sizeof s_cfg.passkey, "%s", v);
    else if (!strcmp(line, "proto")) s_cfg.proto = (uint8_t)(atoi(v) % 3);
    else if (!strcmp(line, "volume")) s_cfg.volume = (uint8_t)constrain(atoi(v), 5, 100);
    else if (!strcmp(line, "listen")) s_cfg.listen = atoi(v) != 0;
    else if (!strcmp(line, "bridge")) snprintf(s_cfg.bridge, sizeof s_cfg.bridge, "%s", v);
    else if (!strcmp(line, "forward")) s_cfg.forward = atoi(v) != 0;
    else if (!strcmp(line, "play")) s_cfg.play = atoi(v) != 0;
  }
  if (bgListenerResetOnce("sound") && s_cfg.listen) { s_cfg.listen = false; saveCfg(); }
}

static const char* myCallsign() {
  if (s_cfg.callsign[0]) return s_cfg.callsign;
  const char* n = the_mesh.getNodePrefs()->node_name;
  return n && *n ? n : "radio";
}

static void toast(const char* m, int ms = 2200) { if (g_lv.task) g_lv.task->showAlert(m, ms); }

// ---- what was heard --------------------------------------------------------------------------
enum class What : uint8_t { Message, Locked, Channel, Contact, Raw, Sent, PadCheck };
struct Entry {
  What what;
  char sender[32];
  char text[160];
  char name[32];      // a link's channel or contact name
  uint8_t key[32];    // its secret (16) or public key (32)
  uint8_t type;       // a contact's type
  bool added;
};
static Entry* s_heard = nullptr;   // PSRAM ring, newest at (s_heard_head - 1)
static int s_heard_n = 0, s_heard_head = 0;
static uint32_t s_ver = 0;          // anything the page shows changed

static Entry* newEntry() {
  if (!s_heard) {
    s_heard = (Entry*)heap_caps_calloc(kHeardMax, sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_heard) return nullptr;
  }
  Entry* e = &s_heard[s_heard_head];
  memset(e, 0, sizeof *e);
  s_heard_head = (s_heard_head + 1) % kHeardMax;
  if (s_heard_n < kHeardMax) s_heard_n++;
  s_ver++;
  return e;
}
static Entry* entryAt(int i) {   // 0 = newest
  return s_heard && i < s_heard_n ? &s_heard[(s_heard_head - 1 - i + 2 * kHeardMax) % kHeardMax] : nullptr;
}

// ---- Wavest's encryption -------------------------------------------------------------------
static bool keyFor(const char* passkey, uint8_t key[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info && mbedtls_md(info, (const unsigned char*)passkey, strlen(passkey), key) == 0;
}

static bool aesOpen(const char* b64, char* out, size_t cap) {
  uint8_t key[32], iv[12], tag[16], ct[soundlink::kMaxBytes];
  size_t n = 0;
  if (!s_cfg.passkey[0] || !soundlink::aesSplit(b64, iv, ct, sizeof ct, &n, tag) || n + 1 > cap ||
      !keyFor(s_cfg.passkey, key))
    return false;
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
            mbedtls_gcm_auth_decrypt(&gcm, n, iv, 12, nullptr, 0, tag, 16, ct, (uint8_t*)out) == 0;
  mbedtls_gcm_free(&gcm);
  out[ok ? n : 0] = 0;
  return ok;
}

// The message to play for `text`: plain, or sealed when a passkey is set. 0 when it can't.
static size_t buildMessage(const char* text, char* out, size_t cap) {
  if (!s_cfg.passkey[0]) return soundlink::buildPlain(myCallsign(), text, out, cap);
  char clip[soundlink::kMaxBytes];
  const size_t n = soundlink::copyUtf8(clip, sizeof clip, text, soundlink::aesMaxText(myCallsign()));
  uint8_t key[32], sealed[soundlink::kMaxBytes];
  if (!n || !keyFor(s_cfg.passkey, key)) return 0;
  esp_fill_random(sealed, 12);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
                  mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, n, sealed, 12, nullptr, 0, (const uint8_t*)clip,
                                            sealed + 12, 16, sealed + 12 + n) == 0;
  mbedtls_gcm_free(&gcm);
  return ok ? soundlink::buildAes(myCallsign(), sealed, n + 28, out, cap) : 0;
}

// ---- the audio engine ------------------------------------------------------------------------
enum class St : uint8_t { Idle, Rx, StoppingRx, Tx };
static St s_st = St::Idle;
static volatile bool s_job_stop = false;
static volatile bool s_job_done = true;
static const char* volatile s_job_err = nullptr;
static volatile int s_level = 0;          // mic peak, 0..100
static char s_err[96] = "";               // why listening or playing failed (shown on the page)
static uint32_t s_rx_retry_at = 8000;
static uint32_t s_listen_until = 0;        // a short listen asked for elsewhere (OTP pad check)
static bool wantListen() {
  return s_cfg.listen || (s_listen_until && (int32_t)(millis() - s_listen_until) < 0);
}   // listening resumes a few seconds after boot

struct Out { char msg[soundlink::kMaxBytes + 1]; uint8_t proto; uint8_t volume; };
static Out s_queue[kQueueMax];
static int s_queue_n = 0;
static Out s_tx;                          // the one playing (the worker reads it)

// Heard on the worker, handled on the loop task.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_inbox[2][soundlink::kMaxBytes + 1];
static volatile int s_inbox_n = 0;

static GGWave::Parameters params(int mode) {
  GGWave::Parameters p = GGWave::getDefaultParameters();
  p.payloadLength = -1;
  p.sampleRateInp = kOpRate;
  p.sampleRateOut = kOpRate;
  p.sampleRate = kOpRate;
  p.samplesPerFrame = kSpf;
  p.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
  p.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;
  p.operatingMode = mode;
  return p;
}

static bool psramOk() { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= kNeedPsram; }

// The mic is read by a task of its own into a ring of 4 s (PSRAM), and ggwave
// decodes from the ring. ggwave's work at the end of a message takes ~2 s on a
// T-Deck, and the listener used to read the mic itself: all that while nothing
// read the I2S, its buffer overflowed, and the gap after a message and the start
// of the next (a team setup's second part, 0.7 s later) were lost.
static constexpr uint32_t kRingSamples = 1u << 16;   // 4.1 s at 16 kHz
static int16_t* s_ring = nullptr;
static volatile uint32_t s_ring_w = 0, s_ring_r = 0;
static volatile bool s_reader_stop = false, s_reader_done = true;
static const char* volatile s_reader_err = nullptr;

static void micReaderTask(void*) {
  int16_t mic[256];
  while (!s_reader_stop) {
    size_t n = 0;
    if (!VoxMic::rawRead(mic, 256, &n, 300)) { s_reader_err = "The microphone stopped"; break; }
    if (!n) continue;
    int pk = 0;
    for (size_t i = 0; i < n; i++) pk = max(pk, abs((int)mic[i]));
    const int lv = pk >= 16000 ? 100 : pk * 100 / 16000;
    if (lv > s_level) s_level = lv;
    const uint32_t w = s_ring_w;
    const uint32_t r = __atomic_load_n(&s_ring_r, __ATOMIC_ACQUIRE);
    if (w - r + (uint32_t)n > kRingSamples) continue;   // the decoder is 4 s behind: drop
    for (size_t i = 0; i < n; i++) s_ring[(w + (uint32_t)i) & (kRingSamples - 1)] = mic[i];
    __atomic_store_n(&s_ring_w, w + (uint32_t)n, __ATOMIC_RELEASE);
  }
  s_reader_done = true;
  vTaskDelete(nullptr);
}

static void rxJob(void*) {
  GGWave::setLogFile(nullptr);
  GGWave::Protocols::rx().disableAll();   // the audible ones only: GGWAVE_MAX_RECORDED_FRAMES is sized for them
  for (int p = GGWAVE_PROTOCOL_AUDIBLE_NORMAL; p <= GGWAVE_PROTOCOL_AUDIBLE_FASTEST; p++)
    GGWave::Protocols::rx().toggle((GGWave::ProtocolId)p, true);
  GGWave* g = new (std::nothrow) GGWave(params(GGWAVE_OPERATING_MODE_RX));
  soundlink::Resampler* up = new (std::nothrow) soundlink::Resampler();
  int16_t* frame = (int16_t*)heap_caps_malloc(kSpf * 2 * sizeof(int16_t), MALLOC_CAP_8BIT);
  if (!s_ring) s_ring = (int16_t*)heap_caps_malloc(kRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_ring_w = s_ring_r = 0;
  s_reader_err = nullptr;
  s_reader_stop = false;
  s_reader_done = false;
  bool reader = s_ring && xTaskCreate(micReaderTask, "snd_mic", 6144, nullptr, 7, nullptr) == pdPASS;
  if (!reader) s_reader_done = true;
  if (!g || !up || !frame || !reader || g->heapSize() == 0 || !up->begin(kMicRate, kOpRate)) {
    s_job_err = "Not enough memory to listen";
  } else {
    int16_t mic[256];
    int16_t hi[256 * 3 / 2 + 4];
    int have = 0;
    while (!s_job_stop) {
      if (s_reader_err) { s_job_err = s_reader_err; break; }
      const uint32_t r = s_ring_r;
      const uint32_t avail = __atomic_load_n(&s_ring_w, __ATOMIC_ACQUIRE) - r;
      if (!avail) { vTaskDelay(pdMS_TO_TICKS(8)); continue; }
      const size_t n = avail < 256 ? avail : 256;
      for (size_t i = 0; i < n; i++) mic[i] = s_ring[(r + (uint32_t)i) & (kRingSamples - 1)];
      __atomic_store_n(&s_ring_r, r + (uint32_t)n, __ATOMIC_RELEASE);
      const size_t m = up->process(mic, n, hi, sizeof hi / sizeof hi[0]);
      for (size_t i = 0; i < m; i++) {
        frame[have++] = hi[i];
        if (have < kSpf * 2) continue;   // ggwave wants whole frames
        have = 0;
        if (!g->decode(frame, kSpf * 2 * sizeof(int16_t))) continue;
        GGWave::TxRxData data;
        const int len = g->rxTakeData(data);
        if (len <= 0) continue;
        portENTER_CRITICAL(&s_mux);
        if (s_inbox_n < 2) {
          const int k = len < (int)soundlink::kMaxBytes ? len : (int)soundlink::kMaxBytes;
          memcpy(s_inbox[s_inbox_n], data.data(), (size_t)k);
          s_inbox[s_inbox_n][k] = 0;
          s_inbox_n = s_inbox_n + 1;
        }
        portEXIT_CRITICAL(&s_mux);
      }
    }
  }
  s_reader_stop = true;   // before the mic is stopped (the loop task does that after this job)
  for (int i = 0; i < 200 && !s_reader_done; i++) vTaskDelay(pdMS_TO_TICKS(10));
  free(frame);
  delete up;
  delete g;
  Serial.printf("[SOUND] listener stack: %u bytes unused\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  s_job_done = true;
}

static void txJob(void*) {
  GGWave::setLogFile(nullptr);
  GGWave* g = new (std::nothrow) GGWave(params(GGWAVE_OPERATING_MODE_TX));
  const char* err = nullptr;
  if (!g || g->heapSize() == 0) err = "Not enough memory to play";
  else if (!g->init((int)strlen(s_tx.msg), s_tx.msg, (GGWave::ProtocolId)s_tx.proto, s_tx.volume)) err = "Couldn't encode";
  uint32_t bytes = err ? 0 : g->encode();
  if (!err && !bytes) err = "Couldn't encode";
  LuaAudioSinkSession sink;
  if (!err && !luaAudioSinkBegin(kOpRate, &sink, &err)) {
    if (!err || !strcmp(err, "muted")) err = "Sound is off - turn it on in Settings > Sound";
  }
  if (!err) {
    const int16_t* w = (const int16_t*)g->txWaveform();
    const size_t n = bytes / sizeof(int16_t);
    soundlink::Resampler down;
    const bool same = sink.output_rate == (uint32_t)kOpRate;
    if (!same && !down.begin(kOpRate, (int)sink.output_rate)) err = "Unsupported speaker rate";
    int16_t out[400];
    for (size_t i = 0; !err && i < n && !s_job_stop; i += 256) {
      const size_t k = n - i < 256 ? n - i : 256;
      size_t m;
      if (same) { memcpy(out, w + i, k * sizeof(int16_t)); m = k; }
      else m = down.process(w + i, k, out, sizeof out / sizeof out[0]);
      for (size_t j = 0; j < m; j++) {
        int32_t v = (int32_t)((float)out[j] * sink.gain);
        out[j] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
      }
      if (m && !luaAudioSinkWrite(&sink, out, m)) err = "The speaker stopped";
    }
    luaAudioSinkEnd(&sink);
  }
  delete g;
  s_job_err = err;
  __atomic_store_n(&s_lua_audio_active, false, __ATOMIC_RELEASE);
  s_job_done = true;
}
static void jobTask(void* fn) {   // when the audio worker is busy or missing
  ((void (*)(void*))fn)(nullptr);
  vTaskDelete(nullptr);
}
static bool runJob(void (*fn)(void*)) {
  s_job_stop = false;
  s_job_err = nullptr;
  s_job_done = false;
  if (VoxAudioTask::run(fn, nullptr)) return true;
  if (xTaskCreate(jobTask, "sound_link", 16 * 1024, (void*)fn, 3, nullptr) == pdPASS) return true;
  s_job_done = true;
  return false;
}

static void setErr(const char* e) {
  snprintf(s_err, sizeof s_err, "%s", e ? e : "");
  s_ver++;
}

static bool startRx() {
  const char* err = nullptr;
  if (!psramOk()) { setErr("Not enough memory to listen"); return false; }
  if (VoxMic::state() == VoxMic::kRecording) return false;   // a voice message first; try again later
  if (!VoxMic::rawStart(&err)) { setErr(err); return false; }
  if (!runJob(rxJob)) { VoxMic::rawStop(); setErr("Not enough memory to listen"); return false; }
  setErr("");
  s_st = St::Rx;
  return true;
}

static bool startTx() {
  if (luaAudioActive() || s_notify_playing) return false;   // a clip or a chime: wait
  if (!psramOk()) {
    setErr("Not enough memory to play");
    s_queue_n = 0;
    return false;
  }
  s_tx = s_queue[0];
  memmove(s_queue, s_queue + 1, sizeof(Out) * (size_t)(--s_queue_n));
  __atomic_store_n(&s_lua_audio_active, true, __ATOMIC_RELEASE);   // chimes off the speaker meanwhile
  if (!runJob(txJob)) {
    __atomic_store_n(&s_lua_audio_active, false, __ATOMIC_RELEASE);
    setErr("Not enough memory to play");
    return false;
  }
  s_st = St::Tx;
  s_ver++;
  return true;
}

// Queues `msg` (already in its final shape) to play.
static bool enqueue(const char* msg) {
  if (!msg || !*msg) return false;
  if (s_queue_n >= kQueueMax) { toast(TR("Sound link: still playing, try again")); return false; }
  Out& o = s_queue[s_queue_n++];
  soundlink::copyUtf8(o.msg, sizeof o.msg, msg, soundlink::kMaxBytes);
  o.proto = s_cfg.proto;
  o.volume = s_cfg.volume;
  return true;
}

// For other screens: play `msg` as it is, and listen for a while.
static bool playRaw(const char* msg) {
  loadCfg();
  if (strlen(msg) > soundlink::kMaxBytes) return false;
  return enqueue(msg);
}
static void listenFor(uint32_t ms) {
  loadCfg();
  s_listen_until = millis() + ms;
  if (!s_listen_until) s_listen_until = 1;
  s_rx_retry_at = millis();
  setErr("");
}

static bool playing() { return s_st == St::Tx || s_queue_n > 0; }
static bool listening() { return s_st == St::Rx; }

// ---- the bridge ------------------------------------------------------------------------------
static bool bridgeChannel(int* idx, ChannelDetails* cd) {
  if (!s_cfg.bridge[0]) return false;
  const int i = teamui::Io::channelIdx(s_cfg.bridge);
  if (i < 0 || !the_mesh.getChannel(i, *cd) || strcmp(cd->name, s_cfg.bridge) != 0) return false;
  *idx = i;
  return true;
}

static bool channelUsable(const char* name) {
  return teamui::Io::channelIdx(name) >= 0 && strcasecmp(name, "public") != 0 && !teamui::threadIsPublicChannel(name);
}

static void forward(const char* sender, const char* text) {
  int idx = -1;
  ChannelDetails cd;
  if (!s_cfg.forward || !bridgeChannel(&idx, &cd)) return;
  char body[200];
  snprintf(body, sizeof body, "%s%s: %s", soundlink::kBridgeTag, sender && *sender ? sender : "?", text);
  char clip[150];
  soundlink::copyUtf8(clip, sizeof clip, body, 140);
  const char* me = the_mesh.getNodePrefs()->node_name;
  const uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  if (!sendGroupMessageScoped(idx, cd, ts, me && *me ? me : "me", clip)) { setErr("Couldn't post to the channel"); return; }
  if (g_lv.task) g_lv.task->appSentMsgToChannel(cd.name, clip, the_mesh.uiLastSentFp());
}

static void onChannelText(int idx, const char* text) {
  loadCfg();
  int bidx = -1;
  ChannelDetails cd;
  if (!s_cfg.play || !text || !bridgeChannel(&bidx, &cd) || bidx != idx) return;
  char sender[32];
  const char* body = soundlink::splitSender(text, sender, sizeof sender);
  if (soundlink::isBridgePost(body) || !*body) return;   // never play the bridge's own posts back
  char msg[soundlink::kMaxBytes + 1];
  if (soundlink::buildPlain(sender[0] ? sender : cd.name, body, msg, sizeof msg)) enqueue(msg);
}

// ---- handling what was heard -------------------------------------------------------------------
static bool s_page_open = false;

static teamsetup::Assembler* s_team_parts = nullptr;   // a team setup played in parts (TEAM_SETUP.md)

static void handleHeard(const char* s) {
  Serial.printf("[SOUND] heard %u bytes\n", (unsigned)strlen(s));
  if (teamsetup::isPart(s)) {   // a team setup, whole or a part: asks to join once it's all here
    if (!s_team_parts) s_team_parts = new (std::nothrow) teamsetup::Assembler();
    teamsetup::Setup* st = new (std::nothrow) teamsetup::Setup();
    if (s_team_parts && st && s_team_parts->accept(s, *st)) {
      s_listen_until = 0;   // a short listen ends with what it was for
      teamSetupHeard(*st);
    } else if (s_team_parts) {
      char t[64];
      snprintf(t, sizeof t, TR("Team setup: heard %d of %d"), s_team_parts->got(), s_team_parts->of());
      toast(t, 1500);
    }
    delete st;
    return;
  }
  Entry* e = newEntry();
  if (!e) return;
  uint8_t key[32];
  char name[64];
  uint8_t type = 1;
  uint32_t pad_n = 0;
  if (otp::parsePadCheck(s, key, &pad_n)) {   // OTP Messenger's pad check code
    e->what = What::PadCheck;
    otpui::onPadCheckHeard(key, pad_n, e->text, sizeof e->text);
    s_listen_until = 0;   // a short listen ends with what it was for
    return;               // onPadCheckHeard said how it went
  } else if (soundlink::parseChannelLink(s, name, sizeof name, key)) {
    e->what = What::Channel;
    soundlink::copyUtf8(e->name, sizeof e->name, name[0] ? name : "Channel", 31);
    memcpy(e->key, key, 16);
    snprintf(e->text, sizeof e->text, "%s %s", TR("Channel:"), e->name);
  } else if (soundlink::parseContactLink(s, name, sizeof name, key, &type)) {
    e->what = What::Contact;
    soundlink::copyUtf8(e->name, sizeof e->name, name[0] ? name : "Contact", 31);
    memcpy(e->key, key, 32);
    e->type = type;
    snprintf(e->text, sizeof e->text, "%s %s", TR("Contact:"), e->name);
  } else {
    const soundlink::Heard h = soundlink::parse(s);
    snprintf(e->sender, sizeof e->sender, "%s", h.sender);
    switch (h.kind) {
      case soundlink::Kind::Plain:
        e->what = What::Message;
        soundlink::copyUtf8(e->text, sizeof e->text, h.body, sizeof e->text - 1);
        break;
      case soundlink::Kind::Aes:
        e->what = aesOpen(h.body, e->text, sizeof e->text) ? What::Message : What::Locked;
        break;
      case soundlink::Kind::Xor:
        e->what = s_cfg.passkey[0] && soundlink::xorOpen(h.body, s_cfg.passkey, e->text, sizeof e->text)
                      ? What::Message : What::Locked;
        break;
      case soundlink::Kind::Raw:
        e->what = What::Raw;
        soundlink::copyUtf8(e->text, sizeof e->text, s, sizeof e->text - 1);
        break;
    }
    if (e->what == What::Locked) snprintf(e->text, sizeof e->text, "%s", TR("(encrypted: set the passkey)"));
    if (e->what == What::Message) forward(e->sender, e->text);
  }
  if (!s_page_open) {
    char t[120];
    snprintf(t, sizeof t, "%s %s", TR("Sound link:"), e->text);
    toast(t, 3000);
  }
  if (g_lv.task && !g_lv.task->isBuzzerQuiet()) uiPlaySlot(TOUCH_SND_MEN);
}

static void loopTick() {
  loadCfg();
  if (s_inbox_n) {
    char got[2][soundlink::kMaxBytes + 1];
    int n;
    portENTER_CRITICAL(&s_mux);
    n = s_inbox_n;
    memcpy(got, s_inbox, sizeof got);
    s_inbox_n = 0;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) handleHeard(got[i]);
  }
  const uint32_t now = millis();
  switch (s_st) {
    case St::Idle:
      if (s_queue_n) startTx();
      else if (wantListen() && (int32_t)(now - s_rx_retry_at) >= 0 && !startRx()) s_rx_retry_at = now + 5000;
      break;
    case St::Rx:
      if (s_job_done) {   // ended by itself: the mic or memory
        VoxMic::rawStop();
        setErr(s_job_err ? s_job_err : "Stopped");
        s_st = St::Idle;
        s_rx_retry_at = now + 5000;
      } else if (s_queue_n || !wantListen()) {
        s_job_stop = true;
        s_st = St::StoppingRx;
      }
      break;
    case St::StoppingRx:
      if (s_job_done) {
        VoxMic::rawStop();
        s_st = St::Idle;
        s_ver++;
      }
      break;
    case St::Tx:
      if (s_job_done) {
        if (s_job_err) { setErr(s_job_err); s_queue_n = 0; }
        s_st = St::Idle;
        s_rx_retry_at = now + 300;   // let the room go quiet before listening again
        s_ver++;
      }
      break;
  }
}

// ---- actions -----------------------------------------------------------------------------------
static void sendText(const char* text) {
  char msg[soundlink::kMaxBytes + 1];
  if (!text || !*text) return;
  if (!buildMessage(text, msg, sizeof msg)) { toast(TR("Couldn't make the message")); return; }
  if (!enqueue(msg)) return;
  Entry* e = newEntry();
  if (e) {
    e->what = What::Sent;
    snprintf(e->sender, sizeof e->sender, "%s", myCallsign());
    soundlink::copyUtf8(e->text, sizeof e->text, text, sizeof e->text - 1);
  }
}

static void playMyContact() {
  const char* n = g_lv.task ? g_lv.task->getNodeNameCstr() : "";
  char link[soundlink::kMaxBytes + 40];
  if (!soundlink::buildContactLink(n && *n ? n : "node", the_mesh.getSelfPubKey(), link, sizeof link) ||
      strlen(link) > soundlink::kMaxBytes) {
    toast(TR("Name too long to play"));
    return;
  }
  if (enqueue(link)) toast(TR("Playing your contact"), 1500);
}

static void playChannel(int idx) {
  ChannelDetails cd;
  char link[soundlink::kMaxBytes + 40];
  if (!the_mesh.getChannel(idx, cd) || !cd.name[0]) return;
  if (!soundlink::buildChannelLink(cd.name, cd.channel.secret, link, sizeof link) || strlen(link) > soundlink::kMaxBytes) {
    toast(TR("Name too long to play"));
    return;
  }
  if (enqueue(link)) toast(TR("Playing the channel"), 1500);
}

static void addFrom(Entry* e) {
  if (!e || e->added) return;
  if (e->what == What::Channel) {
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails cd;
      if (the_mesh.getChannel(i, cd) && cd.name[0] && !memcmp(cd.channel.secret, e->key, 16)) {
        toast(TR("You have this channel already"));
        e->added = true;
        return;
      }
    }
    const int slot = the_mesh.findFirstEmptyChannelSlot();
    if (slot < 0) { toast(TR("Channel table is full.")); return; }
    if (!the_mesh.uiAddOrUpdateChannel(slot, e->name, e->key)) { toast(TR("Failed to save channel.")); return; }
    if (g_lv.task) { g_lv.task->refreshThreadsFromMesh(); g_lv.dirty_threads = true; }
    toast(TR("Channel joined"), 1500);
  } else if (e->what == What::Contact) {
    if (!memcmp(e->key, the_mesh.getSelfPubKey(), 32)) { toast(TR("That's this radio")); return; }
    if (!the_mesh.uiAddDiscoveredContact(e->key, e->type, e->name)) { toast(TR("Already a contact, or the list is full")); return; }
    toast(TR("Contact added"), 1500);
  } else {
    return;
  }
  e->added = true;
  s_ver++;
}

// ---- the page ----------------------------------------------------------------------------------
static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_body = nullptr;
static lv_obj_t* s_meter = nullptr;
static int s_tab_i = 0;   // 0 link, 1 settings
static lv_timer_t* s_page_timer = nullptr;
static bool s_pick_channel = false;   // the channel picker for "Play a channel" is open
static void pageBuild();

using teamui::lbl;
using teamui::rowCard;

static lv_obj_t* wideBtn(lv_obj_t* b, const char* t, lv_event_cb_t cb, void* ud = nullptr, uint32_t bg = 0) {
  lv_obj_t* x = lv_btn_create(b);
  styleButton(x);
  lv_obj_set_size(x, LV_PCT(100), 32);
  if (bg) teamui::fill(x, bg);
  lv_obj_add_event_cb(x, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* l = lv_label_create(x);
  lv_label_set_text(l, t);
  useChainedFont(l);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  return x;
}

static lv_obj_t* chipRow(lv_obj_t* b) {
  lv_obj_t* row = lv_obj_create(b);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(row, 6, LV_PART_MAIN);
  return row;
}
static lv_obj_t* chip(lv_obj_t* row, const char* t, lv_event_cb_t cb, void* ud, bool sel) {
  lv_obj_t* x = lv_btn_create(row);
  styleButton(x);
  lv_obj_set_size(x, LV_SIZE_CONTENT, 30);
  lv_obj_set_style_pad_hor(x, 10, LV_PART_MAIN);
  if (sel) teamui::selected(x);
  lv_obj_add_event_cb(x, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* l = lv_label_create(x);
  lv_label_set_text(l, t);
  useChainedFont(l);
  lv_obj_center(l);
  return x;
}

static void statusLine(char* out, size_t cap) {
  if (s_st == St::Tx || s_queue_n) snprintf(out, cap, "%s", TR("Playing..."));
  else if (listening()) snprintf(out, cap, "%s", TR("Listening. Hold the other device near the mic."));
  else if (wantListen()) snprintf(out, cap, "%s", TR("Starting the microphone..."));
  else snprintf(out, cap, "%s", TR("Not listening."));
}

static void buildLink(lv_obj_t* b) {
  char line[200];
  statusLine(line, sizeof line);
  lbl(b, line, &g_font_12, COLOR_SUB);
  s_meter = lv_bar_create(b);
  lv_obj_set_size(s_meter, LV_PCT(100), 6);
  lv_bar_set_range(s_meter, 0, 100);
  if (!listening()) lv_obj_add_flag(s_meter, LV_OBJ_FLAG_HIDDEN);
  if (s_err[0]) lbl(b, s_err, &g_font_12, 0xF2A33A);
  wideBtn(b, s_cfg.listen ? TR("Stop listening") : TR("Start listening"), [](lv_event_t*) {
    s_cfg.listen = !s_cfg.listen;
    s_rx_retry_at = millis();
    if (s_cfg.listen) setErr("");
    saveCfg();
    pageBuild();
  }, nullptr, s_cfg.listen ? 0x7F1D1D : 0x2E7D32);
  wideBtn(b, TR("Send a message"), [](lv_event_t*) {
    fmTextPrompt(TR("Message to play"), "", [](const char* t) { sendText(t); pageBuild(); });
  });
  wideBtn(b, TR("Play my contact"), [](lv_event_t*) { playMyContact(); pageBuild(); });
  wideBtn(b, TR("Play a channel"), [](lv_event_t*) { s_pick_channel = !s_pick_channel; pageBuild(); });
  if (s_pick_channel) {
    lv_obj_t* row = chipRow(b);
    int shown = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails cd;
      if (!the_mesh.getChannel(i, cd) || !cd.name[0]) continue;
      chip(row, cd.name, [](lv_event_t* e) {
        s_pick_channel = false;
        playChannel((int)(intptr_t)lv_event_get_user_data(e));
        pageBuild();
      }, (void*)(intptr_t)i, false);
      shown++;
    }
    if (!shown) lbl(row, TR("No channels yet."), &g_font_12, COLOR_SUB);
    lbl(b, TR("Anyone who hears it can join the channel."), &g_font_12, 0xF2A33A);
  }
  if (!s_heard_n) {
    lbl(b, TR("Nothing heard yet. MeshCore Open TAK plays channels and contacts from their share screens; "
              "Wavest's web page sends messages."), &g_font_12, COLOR_SUB);
    return;
  }
  for (int i = 0; i < s_heard_n; i++) {
    Entry* e = entryAt(i);
    if (!e) break;
    const bool link = e->what == What::Channel || e->what == What::Contact;
    lv_obj_t* card = rowCard(b, [](lv_event_t* ev) {
      addFrom(entryAt((int)(intptr_t)lv_event_get_user_data(ev)));
      pageBuild();
    }, (void*)(intptr_t)i);
    if (link) {
      lv_obj_set_style_border_color(card, lv_color_hex(0x4FC3F7), LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    }
    if (e->sender[0]) {
      snprintf(line, sizeof line, "%s%s", e->what == What::Sent ? "\xE2\x86\x91 " : "", e->sender);
      lbl(card, line, &g_font_12, e->what == What::Sent ? COLOR_SUB : 0x81C784);
    }
    lbl(card, e->text, &g_font_14, e->what == What::Locked || e->what == What::Raw ? COLOR_SUB : COLOR_TEXT);
    if (link)
      lbl(card, e->added ? TR("Added") : e->what == What::Channel ? TR("Tap to join") : TR("Tap to add"),
          &g_font_12, e->added ? COLOR_SUB : 0x4FC3F7);
  }
}

static void settingsTap(lv_event_t* e) {
  switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case 0:
      fmTextPrompt(TR("Callsign (empty: the radio's name)"), s_cfg.callsign, [](const char* t) {
        soundlink::callsign(t, s_cfg.callsign, sizeof s_cfg.callsign);
        saveCfg();
        pageBuild();
      });
      s_fm_prompt_allow_empty = true;
      return;
    case 1:
      fmTextPrompt(TR("Wavest passkey (empty: none)"), s_cfg.passkey, [](const char* t) {
        snprintf(s_cfg.passkey, sizeof s_cfg.passkey, "%s", t ? t : "");
        saveCfg();
        pageBuild();
      });
      s_fm_prompt_allow_empty = true;
      return;
    case 2: s_cfg.proto = (uint8_t)((s_cfg.proto + 1) % 3); break;
    case 3: s_cfg.volume = (uint8_t)(s_cfg.volume >= 100 ? 25 : s_cfg.volume + 25); break;
    case 4:
      if (!s_cfg.bridge[0]) { toast(TR("Pick the channel first")); return; }
      s_cfg.forward = !s_cfg.forward;
      break;
    case 5:
      if (!s_cfg.bridge[0]) { toast(TR("Pick the channel first")); return; }
      s_cfg.play = !s_cfg.play;
      break;
  }
  saveCfg();
  pageBuild();
}

static void buildSettings(lv_obj_t* b) {
  char line[140];
  auto onoff = [](bool v) { return v ? TR("on") : TR("off"); };
  static const char* const protos[3] = {"Normal", "Fast", "Fastest"};
  snprintf(line, sizeof line, "%s: %s", TR("Callsign"), myCallsign());
  wideBtn(b, line, settingsTap, (void*)0);
  snprintf(line, sizeof line, "%s: %s", TR("Passkey"), s_cfg.passkey[0] ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : TR("none"));
  wideBtn(b, line, settingsTap, (void*)1);
  snprintf(line, sizeof line, "%s: %s", TR("Speed"), TR(protos[s_cfg.proto % 3]));
  wideBtn(b, line, settingsTap, (void*)2);
  snprintf(line, sizeof line, "%s: %u%%", TR("Loudness"), s_cfg.volume);
  wideBtn(b, line, settingsTap, (void*)3);
  lbl(b, TR("A passkey encrypts your messages the way Wavest does (AES-256-GCM, key = SHA-256 of the passkey). "
            "Wavest-compatible, not strong: never reuse a mesh key or password here."),
      &g_font_12, COLOR_SUB);

  lbl(b, TR("Bridge channel (never Public):"), &g_font_12, COLOR_SUB);
  lv_obj_t* row = chipRow(b);
  int shown = 0;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(i, cd) || !cd.name[0] || !channelUsable(cd.name)) continue;
    chip(row, cd.name, [](lv_event_t* e) {
      ChannelDetails d;
      if (!the_mesh.getChannel((int)(intptr_t)lv_event_get_user_data(e), d)) return;
      if (!strcmp(d.name, s_cfg.bridge)) { s_cfg.bridge[0] = 0; s_cfg.forward = s_cfg.play = false; }
      else snprintf(s_cfg.bridge, sizeof s_cfg.bridge, "%s", d.name);
      saveCfg();
      pageBuild();
    }, (void*)(intptr_t)i, !strcmp(cd.name, s_cfg.bridge));
    shown++;
  }
  if (!shown) lbl(row, TR("No channel to bridge. Add a private or hashtag channel in Chats first."), &g_font_12, 0xF2A33A);
  snprintf(line, sizeof line, "%s: %s", TR("Post what is heard there"), onoff(s_cfg.forward));
  wideBtn(b, line, settingsTap, (void*)4, s_cfg.forward ? 0x2E7D32 : 0);
  snprintf(line, sizeof line, "%s: %s", TR("Play its new messages aloud"), onoff(s_cfg.play));
  wideBtn(b, line, settingsTap, (void*)5, s_cfg.play ? 0x2E7D32 : 0);
  lbl(b, TR("With both on, a Wavest web page in the room and the mesh channel talk to each other through this radio. "
            "Posts start with [Wavest] and are never played back."),
      &g_font_12, COLOR_SUB);
  lbl(b, TR("Sound by ggwave (Georgi Gerganov, MIT). Message format from Wavest by Benn Jordan."), &g_font_12, COLOR_SUB);
}

static void pageClose();
static void pageBack() { pageClose(); }

static void pageBuild() {
  if (!s_root) return;
  if (s_body) { lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN); lv_obj_del_async(s_body); }
  s_meter = nullptr;
  s_body = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_body);
  lv_obj_set_size(s_body, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(s_body, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_body, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
  lv_obj_t* tabs = lv_obj_create(s_body);
  lv_obj_remove_style_all(tabs);
  lv_obj_set_size(tabs, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(tabs, 6, LV_PART_MAIN);
  static const char* const names[2] = {"Link", "Settings"};
  for (int i = 0; i < 2; i++) {
    lv_obj_t* x = lv_btn_create(tabs);
    styleButton(x);
    lv_obj_set_size(x, LV_SIZE_CONTENT, 28);
    lv_obj_set_flex_grow(x, 1);
    if (i == s_tab_i) teamui::selected(x);
    lv_obj_add_event_cb(x, [](lv_event_t* e) { s_tab_i = (int)(intptr_t)lv_event_get_user_data(e); pageBuild(); },
                        LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, TR(names[i]));
    useChainedFont(l);
    lv_obj_center(l);
  }
  if (s_tab_i == 0) buildLink(s_body);
  else buildSettings(s_body);
  teamui::tint(s_body);
}

static void pageClose() {
  if (s_page_timer) { lv_timer_del(s_page_timer); s_page_timer = nullptr; }
  s_page_open = false;
  s_body = nullptr;
  s_meter = nullptr;
  if (s_root) { lv_obj_t* r = s_root; s_root = nullptr; appPageDeleteRootAsync(r); }
  appPageEnd(pageBack);
}

static void pageOpen() {
  loadCfg();
  if (s_root) return;
  if (!VoxMic::available()) { toast(TR("This radio has no microphone")); return; }
  s_root = appPageCreateRoot(COLOR_BG);
  appPageBeginSlim("Sound link", pageBack);
  s_page_open = true;
  s_tab_i = 0;
  s_pick_channel = false;
  // Not started by opening the page: listening decodes the mic all the time.
  pageBuild();
  s_page_timer = lv_timer_create([](lv_timer_t*) {
    static uint32_t seen = 0;
    static St st_seen = St::Idle;
    if (s_meter) {
      const int v = s_level;
      s_level = v * 3 / 4;
      lv_bar_set_value(s_meter, v, LV_ANIM_OFF);
    }
    if ((s_ver != seen || s_st != st_seen) && s_tab_i == 0) { seen = s_ver; st_seen = s_st; pageBuild(); }
  }, 150, nullptr);
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

}  // namespace soundui

void soundLinkOnChannelText(int idx, const char* text) { soundui::onChannelText(idx, text); }

#endif  // CAP_SOUND_LINK
