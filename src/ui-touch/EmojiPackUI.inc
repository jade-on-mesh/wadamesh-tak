// SPDX-License-Identifier: GPL-3.0-or-later
// The full emoji set (EmojiPack.h, EMOJI.md): the ~1400 emoji the firmware doesn't
// carry in flash (its app slot is nearly full), from the file emoji-pack.wep:
//   1. the internal store's copy (/emoji.wep), else
//   2. the SD card's (/emoji/emoji-pack.wep or /emoji-pack.wep), copied in, else
//   3. downloaded over Wi-Fi from this firmware's own release (main's if the
//      release has none), then kept on the internal store.
// Held in PSRAM: emojiGlyphLookup() (emoji_data.c) asks emojiPackLookup() for
// anything it hasn't baked. Files are read and written a chunk per loop pass, so
// none of this ever holds the screen. Included into UITask.cpp before the fetch
// worker; emojiPackTick() runs from the UI loop.

#if LV_USE_IMGFONT && defined(ESP32) && !defined(HAS_TANMATSU)
#define EMOJI_PACK 1
#include "EmojiPack.h"

static fs::FS* backupInternalFs();
static void voxThumbLanded();   // re-lays out the open chat (emoji widths changed)

static constexpr const char* kEpPath = "/emoji.wep";
static constexpr const char* kEpTmp  = "/emoji.tmp";
static constexpr size_t kEpChunk = 16 * 1024;

// 0 start, 1 reading a file, 2 in use, 3 wanted (download when Wi-Fi is up),
// 4 downloading, 5 off (no PSRAM to hold it)
static uint8_t   s_ep_state = 0;
static uint8_t*  s_ep_buf = nullptr;     // the file, once in use (the View points into it)
static size_t    s_ep_len = 0;
static emojipack::View s_ep;
static bool      s_ep_ok = false;        // s_ep is published (UI thread only)
static lv_img_dsc_t** s_ep_imgs = nullptr;   // unpacked on first use, kept (callers hold them)
static uint8_t   s_ep_src = 0;           // where it came from: 1 store, 2 SD, 3 download
static uint32_t  s_ep_next_ms = 0;

// Reading one file (state 1).
static fs::FS*   s_ep_rfs = nullptr;
static File      s_ep_rf;
static uint8_t*  s_ep_rbuf = nullptr;
static size_t    s_ep_rsize = 0, s_ep_rgot = 0;
static int       s_ep_rnext = 0;   // the next source to try

// Saving to the internal store (after an SD copy or a download).
static bool      s_ep_save = false;
static File      s_ep_wf;
static size_t    s_ep_wpos = 0;

// The fetch worker (EMOJI_PACK_DOWNLOAD).
#if defined(MULTI_TRANSPORT_COMPANION) && WADA_FORK_RELEASES
#define EMOJI_PACK_DOWNLOAD 1
static volatile bool s_ep_dl_request = false;     // UI -> worker
static volatile bool s_ep_dl_done = false;        // worker -> UI
static volatile bool s_ep_dl_ok = false;
static uint8_t* volatile s_ep_dl = nullptr;
static volatile size_t s_ep_dl_len = 0;
static char s_ep_dl_msg[112];
#else
#define EMOJI_PACK_DOWNLOAD 0
#endif

static void* epAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

// Room for the file, its index of images and the images themselves as they're used.
static bool epRoomFor(size_t n) {
  return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= n + 1536u * 1024u;
}

extern "C" const lv_img_dsc_t* emojiPackLookup(uint32_t cp) {
  if (!s_ep_ok) return nullptr;
  const int i = emojipack::find(s_ep, cp);
  if (i < 0) return nullptr;
  if (s_ep_imgs[i]) return s_ep_imgs[i];
  const emojipack::Entry e = emojipack::entryAt(s_ep, i);
  if (!e.w) return emojiGlyphLookup(0x200D);   // drawn as nothing: the baked zero-width glyph
  const size_t n = (size_t)e.w * e.h * 3u;
  uint8_t tmp[16 * 16 * 3];
  lv_img_dsc_t* d = (lv_img_dsc_t*)epAlloc(sizeof(lv_img_dsc_t) + n);
  if (!d) return nullptr;
  uint8_t* px = (uint8_t*)(d + 1);
  if (!emojipack::unpack(s_ep, i, px, tmp)) { free(d); return nullptr; }
  memset(d, 0, sizeof *d);
  d->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
  d->header.w = e.w;
  d->header.h = e.h;
  d->data_size = (uint32_t)n;
  d->data = px;
  s_ep_imgs[i] = d;
  return d;
}

// Takes `buf` (len bytes, checked) into use. False leaves buf to the caller.
static bool epUse(uint8_t* buf, size_t len, uint8_t src) {
  emojipack::View v;
  if (!emojipack::parse(buf, len, &v)) return false;
  lv_img_dsc_t** imgs = (lv_img_dsc_t**)epAlloc(sizeof(lv_img_dsc_t*) * v.count);
  if (!imgs) return false;
  memset(imgs, 0, sizeof(lv_img_dsc_t*) * v.count);
  s_ep = v;
  s_ep_buf = buf;
  s_ep_len = len;
  s_ep_imgs = imgs;
  s_ep_src = src;
  s_ep_ok = true;
  s_ep_state = 2;
  Serial.printf("[EMOJI] full set in use: %u emoji, %u bytes, from %s\n", (unsigned)v.count,
                (unsigned)len, src == 1 ? "the store" : src == 2 ? "SD" : "download");
  voxThumbLanded();   // lay out the open chat again: its emoji have widths now
  return true;
}

// Opens the next place the file may be. False when there are none left.
static bool epOpenNext() {
  for (;;) {
    fs::FS* fs = nullptr;
    const char* path = nullptr;
    switch (s_ep_rnext++) {
      case 0: fs = backupInternalFs(); path = kEpPath; break;
#if CAP_SD
      case 1: if (SD.cardType() != CARD_NONE) { fs = &SD; path = "/emoji/emoji-pack.wep"; } break;
      case 2: if (SD.cardType() != CARD_NONE) { fs = &SD; path = "/emoji-pack.wep"; } break;
#endif
      case 3: return false;
      default: return false;
    }
    if (!fs || !path || !fs->exists(path)) continue;
    File f = fs->open(path, "r");
    if (!f) continue;
    const size_t sz = f.size();
    if (sz < emojipack::kHeader || sz > emojipack::kMaxBytes) { f.close(); continue; }
    if (!epRoomFor(sz)) { f.close(); s_ep_state = 5; return false; }
    uint8_t* b = (uint8_t*)epAlloc(sz);
    if (!b) { f.close(); s_ep_state = 5; return false; }
    s_ep_rfs = fs;
    s_ep_rf = f;
    s_ep_rbuf = b;
    s_ep_rsize = sz;
    s_ep_rgot = 0;
    return true;
  }
}

// Saving to the store: a chunk at a time into a temporary file, renamed at the end.
static void epSaveStart() {
  fs::FS* fs = backupInternalFs();
  if (!fs || !s_ep_buf) return;
#if !defined(HAS_TDISPLAY_P4) && !defined(HAS_CROWPANEL_P4_101)
  const size_t total = SPIFFS.totalBytes(), used = SPIFFS.usedBytes();
  if (total <= used || total - used < s_ep_len + 64u * 1024u) {
    Serial.println("[EMOJI] not enough room on the internal store: kept for this session only");
    return;
  }
#endif
  fs->remove(kEpTmp);
  s_ep_wf = fs->open(kEpTmp, "w");
  if (!s_ep_wf) return;
  s_ep_wpos = 0;
  s_ep_save = true;
}

static void epSaveStep() {
  fs::FS* fs = backupInternalFs();
  const size_t k = s_ep_len - s_ep_wpos < kEpChunk ? s_ep_len - s_ep_wpos : kEpChunk;
  bool ok = fs != nullptr;
  if (ok && k) {
    WdtHeavyGuard guard;
    ok = s_ep_wf.write(s_ep_buf + s_ep_wpos, k) == k;
  }
  s_ep_wpos += k;
  if (ok && s_ep_wpos < s_ep_len) return;
  s_ep_wf.close();
  s_ep_save = false;
  if (ok) {
    fs->remove(kEpPath);
    ok = fs->rename(kEpTmp, kEpPath);
  }
  if (!ok) {
    if (fs) fs->remove(kEpTmp);
    Serial.println("[EMOJI] couldn't save the full set to the internal store");
    return;
  }
  Serial.println("[EMOJI] full set saved to the internal store");
}

static void emojiPackTick(uint32_t now) {
  if (s_ep_save) { epSaveStep(); return; }
  switch (s_ep_state) {
    case 0:   // start a few seconds after boot, once the store and the card are up
      if (!s_ep_next_ms) { s_ep_next_ms = now + 4000; return; }
      if ((long)(now - s_ep_next_ms) < 0) return;
      s_ep_rnext = 0;
      if (epOpenNext()) s_ep_state = 1;
      else if (s_ep_state == 0) s_ep_state = 3;
      return;
    case 1: {   // a chunk of the file
      const size_t k = s_ep_rsize - s_ep_rgot < kEpChunk ? s_ep_rsize - s_ep_rgot : kEpChunk;
      const int r = k ? s_ep_rf.read(s_ep_rbuf + s_ep_rgot, k) : 0;
      if (r > 0) s_ep_rgot += (size_t)r;
      if (r > 0 && s_ep_rgot < s_ep_rsize) return;
      s_ep_rf.close();
      const bool from_store = s_ep_rnext == 1;
      if (s_ep_rgot == s_ep_rsize && epUse(s_ep_rbuf, s_ep_rsize, from_store ? 1 : 2)) {
        s_ep_rbuf = nullptr;
        if (!from_store) epSaveStart();   // the card's copy: keep it on the store
        return;
      }
      Serial.printf("[EMOJI] %s isn't the emoji pack this firmware reads\n",
                    from_store ? kEpPath : "the SD card's copy");
      if (from_store && s_ep_rfs) s_ep_rfs->remove(kEpPath);   // fetched again below
      free(s_ep_rbuf);
      s_ep_rbuf = nullptr;
      if (epOpenNext()) return;
      if (s_ep_state == 1) s_ep_state = 3;
      return;
    }
#if EMOJI_PACK_DOWNLOAD
    case 3:   // wanted: download once Wi-Fi is up
      if ((long)(now - s_ep_next_ms) < 0 || firmwareReleaseN() < 0) return;
      if (WiFi.status() != WL_CONNECTED || (uint32_t)WiFi.localIP() == 0) return;
      if (!epRoomFor(emojipack::kMaxBytes)) { s_ep_state = 5; return; }
      s_ep_dl = (uint8_t*)epAlloc(emojipack::kMaxBytes);
      if (!s_ep_dl) { s_ep_next_ms = now + 60000; return; }
      s_ep_dl_len = 0;
      s_ep_dl_done = false;
      s_ep_state = 4;
      ensureTileFetchTaskRunning();
      s_ep_dl_request = true;
      return;
    case 4: {
      if (!s_ep_dl_done) return;
      s_ep_dl_done = false;
      uint8_t* b = s_ep_dl;
      s_ep_dl = nullptr;
      if (s_ep_dl_ok && b && epUse(b, s_ep_dl_len, 3)) {
        epSaveStart();
        if (g_lv.task) g_lv.task->showAlert(TR("Full emoji set installed"), 2200);
        return;
      }
      Serial.printf("[EMOJI] download failed: %s\n", s_ep_dl_ok ? "not the expected file" : s_ep_dl_msg);
      free(b);
      s_ep_state = 3;
      s_ep_next_ms = now + 10UL * 60UL * 1000UL;   // try again in 10 min
      return;
    }
#endif
    default:
      return;
  }
}

// For About: whether the full set is here, and from where.
static int emojiPackText(char* buf, size_t cap) {
  if (!cap) return 0;
  const char* st;
  char tmp[64];
  if (s_ep_ok) {
    snprintf(tmp, sizeof tmp, "%u emoji (%s)", (unsigned)s_ep.count,
             s_ep_src == 1 ? "stored" : s_ep_src == 2 ? "from SD" : "downloaded");
    st = tmp;
  } else {
    st = s_ep_state == 4 ? "downloading" : s_ep_state == 5 ? "no room in PSRAM" :
         s_ep_state == 3 ? (EMOJI_PACK_DOWNLOAD ? "waiting for Wi-Fi" : "not installed") : "loading";
  }
  const int n = snprintf(buf, cap, "Full emoji set: %s\n\n", st);
  return n < 0 ? 0 : (n >= (int)cap ? (int)cap - 1 : n);
}
#else
#define EMOJI_PACK 0
#define EMOJI_PACK_DOWNLOAD 0
static void emojiPackTick(uint32_t) {}
static int emojiPackText(char*, size_t) { return 0; }
#endif
