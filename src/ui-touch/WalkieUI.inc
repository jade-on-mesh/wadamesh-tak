// SPDX-License-Identifier: GPL-3.0-or-later
// Included by UITask.cpp (after SoundLinkUI.inc): push-to-talk on a channel
// over MediaMesh, the same as MeshCore Open TAK's walkie-talkie. Wire format
// and jitter buffer: WalkieWire.h (host-tested); documentation: WALKIE.md.
//
// Hold the button and talk: the mic is encoded to Codec2 1200 as it's
// captured (VoxMic), and each second goes out on the channel as soon as it's
// there. Talks on any channel play as they arrive, through the audio worker,
// unless "Play talks live" is off or the channel is muted (then only while its
// walkie-talkie is open). When the button is let go the talk is posted as a
// voice message with the same session id and the "PTT" caption, which the chat
// shows as a talk with a waveform; a talk heard in full is kept, so its message
// plays with nothing to fetch.
// Everything here runs on the loop task except the playback job.

#include "WalkieWire.h"

namespace walkie {

constexpr int kHeardSlots = 4;
constexpr int kPktBytes = VoxWire::kBytesPerPacket;   // 156: 26 Codec2 1200 frames
// A GRP_DATA packet on air: header, path length, channel hash, MAC, and the
// type/length/payload block padded to the cipher's 16 bytes.
constexpr int kAirBytes = 1 + 1 + 1 + 2 + ((3 + WalkieWire::kAudioHeaderLen + kPktBytes + 15) / 16) * 16;

// ---- talks heard -------------------------------------------------------------
struct Heard {
  uint32_t sid;
  int      ch;
  char     name[WalkieWire::kMaxNameBytes + 1];
  uint32_t first_ms, last_ms;
  bool     end;
  bool     played;
  bool     stored;
  int      total;                                   // -1 until the last packet
  uint8_t  lens[WalkieWire::kMaxPacketsTalk];
  bool     have[WalkieWire::kMaxPacketsTalk];
  uint8_t  bits[WalkieWire::kMaxPacketsTalk * kPktBytes];
};
static Heard* s_heard = nullptr;   // kHeardSlots, PSRAM
static int s_heard_next = 0;

static bool heardInit() {
  if (s_heard) return true;
  s_heard = (Heard*)psAlloc(sizeof(Heard) * kHeardSlots);
  return s_heard != nullptr;
}

static Heard* heardFind(uint32_t sid) {
  if (!s_heard || !sid) return nullptr;
  for (int i = 0; i < kHeardSlots; i++) if (s_heard[i].sid == sid) return &s_heard[i];
  return nullptr;
}

static Heard* heardGet(uint32_t sid, int ch, uint32_t now) {
  Heard* h = heardFind(sid);
  if (h || !heardInit()) return h;
  h = &s_heard[s_heard_next];
  s_heard_next = (s_heard_next + 1) % kHeardSlots;
  memset(h, 0, sizeof *h);
  h->sid = sid;
  h->ch = ch;
  h->first_ms = h->last_ms = now;
  h->total = -1;
  return h;
}

static bool heardActive(const Heard& h, uint32_t now) {
  return h.sid && !h.end && now - h.last_ms <= WalkieWire::kTalkTimeoutMs;
}

// The talk on the air on channel `ch`, if anyone's talking.
static Heard* activeOn(int ch, uint32_t now) {
  if (!s_heard) return nullptr;
  Heard* best = nullptr;
  for (int i = 0; i < kHeardSlots; i++) {
    Heard& h = s_heard[i];
    if (h.ch == ch && heardActive(h, now) && (!best || (int32_t)(h.last_ms - best->last_ms) > 0)) best = &h;
  }
  return best;
}

// A talk heard in full: kept as its voice message's clip.
static void heardMaybeStore(Heard& h) {
  if (h.stored || h.total <= 0 || h.total > WalkieWire::kMaxPacketsTalk) return;
  size_t len = 0;
  for (int i = 0; i < h.total; i++) {
    if (!h.have[i]) return;
    if (i < h.total - 1 && h.lens[i] != kPktBytes) return;
    len += h.lens[i];
  }
  h.stored = VoxPhotos::storeHeard(h.sid, WalkieWire::kMode, h.bits, len);
}

// Session ids heard (or sent) as talks: their voice messages show as talks
// even when announced without the "PTT" caption.
static uint32_t s_talk_sids[32] = {0};
static int s_talk_sids_next = 0;
static void noteTalk(uint32_t sid) {
  if (!sid) return;
  for (uint32_t t : s_talk_sids) if (t == sid) return;
  s_talk_sids[s_talk_sids_next] = sid;
  s_talk_sids_next = (s_talk_sids_next + 1) % (int)(sizeof s_talk_sids / sizeof s_talk_sids[0]);
}
static bool heardTalk(uint32_t sid) {
  if (!sid) return false;
  for (uint32_t t : s_talk_sids) if (t == sid) return true;
  return false;
}

// ---- settings ----------------------------------------------------------------
static bool s_live_all = true;        // talks on every channel play live (unless muted)
static String s_muted;                // channels whose talks don't play live, one name per line
static String s_direct;               // channels set to Direct, one name per line
static char s_last[33] = "";          // the walkie-talkie last opened (the Mic key talks there)
static bool s_ball_talk = true;       // T-Deck: the trackball's button talks in a channel chat
static bool s_prefs_loaded = false;

static void prefsLoad() {
  if (s_prefs_loaded) return;
  s_prefs_loaded = true;
  SdNvsPrefs p;
  if (!p.begin("walkie", true)) return;
  s_live_all = p.getBool("liveall", true);
  s_muted = p.getString("muted", "");
  s_direct = p.getString("direct", "");
  snprintf(s_last, sizeof s_last, "%s", p.getString("last", "").c_str());
  s_ball_talk = p.getBool("balltalk", true);
  p.end();
}
static void prefsSave() {
  SdNvsPrefs p;
  if (!p.begin("walkie", false)) return;
  p.putBool("liveall", s_live_all);
  p.putString("muted", s_muted);
  p.putString("direct", s_direct);
  p.putString("last", s_last);
  p.putBool("balltalk", s_ball_talk);
  p.end();
}

static bool channelName(int ch, char* out, size_t cap) {
  ChannelDetails cd;
  if (ch < 0 || !the_mesh.getChannel(ch, cd) || !cd.name[0]) return false;
  snprintf(out, cap, "%s", cd.name);
  return true;
}

// A channel name in a one-name-per-line list.
static bool listHas(const String& list, int ch) {
  prefsLoad();
  char n[33];
  if (!channelName(ch, n, sizeof n)) return false;
  String line = String("\n") + n + "\n";
  return (String("\n") + list + "\n").indexOf(line) >= 0;
}
static void listSet(String& list, int ch, bool on) {
  char n[33];
  if (!channelName(ch, n, sizeof n) || listHas(list, ch) == on) return;
  String all = String("\n") + list + "\n";
  if (on) all += String(n) + "\n";
  else all.replace(String("\n") + n + "\n", "\n");
  all.trim();
  list = all;
  prefsSave();
}

static bool isDirect(int ch) { return listHas(s_direct, ch); }
static void setDirect(int ch, bool on) { listSet(s_direct, ch, on); }

// The channel's talks play live while "Play talks live" is on (not muted).
static bool channelLive(int ch) { return !listHas(s_muted, ch); }
static void setChannelLive(int ch, bool on) { listSet(s_muted, ch, !on); }
static bool liveAll() { prefsLoad(); return s_live_all; }
static void setLiveAll(bool on) {
  prefsLoad();
  s_live_all = on;
  prefsSave();
}

// ---- live playback -----------------------------------------------------------
static int s_card_ch = -1;   // the channel whose walkie-talkie is open
static bool listensTo(int ch) { return ch == s_card_ch || (liveAll() && channelLive(ch)); }

#if CAP_LUA_AUDIO && VOX_CODEC2_AVAILABLE
struct Chunk { uint16_t len; uint8_t data[kPktBytes]; };
constexpr uint16_t kChunkEnd = 0xFFFF;
static QueueHandle_t s_q = nullptr;
static volatile bool s_job_running = false;
static volatile bool s_job_stop = false;
static WalkieWire::Jitter* s_jit = nullptr;   // PSRAM
static uint32_t s_play_sid = 0;
static bool s_owns_audio = false;

// On the audio worker: decodes what the loop task hands over, as it comes,
// and keeps the speaker fed with silence in between.
static void playJob(void*) {
  VoxCodec2Stream codec;
  LuaAudioSinkSession sink;
  const char* err = nullptr;
  if (codec.begin(WalkieWire::kMode) && luaAudioSinkBegin(voxSourceRate(), &sink, &err)) {
    const int spf = codec.samplesPerFrame();
    const size_t bpf = (size_t)codec.bytesPerFrame();
    int16_t pcm[320];
    int16_t out[256 + 16];
    size_t n_out = 0;
    bool failed = false;
    uint32_t idle_ms = 0;
    VoxPlayOut po;   // interpolated + low-passed to the speaker's rate (VoxVoice.h)
    po.begin(sink.output_rate, sink.gain);
    static VoxBrighten bright;   // "Brighter voice", when on (UITask.cpp voxBrightBegin)
    voxBrightBegin(&bright, sink.output_rate);
    static Chunk c;
    while (!s_job_stop && !failed) {
      if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(40)) != pdTRUE) {
        // Nothing yet: 40 ms of silence keeps the speaker from repeating its
        // last buffer. A talk that went quiet for good ends from the loop task.
        idle_ms += 40;
        if (idle_ms > 8000) break;
        static int16_t quiet[256];
        size_t left = sink.output_rate * 40 / 1000;
        while (left && !failed) {
          const size_t n = left > 256 ? 256 : left;
          memset(quiet, 0, sizeof quiet);   // VoxBrighten writes in place
          failed = !voxSinkWrite(&sink, &bright, quiet, n);
          left -= n;
        }
        continue;
      }
      idle_ms = 0;
      if (c.len == kChunkEnd) break;
      for (size_t i = 0; i + bpf <= c.len && !failed && !s_job_stop; i += bpf) {
        codec.decode(c.data + i, pcm);
        for (int k = 0; k < spf && !failed; k++) {
          po.push(pcm[k], out, sizeof out / sizeof out[0], &n_out);
          if (n_out >= 256) {
            failed = !voxSinkWrite(&sink, &bright, out, n_out);
            n_out = 0;
          }
        }
      }
      if (n_out && !failed) {
        failed = !voxSinkWrite(&sink, &bright, out, n_out);
        n_out = 0;
      }
    }
    if (!failed) voxSinkFlush(&sink, &bright);
    bright.end();
    luaAudioSinkEnd(&sink);
  } else {
    Serial.printf("[WALKIE] playback couldn't start: %s\n", err ? err : "codec");
  }
  codec.end();
  s_job_running = false;
}
static void playTask(void* arg) {   // when the audio worker is busy or missing
  playJob(arg);
  vTaskDelete(nullptr);
}

static void playStop() {
  if (!s_play_sid) return;
  s_play_sid = 0;
  if (s_q) {
    Chunk end;
    end.len = kChunkEnd;
    xQueueSend(s_q, &end, 0);
  }
}

// The audio path is free for a talk: nothing else playing or recording.
static bool audioFree() {
  if (s_job_running) return false;
  if (luaAudioActive() || s_notify_playing || s_vox_voice_sid) return false;
  if (VoxMic::rawActive() || VoxMic::state() == VoxMic::kRecording) return false;
  if (!g_lv.task || g_lv.task->isBuzzerQuiet() || touchPrefsGetSoundVolume() == 0) return false;
  return true;
}

static bool playStart(Heard& h) {
  if (!audioFree()) return false;
  if (!s_jit) {
    s_jit = (WalkieWire::Jitter*)psAlloc(sizeof(WalkieWire::Jitter));
    if (!s_jit) return false;
  }
  if (!s_q) s_q = xQueueCreate(4, sizeof(Chunk));
  if (!s_q) return false;
  xQueueReset(s_q);
  s_jit->reset();
  // Whatever of this talk has come so far.
  for (int i = 0; i < WalkieWire::kMaxPacketsTalk; i++) {
    if (!h.have[i]) continue;
    WalkieWire::Audio a;
    a.sid = h.sid;
    a.index = (uint8_t)i;
    a.data = h.bits + (size_t)i * kPktBytes;
    a.len = h.lens[i];
    a.last = h.total >= 0 && (i + 1 == h.total);
    s_jit->add(a, millis());
  }
  __atomic_store_n(&s_lua_audio_active, true, __ATOMIC_RELEASE);   // chimes off meanwhile
  s_owns_audio = true;
  s_job_stop = false;
  s_job_running = true;
  if (!VoxAudioTask::run(playJob, nullptr) &&
      xTaskCreate(playTask, "walkie_play", 16 * 1024, nullptr, 3, nullptr) != pdPASS) {
    s_job_running = false;
    __atomic_store_n(&s_lua_audio_active, false, __ATOMIC_RELEASE);
    s_owns_audio = false;
    return false;
  }
  s_play_sid = h.sid;
  h.played = true;
  return true;
}

static void playTick(uint32_t now) {
  if (s_play_sid && s_jit) {
    const uint8_t* d = nullptr;
    int len = 0;
    while (uxQueueSpacesAvailable(s_q) > 0) {
      if (s_jit->due(now, &d, &len) < 0) break;
      static Chunk c;
      c.len = (uint16_t)len;
      memcpy(c.data, d, (size_t)len);
      xQueueSend(s_q, &c, 0);
    }
    if (s_jit->finished(now)) playStop();
  }
  if (!s_job_running && s_owns_audio) {   // the job has let the speaker go
    __atomic_store_n(&s_lua_audio_active, false, __ATOMIC_RELEASE);
    s_owns_audio = false;
  }
}
static void playFeed(const WalkieWire::Audio& a, uint32_t now) {
  if (s_play_sid == a.sid && s_jit) s_jit->add(a, now);
}
static bool playing(uint32_t sid) { return sid && s_play_sid == sid; }
#else
static void playStop() {}
static bool playStart(Heard&) { return false; }
static void playTick(uint32_t) {}
static void playFeed(const WalkieWire::Audio&, uint32_t) {}
static bool playing(uint32_t) { return false; }
#endif

// ---- talking -----------------------------------------------------------------
#if (defined(HAS_TDECK_GT911) || defined(HAS_TDISPLAY_P4)) && CAP_LUA_AUDIO
#define WALKIE_CAN_TALK 1
#else
#define WALKIE_CAN_TALK 0
#endif

static int      s_tx_ch = -1;
static uint32_t s_tx_sid = 0;
static bool     s_tx_direct = false;
static int      s_tx_sent = 0;          // audio packets sent
static uint32_t s_tx_last_ms = 0;       // when the last packet went out
static bool     s_tx_released = false;  // let go (or 20 s, or the mic stopped)
static bool     s_tx_mic_done = false;  // the recording is in s_tx_bits
static bool     s_tx_failed = false;
static uint8_t* s_tx_bits = nullptr;    // the finished recording (VoxMic::take)
static size_t   s_tx_len = 0;
static uint32_t s_tx_ms = 0;
static uint8_t  s_tx_mode = WalkieWire::kMode;
static bool     s_tx_owns_audio = false;
static lv_indev_t* s_tx_indev = nullptr;

static bool talking() { return s_tx_ch >= 0; }

// Pause between packets, as the app spaces them: through repeaters a packet
// must not talk over the relays of the one before; direct, just its airtime.
static uint32_t gapMs(bool direct) {
  uint32_t air = mediamesh::mediaRadio2AirMs(kAirBytes);
  if (!air) air = 400;
  return direct ? (air + 60 > 150 ? air + 60 : 150) : (air * 4 + 150 > 800 ? air * 4 + 150 : 800);
}

static bool sendData(const uint8_t* b, int n) {
  return n > 0 && the_mesh.voxSendChannelData(s_tx_ch, VoxWire::kChannelDataType, b, (size_t)n, s_tx_direct);
}

static void txEnd() {
  if (s_tx_bits) free(s_tx_bits);
  s_tx_bits = nullptr;
  s_tx_len = 0;
  s_tx_ch = -1;
  s_tx_sid = 0;
  s_tx_indev = nullptr;
  if (s_tx_owns_audio) {
    __atomic_store_n(&s_lua_audio_active, false, __ATOMIC_RELEASE);
    s_tx_owns_audio = false;
  }
}

// Starts talking on channel `ch`. False with `why` set when it can't.
static bool txStart(int ch, const char** why) {
#if WALKIE_CAN_TALK
  const uint32_t now = millis();
  if (talking()) { *why = "Already talking"; return false; }
  if (!VoxMic::available()) { *why = "This radio has no microphone"; return false; }
  if (!mediamesh::mediaSendable()) { *why = "Talking needs MediaMesh: radio2 on rx+tx"; return false; }
  if (activeOn(ch, now)) { *why = "Someone is talking"; return false; }
  playStop();
#if CAP_LUA_AUDIO && VOX_CODEC2_AVAILABLE
  if (s_job_running) { *why = "Audio is busy"; return false; }
#endif
  if (luaAudioActive() || s_notify_playing || s_vox_voice_sid) { *why = "Audio is busy"; return false; }
  __atomic_store_n(&s_lua_audio_active, true, __ATOMIC_RELEASE);   // chimes would be recorded
  s_tx_owns_audio = true;
  if (!VoxMic::start(WalkieWire::kMode, why)) {
    txEnd();
    if (!*why) *why = "Recording failed";
    return false;
  }
  s_tx_ch = ch;
  s_tx_sid = 0;
  while (!s_tx_sid) s_tx_sid = esp_random();
  s_tx_direct = isDirect(ch);
  s_tx_sent = 0;
  s_tx_released = s_tx_mic_done = s_tx_failed = false;
  s_tx_mode = WalkieWire::kMode;
  s_tx_ms = 0;
  uint8_t b[WalkieWire::kStartHeaderLen + WalkieWire::kMaxNameBytes];
  const int n = WalkieWire::encodeStart(s_tx_sid, s_tx_mode, the_mesh.getNodePrefs()->node_name, b, sizeof b);
  sendData(b, n);
  s_tx_last_ms = now;
  Serial.printf("[WALKIE] talking on channel %d (%s), sid %08lx\n", ch, s_tx_direct ? "direct" : "repeaters",
                (unsigned long)s_tx_sid);
  return true;
#else
  (void)ch;
  *why = "This radio has no microphone";
  return false;
#endif
}

static void txRelease() {
  if (!talking() || s_tx_released) return;
  s_tx_released = true;
  VoxMic::stop();
}

// Posts the finished talk in the chat as its voice message (VE3, same sid).
static void txAnnounce() {
  char env[48];
  uint32_t sid = 0;
  uint8_t* bits = s_tx_bits;
  s_tx_bits = nullptr;   // addRecording owns it now
  if (!VoxPhotos::addRecording(s_tx_mode, bits, s_tx_len, s_tx_ms, env, sizeof env, &sid, s_tx_sid)) {
    if (g_lv.task) g_lv.task->showAlert(TR("Couldn't save the talk"), 1800);
    return;
  }
  // The "PTT" caption marks it as a talk (WALKIE.md); versions that don't know
  // it show "Voice message: PTT".
  const size_t used = strlen(env);
  if (used + 4 < sizeof env) memcpy(env + used, " PTT", 5);
  noteTalk(sid);
  ChannelDetails cd;
  if (!the_mesh.getChannel(s_tx_ch, cd) || !cd.name[0]) return;
  const uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  if (sendGroupMessageScoped(s_tx_ch, cd, ts, the_mesh.getNodePrefs()->node_name, env)) {
    if (g_lv.task) g_lv.task->appSentMsgToChannel(cd.name, env, the_mesh.uiLastSentFp());
    g_lv.dirty_threads = true;
  }
}

static void txTick(uint32_t now) {
  if (!talking()) return;
  // Let go: the finger left the button (read from the touch panel, as the mic does).
  if (!s_tx_released && s_tx_indev && s_tx_indev->proc.state != LV_INDEV_STATE_PRESSED) txRelease();
  if (!s_tx_mic_done) {
    const VoxMic::State st = VoxMic::state();
    if (st == VoxMic::kReady) {   // let go, or 20 s
      uint8_t mode = 0;
      uint8_t* bits = nullptr;
      size_t len = 0;
      uint32_t ms = 0;
      if (VoxMic::take(&mode, &bits, &len, &ms, 0)) {
        s_tx_bits = bits;
        s_tx_len = len;
        s_tx_ms = ms;
        s_tx_mode = mode;
      }
      s_tx_mic_done = s_tx_released = true;
    } else if (st != VoxMic::kRecording) {   // failed or cancelled
      if (st == VoxMic::kFailed && g_lv.task) {
        const char* e = VoxMic::error();
        g_lv.task->showAlert(TR(e && *e ? e : "Recording failed"), 2000);
        VoxMic::cancel();
      }
      s_tx_failed = true;
      s_tx_mic_done = s_tx_released = true;
    }
  }
  if (now - s_tx_last_ms < gapMs(s_tx_direct)) return;
  const uint8_t* bits = nullptr;
  size_t len = 0;
  if (s_tx_bits) { bits = s_tx_bits; len = s_tx_len; }
  else if (!s_tx_mic_done) VoxMic::liveBits(&bits, &len);
  uint8_t b[WalkieWire::kAudioHeaderLen + kPktBytes];
  if (bits && len >= (size_t)(s_tx_sent + 1) * kPktBytes && s_tx_sent < WalkieWire::kMaxPacketsTalk - 1) {
    const int n = WalkieWire::encodeAudio(s_tx_sid, (uint8_t)s_tx_sent, s_tx_mode, false,
                                          bits + (size_t)s_tx_sent * kPktBytes, kPktBytes, b, sizeof b);
    sendData(b, n);
    s_tx_sent++;
    s_tx_last_ms = now;
    return;
  }
  if (!s_tx_mic_done) return;
  // Everything full is out: the rest, marked last (possibly with no audio).
  const size_t done = (size_t)s_tx_sent * kPktBytes;
  const int rest = bits && len > done ? (int)(len - done) : 0;
  const int n = WalkieWire::encodeAudio(s_tx_sid, (uint8_t)s_tx_sent, s_tx_mode, true,
                                        rest ? bits + done : nullptr, rest, b, sizeof b);
  sendData(b, n);
  Serial.printf("[WALKIE] talk %08lx done: %d packets, %lu ms\n", (unsigned long)s_tx_sid,
                s_tx_sent + (rest ? 1 : 0), (unsigned long)s_tx_ms);
  if (!s_tx_failed && s_tx_bits && s_tx_ms >= WalkieWire::kMinTalkMs) txAnnounce();
  txEnd();
}

// ---- the walkie-talkie card ----------------------------------------------------
static lv_obj_t*   s_card = nullptr;
static lv_obj_t*   s_status = nullptr;
static lv_obj_t*   s_bar = nullptr;
static lv_obj_t*   s_talk_btn = nullptr;
static lv_obj_t*   s_talk_lbl = nullptr;
static lv_obj_t*   s_reach_lbl = nullptr;
static lv_obj_t*   s_live_all_lbl = nullptr;
static lv_obj_t*   s_listen_lbl = nullptr;
static lv_timer_t* s_timer = nullptr;
static int         s_talk_style = -1;   // 0 idle, 1 on air, 2 busy

static void cardClose();

static void refreshCard(uint32_t now) {
  if (!s_card) return;
  char line[80];
  Heard* h = activeOn(s_card_ch, now);
  int style = 0;
  if (talking() && s_tx_ch == s_card_ch) {
    const uint32_t ms = s_tx_mic_done ? s_tx_ms : VoxMic::elapsedMs();
    if (s_tx_released) snprintf(line, sizeof line, "%s", TR("Finishing..."));
    else snprintf(line, sizeof line, "%s  %lu:%02lu / 0:%02lu", TR("On air"), (unsigned long)(ms / 60000),
                  (unsigned long)(ms / 1000 % 60), (unsigned long)(WalkieWire::kMaxTalkMs / 1000));
    style = 1;
    if (s_bar) {
      lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
      lv_bar_set_value(s_bar, s_tx_released ? 0 : VoxMic::level(), LV_ANIM_OFF);
    }
  } else {
    if (s_bar) lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    if (h) {
      if (h->name[0]) snprintf(line, sizeof line, "%s %s", h->name, TR("is talking"));
      else snprintf(line, sizeof line, "%s", TR("Someone is talking"));
      if (playing(h->sid)) strncat(line, "  " LV_SYMBOL_VOLUME_MAX, sizeof line - strlen(line) - 1);
      style = 2;
    } else {
      snprintf(line, sizeof line, "%s", TR("Channel clear"));
    }
  }
  if (strcmp(lv_label_get_text(s_status), line) != 0) lv_label_set_text(s_status, line);
  if (style != s_talk_style && s_talk_btn) {
    s_talk_style = style;
    if (style == 1) styleDangerSolid(s_talk_btn);
    else if (style == 2) styleButton(s_talk_btn);
    else stylePrimary(s_talk_btn);
    lv_obj_set_style_radius(s_talk_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_label_set_text(s_talk_lbl, style == 1 ? TR("Let go to finish") : TR("Hold to talk"));
  }
}

static void talkBtnCb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    if (talking()) return;
    const char* why = nullptr;
    if (!txStart(s_card_ch, &why)) {
      if (g_lv.task) g_lv.task->showAlert(TR(why ? why : "Can't talk now"), 1800);
      return;
    }
    s_tx_indev = lv_indev_get_act();
    refreshCard(millis());
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (talking() && s_tx_ch == s_card_ch) txRelease();
  }
}

static void reachText() {
  if (!s_reach_lbl) return;
  lv_label_set_text_fmt(s_reach_lbl, "%s %s", LV_SYMBOL_SHUFFLE,
                        isDirect(s_card_ch) ? TR("Reach: direct (no repeaters)") : TR("Reach: through repeaters"));
}
static void listenText() {
  if (s_live_all_lbl)
    lv_label_set_text_fmt(s_live_all_lbl, "%s %s", liveAll() ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                          TR("Play talks live (all channels)"));
  if (s_listen_lbl)
    lv_label_set_text_fmt(s_listen_lbl, "%s %s", liveAll() && channelLive(s_card_ch) ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                          liveAll() ? TR("Play this channel's talks live") : TR("This channel: only while open"));
}
static void reachCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || talking()) return;
  setDirect(s_card_ch, !isDirect(s_card_ch));
  reachText();
}
static void liveAllCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  setLiveAll(!liveAll());
  listenText();
}
static void listenCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !liveAll()) return;
  setChannelLive(s_card_ch, !channelLive(s_card_ch));
  listenText();
}
static void closeCb(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) cardClose();
}
#if CAP_TRACKBALL && WALKIE_CAN_TALK
static lv_obj_t* s_ball_lbl = nullptr;
static void ballText() {
  if (s_ball_lbl)
    lv_label_set_text_fmt(s_ball_lbl, "%s %s", s_ball_talk ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                          TR("Trackball button talks in channel chats"));
}
static void ballCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  prefsLoad();
  s_ball_talk = !s_ball_talk;
  prefsSave();
  ballText();
}
#endif

static lv_obj_t* rowButton(lv_obj_t* parent, lv_event_cb_t cb, lv_obj_t** label) {
  lv_obj_t* b = lv_btn_create(parent);
  styleButton(b);
  lv_obj_set_width(b, lv_pct(100));
  lv_obj_set_height(b, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_ver(b, SC(6), LV_PART_MAIN);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  *label = lv_label_create(b);
  lv_obj_set_style_text_font(*label, &g_font_14, LV_PART_MAIN);
  lv_obj_set_width(*label, lv_pct(100));
  lv_label_set_long_mode(*label, LV_LABEL_LONG_WRAP);
  if (s_nav_group) lv_group_add_obj(s_nav_group, b);
  return b;
}

static void tick(uint32_t now);
static void timerCb(lv_timer_t*) { tick(millis()); }

static void cardOpen(int ch) {
  if (s_card) cardClose();
  char name[33];
  if (!channelName(ch, name, sizeof name)) return;
  prefsLoad();
  if (strcmp(s_last, name) != 0) {   // the Mic key's channel from now on
    snprintf(s_last, sizeof s_last, "%s", name);
    prefsSave();
  }
  s_card_ch = ch;
  s_talk_style = -1;
  s_card = lv_obj_create(lv_layer_top());
  lv_obj_t* card = s_card;
  styleCard(card);
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
  const lv_coord_t sh = lv_disp_get_ver_res(nullptr);
  lv_obj_set_width(card, LV_MIN((lv_coord_t)(sw - SC(12)), (lv_coord_t)SC(300)));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(card, sh - SC(12), LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, SC(8), LV_PART_MAIN);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(card, SC(6), LV_PART_MAIN);

  lv_obj_t* head = lv_obj_create(card);   // [mic] Walkie-talkie · #name   [x]
  lv_obj_remove_style_all(head);
  lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(head, SC(6), LV_PART_MAIN);
  lv_obj_t* mic = lv_label_create(head);
  lv_label_set_text(mic, UI_ICON_MIC);
  lv_obj_set_style_text_font(mic, &ui_icons_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(mic, lv_color_hex(COLOR_GLOW), LV_PART_MAIN);
  lv_obj_t* title = lv_label_create(head);
  lv_label_set_text_fmt(title, "%s - %s", TR("Walkie-talkie"), name);
  lv_obj_set_style_text_font(title, &g_font_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  lv_obj_set_flex_grow(title, 1);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_t* x = lv_btn_create(head);
  styleButton(x);
  lv_obj_set_size(x, SC(30), SC(26));
  lv_obj_add_event_cb(x, closeCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* xl = lv_label_create(x);
  lv_label_set_text(xl, LV_SYMBOL_CLOSE);
  lv_obj_center(xl);

  s_status = lv_label_create(card);
  lv_label_set_text(s_status, "");
  lv_obj_set_style_text_font(s_status, &g_font_semi_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_status, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);

  s_bar = lv_bar_create(card);
  lv_obj_set_size(s_bar, lv_pct(90), SC(5));
  lv_bar_set_range(s_bar, 0, 100);
  lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_STATUS_DANGER), LV_PART_INDICATOR);
  lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

  const lv_coord_t d = LV_MIN((lv_coord_t)SC(110), (lv_coord_t)(sh / 3));
  s_talk_btn = lv_btn_create(card);
  lv_obj_set_size(s_talk_btn, d, d);
  lv_obj_add_event_cb(s_talk_btn, talkBtnCb, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s_talk_btn, talkBtnCb, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(s_talk_btn, talkBtnCb, LV_EVENT_PRESS_LOST, nullptr);
  s_talk_lbl = lv_label_create(s_talk_btn);
  lv_obj_set_width(s_talk_lbl, d - SC(12));
  lv_obj_set_style_text_align(s_talk_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(s_talk_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_center(s_talk_lbl);
  if (s_nav_group) {
    lv_group_add_obj(s_nav_group, s_talk_btn);
    lv_group_focus_obj(s_talk_btn);
  }
#if !WALKIE_CAN_TALK
  lv_obj_add_state(s_talk_btn, LV_STATE_DISABLED);   // listen only: no microphone
#endif

  lv_obj_t* tmp = nullptr;
  rowButton(card, reachCb, &tmp);
  s_reach_lbl = tmp;
  rowButton(card, liveAllCb, &tmp);
  s_live_all_lbl = tmp;
  rowButton(card, listenCb, &tmp);
  s_listen_lbl = tmp;
#if CAP_TRACKBALL && WALKIE_CAN_TALK
  rowButton(card, ballCb, &tmp);
  s_ball_lbl = tmp;
  ballText();
#endif
  reachText();
  listenText();

  lv_obj_t* hint = lv_label_create(card);
  lv_obj_set_width(hint, lv_pct(100));
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
#if defined(HAS_TDECK_KEYBOARD)
#if CAP_TRACKBALL
  lv_label_set_text(hint, TR("Up to 20 s a talk, on MediaMesh only. Each one is saved in the chat. "
                             "From any screen, hold the keyboard's Mic key, or hold the trackball still, to talk."));
#else
  lv_label_set_text(hint, TR("Up to 20 s a talk, on MediaMesh only. Each one is saved in the chat. "
                             "From any screen, hold the keyboard's Mic key to talk."));
#endif
#else
  lv_label_set_text(hint, TR(WALKIE_CAN_TALK ? "Up to 20 s a talk, on MediaMesh only. Each one is saved in the chat."
                                             : "No microphone here: you hear talks as they come."));
#endif
  lv_obj_set_style_text_font(hint, &g_font_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_SUB), LV_PART_MAIN);

  lv_obj_update_layout(card);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
  refreshCard(millis());
  if (!s_timer) s_timer = lv_timer_create(timerCb, 50, nullptr);
}

static void cardClose() {
  if (talking() && s_tx_ch == s_card_ch) txRelease();   // the talk still finishes
  if (s_card) lv_obj_del(s_card);
  s_card = s_status = s_bar = s_talk_btn = s_talk_lbl = s_reach_lbl = s_listen_lbl = s_live_all_lbl = nullptr;
#if CAP_TRACKBALL && WALKIE_CAN_TALK
  s_ball_lbl = nullptr;
#endif
#if CAP_LUA_AUDIO && VOX_CODEC2_AVAILABLE
  if (s_play_sid && !(liveAll() && channelLive(s_card_ch))) playStop();
#endif
  s_card_ch = -1;
}

// ---- every 50 ms (card open) or from the loop ------------------------------------
static void tick(uint32_t now) {
  txTick(now);
  playTick(now);
  // Forget talks long gone.
  if (s_heard) {
    for (int i = 0; i < kHeardSlots; i++) {
      Heard& h = s_heard[i];
      if (h.sid && !playing(h.sid) && now - h.last_ms > 10u * 60u * 1000u) h.sid = 0;
    }
  }
  refreshCard(now);
  if (!s_card && !talking() && s_timer) {
#if CAP_LUA_AUDIO && VOX_CODEC2_AVAILABLE
    if (s_play_sid || s_job_running) return;
#endif
    lv_timer_del(s_timer);
    s_timer = nullptr;
  }
}

static void ensureTimer() {
  if (!s_timer) s_timer = lv_timer_create(timerCb, 50, nullptr);
}

// ---- the Mic key (T-Deck keyboard): hold to talk from any screen ----------------------
// Held 0.3 s (so Sym + Mic, typing a 0, doesn't start a talk): talks on the
// channel whose chat is open, else the walkie-talkie opened last, with its card
// up so you see it's on the air. Let go: the talk ends. Works locked too.
static bool s_key_down = false, s_key_talking = false, s_key_tried = false;
static uint32_t s_key_since = 0;

// The open channel chat's channel, else the last walkie-talkie's; -1 if neither.
static int keyChannel() {
  if (g_lv.ch.detail_open && g_lv.task) {
    bool ch = false; uint16_t unread; uint32_t ts;
    char tname[UITask::MAX_THREAD_NAME + 1];
    const int t = g_lv.task->activeThreadIdx();
    if (t >= 0 && g_lv.task->getThreadInfo(t, ch, unread, ts, tname, sizeof tname) && ch) {
      ChannelDetails cd{};
      for (int i = 0; i < MAX_GROUP_CHANNELS; ++i)
        if (the_mesh.getChannel(i, cd) && cd.name[0] && strncmp(cd.name, tname, sizeof cd.name) == 0) return i;
    }
  }
  prefsLoad();
  if (!s_last[0]) return -1;
  ChannelDetails cd{};
  for (int i = 0; i < MAX_GROUP_CHANNELS; ++i)
    if (the_mesh.getChannel(i, cd) && cd.name[0] && strcmp(cd.name, s_last) == 0) return i;
  return -1;
}

// Whether pressing the trackball's button in should talk (T-Deck): with the
// walkie-talkie card up, always; otherwise in a channel's chat, at its composer,
// while that's switched on (it is by default). Everywhere else it clicks.
static bool ballTalks() {
#if CAP_TRACKBALL && WALKIE_CAN_TALK
  if (s_card) return true;
  prefsLoad();
  return s_ball_talk && g_lv.ch.detail_open && g_lv.ch.composer_ta && keyChannel() >= 0;
#else
  return false;
#endif
}

// `held`: the Mic key or the trackball's button. The Mic key talks after 0.3 s
// held (Sym + Mic types 0); the trackball's button (`at_once`) as it goes in.
static void keyTick(bool held, uint32_t now, bool at_once = false) {
#if WALKIE_CAN_TALK
  if (held && !s_key_down) {
    s_key_down = true;
    s_key_tried = false;
    s_key_since = now;
  } else if (!held && s_key_down) {
    s_key_down = false;
    if (s_key_talking && talking()) txRelease();
    s_key_talking = false;
    return;
  }
  if (!s_key_down || s_key_tried || (!at_once && now - s_key_since < 300)) return;
  s_key_tried = true;
  if (talking()) return;
  const int ch = keyChannel();
  if (ch < 0) {
    if (g_lv.task) g_lv.task->showAlert(TR("Open a channel, or its walkie-talkie once, then hold Mic to talk"), 2200);
    return;
  }
  if (!s_card || s_card_ch != ch) cardOpen(ch);
  const char* why = nullptr;
  if (!txStart(ch, &why)) {
    if (g_lv.task) g_lv.task->showAlert(TR(why ? why : "Can't talk now"), 1800);
    return;
  }
  s_key_talking = true;
  ensureTimer();
  refreshCard(now);
#else
  (void)held; (void)now;
#endif
}

// ---- packets from the mesh -------------------------------------------------------
static void onPayload(const uint8_t* data, size_t len, int ch) {
#if !(CAP_LUA_AUDIO && VOX_CODEC2_AVAILABLE)
  (void)data; (void)len; (void)ch;
  return;   // no speaker or no Codec2 here: talks arrive as voice messages
#endif
  const uint32_t now = millis();
  if (data[0] == WalkieWire::kStartMagic) {
    WalkieWire::Start s;
    if (!WalkieWire::parseStart(data, len, &s) || s.sid == s_tx_sid) return;
    noteTalk(s.sid);
    Heard* h = heardGet(s.sid, ch, now);
    if (!h) return;
    snprintf(h->name, sizeof h->name, "%s", s.name);
    h->last_ms = now;
    return;
  }
  WalkieWire::Audio a;
  if (!WalkieWire::parseAudio(data, len, &a) || a.sid == s_tx_sid) return;
  noteTalk(a.sid);
  Heard* h = heardGet(a.sid, ch, now);
  if (!h) return;
  h->last_ms = now;
  if (a.last) {
    h->end = true;
    h->total = a.totalIfLast();
  }
  if (a.len > 0 && a.index < WalkieWire::kMaxPacketsTalk) {
    memcpy(h->bits + (size_t)a.index * kPktBytes, a.data, (size_t)a.len);
    h->lens[a.index] = (uint8_t)a.len;
    h->have[a.index] = true;
  }
  playFeed(a, now);
  if (!h->played && !talking() && listensTo(ch)) {
    if (playStart(*h)) ensureTimer();
  }
  heardMaybeStore(*h);
}

}  // namespace walkie

// MyMesh::onChannelDataRecv: GRP_DATA 0x5643 (WalkieWire.h).
void walkieOnChannelData(int channel_idx, const uint8_t* data, size_t len) {
  if (!WalkieWire::isLive(data, len) || channel_idx < 0) return;
  walkie::onPayload(data, len, channel_idx);
}
