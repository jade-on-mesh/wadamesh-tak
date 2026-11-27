// SPDX-License-Identifier: GPL-3.0-or-later
// The full emoji set (EMOJI.md): upstream's emoji pack (EmojiPack.cpp, emoji.pack,
// picker tabs and sequences) with the fork's ways of getting it, and its names for
// the picker's search (emoji-names.txt, scripts/build/make-emoji-names.py).
//
// The pack, from the first of:
//   1. the copy kept on the map tiles' internal LittleFS partition (/emoji.pack);
//   2. the SD card's or the profile store's, under /emoji/ or the root (upstream's
//      places), then copied onto the tiles partition;
//   3. a Wi-Fi download from this firmware's own release (main's if the release
//      has none), saved onto the tiles partition and read from there.
// The names come the same way, as /emoji-names.txt (/emoji/emoji-names.txt on a card).
// Never kept on SPIFFS: the chat history and contacts live there, and every write
// to a fuller SPIFFS makes its garbage collection, which stalls both cores, slower.
// With no tiles partition a download goes to the SD card, else it can't be kept.
//
// Upstream's loader reads the pack into PSRAM a slice per loop pass (UITask's
// emojiPackPump); copies, saves and the names are a chunk per pass here too, so
// none of this ever holds the screen. Included into UITask.cpp before the fetch
// worker; emojiPackTick() runs from the UI loop.

#if CAP_EMOJI_PACK
#define EMOJI_PACK 1

static void voxThumbLanded();          // re-lays out the open chat (emoji widths changed)
static fs::FS* uiDataFsForEmoji();     // the profile store (upstream's place), defined with it

static constexpr const char* kEpPath   = "/emoji.pack";
static constexpr const char* kEnPath   = "/emoji-names.txt";
static constexpr const char* kEpOldWep = "/emoji.wep";   // an earlier build's pack (another format)
static constexpr size_t kEpChunk = 16 * 1024;
static constexpr size_t kEpMaxBytes = 2u * 1024u * 1024u;   // EmojiPack.cpp's kMaxFile
// What a download is fetched into: the pack is about 1.06 MB, so 2 MB (with the 1.5 MB
// kept free on top) asked for more PSRAM than a T-Deck with its map and photos open has.
static constexpr size_t kEpDlBytes = 1536u * 1024u;
static constexpr size_t kEnMaxBytes = 256u * 1024u;

// The pack: 0 start, 1 being read (upstream's pump), 2 in use, 3 wanted (download
// when Wi-Fi is up), 4 downloading, 5 off (no room in PSRAM), 6 nowhere to keep a download,
// 7 making room for a download on the tiles partition (map tiles removed, fetched again).
// 5 and 6 look again every 10 minutes; Settings > Keyboard > Full emoji set downloads now.
static uint8_t  s_ep_state = 0;
static bool     s_ep_force = false;      // "Download now": even a build with no release
static bool     s_ep_notice = false;     // the after-an-update notice, once a boot
static char     s_ep_err[80] = "";       // why the last download didn't land
static uint8_t* s_ep_hold = nullptr;     // a download waiting for room (state 7)
static size_t   s_ep_hold_len = 0;
static uint8_t  s_ep_src = 0;          // where it came from: 1 kept, 2 SD or store, 3 download
static uint8_t  s_ep_rsrc = 0;         // the one being read
static fs::FS*  s_ep_rfs = nullptr;
static const char* s_ep_rpath = nullptr;
static int      s_ep_next = 0;         // the next place to look
static uint32_t s_ep_next_ms = 0;

// The names: 0 waiting for the pack, 1 reading, 2 in use, 3 wanted, 4 downloading, 5 none
static uint8_t  s_en_state = 0;
static int      s_en_next = 0;
static File     s_en_rf;
static fs::FS*  s_en_rfs = nullptr;
static bool     s_en_from_store = false;
static char*    s_en_buf = nullptr;
static size_t   s_en_len = 0, s_en_got = 0;
static const char** s_en_utf8 = nullptr;   // per emoji: the text to insert
static const char** s_en_text = nullptr;   // and "name|keywords"
static int      s_en_count = 0;
static uint32_t s_en_next_ms = 0;

// A file being written onto the store (or the card), a chunk per pass: from memory
// (a download, the names) or from another file (the card's pack).
static bool     s_ep_job = false;
static fs::FS*  s_ep_jfs = nullptr;
static const char* s_ep_jpath = nullptr;
static File     s_ep_jout, s_ep_jin;
static const uint8_t* s_ep_jbuf = nullptr;
static size_t   s_ep_jlen = 0, s_ep_jpos = 0;
static uint8_t* s_ep_jchunk = nullptr;
static void   (*s_ep_jdone)(bool ok) = nullptr;
static uint8_t* s_ep_dl_kept = nullptr;   // a downloaded pack, freed once saved

// The fetch worker (EMOJI_PACK_DOWNLOAD): one file at a time.
#if defined(MULTI_TRANSPORT_COMPANION) && WADA_FORK_RELEASES
#define EMOJI_PACK_DOWNLOAD 1
static volatile bool s_ep_dl_request = false;     // UI -> worker
static volatile bool s_ep_dl_done = false;        // worker -> UI
static volatile bool s_ep_dl_ok = false;
static uint8_t* volatile s_ep_dl = nullptr;
static volatile size_t s_ep_dl_len = 0;
static volatile size_t s_ep_dl_max = 0;
static const char* volatile s_ep_dl_path = nullptr;   // in the repo: the pack, or the names
static char s_ep_dl_msg[112];
static constexpr const char* kEpRepoPath = "deploy/site/emoji/emoji.pack";
static constexpr const char* kEnRepoPath = "emoji/emoji-names.txt";
#else
#define EMOJI_PACK_DOWNLOAD 0
#endif

static void* epAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
// PSRAM for `n` bytes, with some left for the map and photos (less when asked by
// "Download now"). What it last wanted and found is kept for the status line.
static size_t s_ep_need = 0, s_ep_free = 0;
static bool epRoomFor(size_t n) {
  const size_t spare = s_ep_force ? 256u * 1024u : 768u * 1024u;
  s_ep_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s_ep_need = n + spare;
  return s_ep_free >= s_ep_need && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= n;
}

// Where things are kept: the tiles partition (LittleFS), else the SD card for a
// download, else nowhere.
static fs::FS* epStoreFs() { return s_tiles_fs_ready ? static_cast<fs::FS*>(&s_tiles_fs) : nullptr; }
// The SD card when it's mounted, else null.
static fs::FS* epCard() {
#if CAP_SD || defined(TLORA_PAGER)
  if (sdAdoptLiveMount()) return &SD;
#endif
  return nullptr;
}
static bool epRoomOnStore(size_t n) {
  if (!epStoreFs()) return false;
  const size_t total = s_tiles_fs.totalBytes(), used = s_tiles_fs.usedBytes();
  return total > used && total - used >= n + 64u * 1024u;
}

// Map tiles off the tiles partition to make room for the pack: up to `budget` files of
// the most zoomed-in level left (the most numerous, and fetched again when shown),
// emptying and removing directories as they go. False when there are none left.
static bool epFreeSomeTiles(int budget) {
  fs::FS* fs = epStoreFs();
  if (!fs) return false;
  WdtHeavyGuard guard;
  char zdir[40] = "";
  long best = -1;
  for (const char* root : { "/tiles/topo", "/tiles" }) {
    File r = fs->open(root);
    if (!r || !r.isDirectory()) continue;
    for (File e = r.openNextFile(); e; e = r.openNextFile()) {
      const char* n = strrchr(e.path(), '/');
      n = n ? n + 1 : e.path();
      char* end = nullptr;
      const long z = strtol(n, &end, 10);
      if (e.isDirectory() && end && end != n && *end == '\0' && z > best) {
        best = z;
        snprintf(zdir, sizeof zdir, "%s", e.path());
      }
    }
  }
  if (best < 0) return false;
  char xdir[64] = "";
  {
    File z = fs->open(zdir);
    File x = z ? z.openNextFile() : File();
    if (!x) { z.close(); fs->rmdir(zdir); return true; }
    snprintf(xdir, sizeof xdir, "%s", x.path());
    if (!x.isDirectory()) { x.close(); z.close(); fs->remove(xdir); return true; }
  }
  char victims[24][72];
  int n = 0;
  {
    File x = fs->open(xdir);
    for (File f = x.openNextFile(); f && n < budget && n < 24; f = x.openNextFile())
      if (!f.isDirectory()) snprintf(victims[n++], sizeof victims[0], "%s", f.path());
  }
  for (int i = 0; i < n; ++i) fs->remove(victims[i]);
  if (!n) fs->rmdir(xdir);
  return true;
}

// An earlier build's emoji-pack.wep: not read any more, so give its ~900 KB back.
static void epDropOldWep() {
  WdtHeavyGuard guard;
  if (fs::FS* fs = epStoreFs()) if (fs->exists(kEpOldWep)) fs->remove(kEpOldWep);
#if !defined(HAS_TDISPLAY_P4) && !defined(HAS_CROWPANEL_P4_101)
  if (SPIFFS.exists(kEpOldWep)) SPIFFS.remove(kEpOldWep);
#endif
}

// ---- writing a file a chunk at a time ----------------------------------------------
static bool epJobStart(fs::FS* fs, const char* path, const uint8_t* buf, size_t len, File in,
                       void (*done)(bool)) {
  if (!fs || s_ep_job) { if (in) in.close(); return false; }
  char tmp[40];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  fs->remove(tmp);
  s_ep_jout = fs->open(tmp, FILE_WRITE);
  if (!s_ep_jout) { if (in) in.close(); return false; }
  if (in && !s_ep_jchunk) s_ep_jchunk = (uint8_t*)epAlloc(kEpChunk);
  if (in && !s_ep_jchunk) { s_ep_jout.close(); fs->remove(tmp); in.close(); return false; }
  s_ep_jfs = fs;
  s_ep_jpath = path;
  s_ep_jbuf = buf;
  s_ep_jlen = in ? in.size() : len;
  s_ep_jpos = 0;
  s_ep_jin = in;
  s_ep_jdone = done;
  s_ep_job = true;
  return true;
}

static void epJobStep() {
  const size_t k = s_ep_jlen - s_ep_jpos < kEpChunk ? s_ep_jlen - s_ep_jpos : kEpChunk;
  bool ok = true;
  if (k) {
    WdtHeavyGuard guard;
    const uint8_t* src = s_ep_jbuf ? s_ep_jbuf + s_ep_jpos : s_ep_jchunk;
    if (!s_ep_jbuf) ok = s_ep_jin.read(s_ep_jchunk, k) == k;
    ok = ok && s_ep_jout.write(src, k) == k;
  }
  s_ep_jpos += k;
  if (ok && s_ep_jpos < s_ep_jlen) return;
  s_ep_jout.close();
  if (s_ep_jin) s_ep_jin.close();
  s_ep_job = false;
  char tmp[40];
  snprintf(tmp, sizeof tmp, "%s.tmp", s_ep_jpath);
  {
    WdtHeavyGuard guard;
    if (ok) {
      s_ep_jfs->remove(s_ep_jpath);
      ok = s_ep_jfs->rename(tmp, s_ep_jpath);
    }
    if (!ok) s_ep_jfs->remove(tmp);
  }
  if (s_ep_jdone) s_ep_jdone(ok);
}

// ---- the pack ------------------------------------------------------------------------
// Hands the next place the pack may be to upstream's loader. False when none is left.
static bool epBeginNext() {
  for (;;) {
    fs::FS* fs = nullptr;
    const char* path = nullptr;
    uint8_t src = 2;
    switch (s_ep_next++) {
      case 0: fs = epStoreFs(); path = kEpPath; src = 1; break;
      case 1: fs = epCard(); path = "/emoji/emoji.pack"; break;
      case 2: fs = epCard(); path = "/emoji.pack"; break;
      case 3: fs = uiDataFsForEmoji(); path = "/emoji/emoji.pack"; break;
      case 4: fs = uiDataFsForEmoji(); path = "/emoji.pack"; break;
      default: return false;
    }
    if (!fs || !path || !fs->exists(path)) continue;
    File f = fs->open(path, FILE_READ);
    if (!f) continue;
    if (!epRoomFor(f.size())) { f.close(); s_ep_state = 5; return false; }
    if (!emojiPackBegin(f)) continue;   // it closes f
    s_ep_rfs = fs;
    s_ep_rpath = path;
    s_ep_rsrc = src;
    return true;
  }
}

static void epPackKept(bool ok) {
  pushDiagLine(ok ? "emoji: pack kept on internal storage" : "emoji: couldn't keep the pack");
}

// A downloaded pack saved: read it from where it was saved.
static void epDownloadSaved(bool ok) {
  free(s_ep_dl_kept);
  s_ep_dl_kept = nullptr;
  if (ok) {
    File f = s_ep_jfs->open(s_ep_jpath, FILE_READ);
    if (f && emojiPackBegin(f)) {
      s_ep_rfs = s_ep_jfs;
      s_ep_rpath = s_ep_jpath;
      s_ep_rsrc = 3;
      s_ep_state = 1;
      return;
    }
  }
  pushDiagLine("emoji: couldn't save the downloaded pack");
  s_ep_state = 3;
  s_ep_next_ms = millis() + 10UL * 60UL * 1000UL;   // try again in 10 min
}

// ---- the names (search) ----------------------------------------------------------------
// Takes `buf` (len bytes, NUL after them) into use: one emoji a line, "utf8 TAB text".
static bool enUse(char* buf, size_t len) {
  int n = 0;
  for (size_t i = 0; i < len; ++i) n += buf[i] == '\n';
  if (n <= 0) return false;
  const char** u = (const char**)epAlloc(sizeof(char*) * (size_t)n);
  const char** t = (const char**)epAlloc(sizeof(char*) * (size_t)n);
  if (!u || !t) { free(u); free(t); return false; }
  int k = 0;
  for (char* p = buf; p < buf + len && k < n;) {
    char* nl = (char*)memchr(p, '\n', (size_t)(buf + len - p));
    if (!nl) break;
    *nl = '\0';
    char* tab = strchr(p, '\t');
    if (tab && tab > p && tab[1]) {
      *tab = '\0';
      u[k] = p;
      t[k] = tab + 1;
      ++k;
    }
    p = nl + 1;
  }
  if (!k) { free(u); free(t); return false; }
  s_en_buf = buf;
  s_en_len = len;
  s_en_utf8 = u;
  s_en_text = t;
  s_en_count = k;
  s_en_state = 2;
  return true;
}

static void enSaved(bool ok) {
  if (!ok) pushDiagLine("emoji: couldn't keep the search names");
}

// Opens the next place the names may be. False when none is left.
static bool enOpenNext() {
  for (;;) {
    fs::FS* fs = nullptr;
    const char* path = nullptr;
    switch (s_en_next++) {
      case 0: fs = epStoreFs(); path = kEnPath; break;
      case 1: fs = epCard(); path = "/emoji/emoji-names.txt"; break;
      case 2: fs = epCard(); path = kEnPath; break;
      case 3: fs = uiDataFsForEmoji(); path = "/emoji/emoji-names.txt"; break;
      case 4: fs = uiDataFsForEmoji(); path = kEnPath; break;
      default: return false;
    }
    if (!fs || !path || !fs->exists(path)) continue;
    File f = fs->open(path, FILE_READ);
    if (!f) continue;
    const size_t sz = f.size();
    if (!sz || sz > kEnMaxBytes) { f.close(); continue; }
    char* b = (char*)epAlloc(sz + 1);
    if (!b) { f.close(); return false; }
    s_en_rf = f;
    s_en_rfs = fs;
    s_en_from_store = fs == epStoreFs() && fs != nullptr;
    s_en_buf = b;
    s_en_len = sz;
    s_en_got = 0;
    return true;
  }
}

static void enTick(uint32_t now) {
  switch (s_en_state) {
    case 0:
      s_en_next = 0;
      s_en_state = enOpenNext() ? 1 : (EMOJI_PACK_DOWNLOAD ? 3 : 5);
      return;
    case 1: {
      const size_t k = s_en_len - s_en_got < kEpChunk ? s_en_len - s_en_got : kEpChunk;
      const int r = k ? s_en_rf.read((uint8_t*)s_en_buf + s_en_got, k) : 0;
      if (r > 0) s_en_got += (size_t)r;
      if (r > 0 && s_en_got < s_en_len) return;
      s_en_rf.close();
      char* b = s_en_buf;
      s_en_buf = nullptr;
      if (s_en_got == s_en_len) {
        b[s_en_len] = '\0';
        if (enUse(b, s_en_len)) {
          // The card's or the profile store's copy: keep it on the tiles partition too.
          if (!s_en_from_store && epRoomOnStore(s_en_len))
            epJobStart(epStoreFs(), kEnPath, (const uint8_t*)s_en_buf, s_en_len, File(), enSaved);
          return;
        }
      }
      free(b);
      if (s_en_from_store && s_en_rfs) { WdtHeavyGuard guard; s_en_rfs->remove(kEnPath); }
      if (enOpenNext()) return;
      s_en_state = EMOJI_PACK_DOWNLOAD ? 3 : 5;
      return;
    }
#if EMOJI_PACK_DOWNLOAD
    case 3:   // wanted: download once Wi-Fi is up and the worker is free
      if ((long)(now - s_en_next_ms) < 0 || firmwareReleaseN() < 0 || s_ep_state == 4) return;
      if (WiFi.status() != WL_CONNECTED || (uint32_t)WiFi.localIP() == 0) return;
      s_ep_dl = (uint8_t*)epAlloc(kEnMaxBytes + 1);
      if (!s_ep_dl) { s_en_next_ms = now + 60000; return; }
      s_ep_dl_len = 0;
      s_ep_dl_max = kEnMaxBytes;
      s_ep_dl_path = kEnRepoPath;
      s_ep_dl_done = false;
      s_en_state = 4;
      ensureTileFetchTaskRunning();
      s_ep_dl_request = true;
      return;
    case 4: {
      if (!s_ep_dl_done) return;
      s_ep_dl_done = false;
      char* b = (char*)s_ep_dl;
      s_ep_dl = nullptr;
      const size_t len = s_ep_dl_len;
      if (s_ep_dl_ok && b && len) {
        b[len] = '\0';
        if (enUse(b, len)) {
          if (epRoomOnStore(len))
            epJobStart(epStoreFs(), kEnPath, (const uint8_t*)s_en_buf, s_en_len, File(), enSaved);
          return;
        }
      }
      free(b);
      s_en_state = 3;
      s_en_next_ms = now + 10UL * 60UL * 1000UL;
      return;
    }
#endif
    default:
      return;
  }
}

// ---- the loop --------------------------------------------------------------------------
// A download saved where it fits: the tiles partition, else the SD card. False when
// neither has room (state 7 then makes room on the partition).
static bool epKeepDownload(uint8_t* b, size_t len) {
  fs::FS* card = epCard();
  fs::FS* keep = epRoomOnStore(len) ? epStoreFs() : card;
  if (!keep) return false;
  const char* path = keep == epStoreFs() ? kEpPath : "/emoji/emoji.pack";
  if (keep == card) { WdtHeavyGuard guard; if (!card->exists("/emoji")) card->mkdir("/emoji"); }
  if (!epJobStart(keep, path, b, len, File(), epDownloadSaved)) return false;
  s_ep_dl_kept = b;   // freed once saved
  s_ep_state = 1;     // epDownloadSaved hands it to the loader
  return true;
}

// After an update that moved the tiles partition (or on a new standalone) the map
// tiles and the pack kept there are gone: say they come back over Wi-Fi, once.
static void epNoticeIfReset() {
  if (s_ep_notice || !g_lv.task || !epStoreFs()) return;
  s_ep_notice = true;
  if (s_tiles_fs.usedBytes() > 256u * 1024u) return;   // tiles still here: not a reset
  g_lv.task->showAlert(WiFi.status() == WL_CONNECTED
                           ? TR("Map tiles and the full emoji set are downloading again")
                           : TR("Map tiles and the full emoji set download again on Wi-Fi"),
                       5000);
}

static void emojiPackTick(uint32_t now) {
  // Hold the map tiles back from the partition's last 1.2 MB while the pack wants it.
  s_ep_wants_store_room = epStoreFs() && (s_ep_state == 3 || s_ep_state == 4 || s_ep_state >= 6);
  if (s_ep_job) { epJobStep(); return; }
  switch (s_ep_state) {
    case 0:   // start a few seconds after boot, once the store and the card are up
      if (!s_ep_next_ms) { s_ep_next_ms = now + 4000; return; }
      if ((long)(now - s_ep_next_ms) < 0) return;
      epDropOldWep();
      s_ep_next = 0;
      if (epBeginNext()) s_ep_state = 1;
      else if (s_ep_state == 0) { s_ep_state = 3; s_ep_next_ms = now; epNoticeIfReset(); }
      if (s_ep_state == 5) s_ep_next_ms = now + 10UL * 60UL * 1000UL;
      return;
    case 5:   // no room in PSRAM, or
    case 6:   // nowhere to keep a download: look again (a card, freed memory) now and then
      if ((long)(now - s_ep_next_ms) < 0) return;
      s_ep_state = 0;
      s_ep_next_ms = now;
      return;
    case 7: {   // making room on the tiles partition for a download in hand
      static uint8_t passes = 0;
      if (++passes % 4) { epFreeSomeTiles(24); return; }   // check the room every 4th pass
      if (epRoomOnStore(s_ep_hold_len) || !epFreeSomeTiles(24)) {
        uint8_t* b = s_ep_hold;
        const size_t len = s_ep_hold_len;
        s_ep_hold = nullptr;
        s_ep_hold_len = 0;
        if (epKeepDownload(b, len)) return;
        free(b);
        snprintf(s_ep_err, sizeof s_ep_err, "%s", "no room on internal storage");
        s_ep_state = 6;
        s_ep_next_ms = now + 10UL * 60UL * 1000UL;
      }
      return;
    }
    case 1:   // upstream's pump reads it (UITask loop); here only the outcome
      if (emojiPackLoading()) return;
      if (emojiPackLoaded()) {
        s_ep_src = s_ep_rsrc;
        s_ep_state = 2;
        s_ep_force = false;
        if (s_ep_src == 3 && g_lv.task) g_lv.task->showAlert(TR("Full emoji set installed"), 2200);
        voxThumbLanded();   // lay out the open chat again: its emoji have widths now
        // The card's or the profile store's copy: keep it on the tiles partition.
        if (s_ep_rsrc == 2 && s_ep_rfs && epRoomOnStore(1100u * 1024u)) {
          File in = s_ep_rfs->open(s_ep_rpath, FILE_READ);
          if (in) epJobStart(epStoreFs(), kEpPath, nullptr, 0, in, epPackKept);
        }
        return;
      }
      // Not a pack this firmware reads: drop a kept copy (fetched again), try the next.
      if (s_ep_rsrc == 1 || s_ep_rsrc == 3) {
        WdtHeavyGuard guard;
        if (s_ep_rfs && s_ep_rpath) s_ep_rfs->remove(s_ep_rpath);
      }
      if (s_ep_rsrc != 3 && epBeginNext()) return;
      if (s_ep_state == 1) s_ep_state = 3;
      s_ep_next_ms = s_ep_rsrc == 3 ? now + 10UL * 60UL * 1000UL : now;
      return;
    case 2:
      enTick(now);
      return;
#if EMOJI_PACK_DOWNLOAD
    case 3: {   // wanted: download once Wi-Fi is up
      if ((long)(now - s_ep_next_ms) < 0 || (firmwareReleaseN() < 0 && !s_ep_force)) return;
      if (WiFi.status() != WL_CONNECTED || (uint32_t)WiFi.localIP() == 0) return;
      const uint32_t later = now + 10UL * 60UL * 1000UL;
      if (!epStoreFs() && !epCard()) { s_ep_state = 6; s_ep_next_ms = later; return; }
      if (!epRoomFor(kEpDlBytes)) { s_ep_state = 5; s_ep_next_ms = later; return; }
      s_ep_dl = (uint8_t*)epAlloc(kEpDlBytes);
      if (!s_ep_dl) { s_ep_next_ms = now + 60000; return; }
      s_ep_dl_len = 0;
      s_ep_dl_max = kEpDlBytes;
      s_ep_dl_path = kEpRepoPath;
      s_ep_dl_done = false;
      s_ep_state = 4;
      ensureTileFetchTaskRunning();
      s_ep_dl_request = true;
      return;
    }
    case 4: {
      if (!s_ep_dl_done) return;
      s_ep_dl_done = false;
      uint8_t* b = s_ep_dl;
      s_ep_dl = nullptr;
      const size_t len = s_ep_dl_len;
      if (s_ep_dl_ok && b && len) {
        s_ep_err[0] = '\0';
        if (epKeepDownload(b, len)) return;
        // The map tiles filled the partition (as after an update that moved it): make
        // room, a few tiles a pass, rather than give up with nowhere to keep it.
        if (epStoreFs()) {
          s_ep_hold = b;
          s_ep_hold_len = len;
          s_ep_state = 7;
          return;
        }
      }
      snprintf(s_ep_err, sizeof s_ep_err, "%s", s_ep_dl_ok ? "nowhere to keep it" : s_ep_dl_msg);
      Serial.printf("[EMOJI] download failed: %s\n", s_ep_err);
      free(b);
      s_ep_state = s_ep_dl_ok ? 6 : 3;
      s_ep_next_ms = now + 10UL * 60UL * 1000UL;   // try again in 10 min
      return;
    }
#endif
    default:
      return;
  }
}

// ---- search (the picker's 🔍) ------------------------------------------------------------
static bool emojiPackSearchable() { return s_en_state == 2 && s_en_count > 0; }

namespace epsearch {
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
inline bool wordStart(const char* text, const char* p) {
  if (p == text) return true;
  const char c = p[-1];
  return c == ' ' || c == '|' || c == ':' || c == '-' || c == '_' || c == '(' || c == '"';
}
// `word` (n chars, lowercase) starts a word in `from..to`.
inline bool hasWordPrefix(const char* text, const char* from, const char* to, const char* word, size_t n) {
  for (const char* p = from; p + n <= to; ++p)
    if (wordStart(text, p) && strncmp(p, word, n) == 0) return true;
  return false;
}
}  // namespace epsearch

// Whether this standalone can draw the emoji (baked, or in the pack): a sequence
// only as its own image, never as its first part.
static bool enDrawable(const char* utf8) {
  if (emojiPickerCompositeGlyph(utf8)) return true;
  if (strstr(utf8, "\xE2\x80\x8D")) return false;   // a ZWJ sequence the pack doesn't have
  uint32_t off = 0;
  const uint32_t lead = _lv_txt_encoded_next(utf8, &off);
  if (emojiIsRegionalIndicator(lead)) return false;   // a flag: the composite lookup above
  return emojiLookupAny(lead) != nullptr;
}

// The emoji to insert for the best matches of `query`, at most `max`. Every word of
// the query must start a word of the emoji's name or keywords ("fi tro" finds 🐠).
// The name that is the query comes first, then names starting with it, then names
// with every word, then keyword-only matches; each in Unicode's order.
static int emojiPackSearch(const char* query, const char** out, int max) {
  if (!emojiPackSearchable() || !query || !out || max <= 0) return 0;
  char q[48];
  size_t qn = 0;
  for (const char* p = query; *p && qn + 1 < sizeof q; ++p) q[qn++] = epsearch::lower(*p);
  while (qn && q[qn - 1] == ' ') --qn;
  size_t qs = 0;
  while (qs < qn && q[qs] == ' ') ++qs;
  q[qn] = 0;
  if (qs >= qn) return 0;
  const char* qq = q + qs;
  const size_t qlen = qn - qs;
  int n = 0;
  for (int tier = 0; tier < 4 && n < max; ++tier) {
    for (int i = 0; i < s_en_count && n < max; ++i) {
      const char* t = s_en_text[i];
      const char* bar = strchr(t, '|');
      const char* name_end = bar ? bar : t + strlen(t);
      const char* end = t + strlen(t);
      bool all_name = true, all_any = true;
      for (const char* w = qq; *w;) {
        while (*w == ' ') ++w;
        if (!*w) break;
        const char* we = w;
        while (*we && *we != ' ') ++we;
        const size_t wl = (size_t)(we - w);
        if (!epsearch::hasWordPrefix(t, t, name_end, w, wl)) all_name = false;
        if (!epsearch::hasWordPrefix(t, t, end, w, wl)) { all_any = false; break; }
        w = we;
      }
      if (!all_any) continue;
      const size_t nl = (size_t)(name_end - t);
      const bool starts = nl >= qlen && strncmp(t, qq, qlen) == 0;
      const int my = (starts && nl == qlen) ? 0 : starts ? 1 : all_name ? 2 : 3;
      if (my == tier && enDrawable(s_en_utf8[i])) out[n++] = s_en_utf8[i];
    }
  }
  return n;
}

// Whether the full set is here, and from where; else what it's waiting for.
static void emojiPackStatus(char* buf, size_t cap) {
  if (!cap) return;
  if (s_ep_state == 2) {
    snprintf(buf, cap, TR("%d emoji (%s)%s"), emojiPackItemCount(),
             s_ep_src == 1 ? TR("stored") : s_ep_src == 2 ? TR("from SD") : TR("downloaded"),
             emojiPackSearchable() ? "" : TR(", no search names"));
    return;
  }
  const char* st =
      s_ep_state == 4 ? TR("downloading") : s_ep_state == 5 ? TR("no room in PSRAM, trying again soon") :
      s_ep_state == 6 ? TR("nowhere to keep it, trying again soon") :
      s_ep_state == 7 ? TR("making room on internal storage") :
      s_ep_state == 3 ? (!EMOJI_PACK_DOWNLOAD ? TR("not installed")
                         : WiFi.status() != WL_CONNECTED ? TR("waiting for Wi-Fi") : TR("downloading soon"))
                      : TR("loading");
  if (s_ep_state == 5 && s_ep_need)
    snprintf(buf, cap, "%s (%.1f MB %s, %s %.1f MB)", st, s_ep_free / 1048576.0, TR("free"), TR("needs"),
             s_ep_need / 1048576.0);
  else if (s_ep_err[0] && (s_ep_state == 3 || s_ep_state == 6))
    snprintf(buf, cap, "%s (%s: %s)", st, TR("last try"), s_ep_err);
  else
    snprintf(buf, cap, "%s", st);
}

// For System Information.
static int emojiPackText(char* buf, size_t cap) {
  if (!cap) return 0;
  char st[160];
  emojiPackStatus(st, sizeof st);
  const int n = snprintf(buf, cap, "Full emoji set: %s\n\n", st);
  return n < 0 ? 0 : (n >= (int)cap ? (int)cap - 1 : n);
}

// Settings > Keyboard > Full emoji set: download now. 0 asked (it starts within a
// second), 1 already installed, 2 already under way, 3 no Wi-Fi, 4 not in this build.
static int emojiPackDownloadNow() {
#if EMOJI_PACK_DOWNLOAD
  if (s_ep_state == 2) return 1;
  if (s_ep_state == 0 || s_ep_state == 1 || s_ep_state == 4 || s_ep_state == 7 || s_ep_job) return 2;
  if (WiFi.status() != WL_CONNECTED) return 3;
  s_ep_force = true;
  s_ep_err[0] = '\0';
  // No room last time: try the copy that's kept first (it may be here already), then download.
  s_ep_state = s_ep_state == 5 ? 0 : 3;
  s_ep_next_ms = millis() | 1u;
  return 0;
#else
  return 4;
#endif
}
static bool emojiPackInstalled() { return s_ep_state == 2; }
#else
#define EMOJI_PACK 0
#define EMOJI_PACK_DOWNLOAD 0
static void emojiPackTick(uint32_t) {}
static int emojiPackText(char*, size_t) { return 0; }
static void emojiPackStatus(char* buf, size_t cap) { if (cap) buf[0] = '\0'; }
static int emojiPackDownloadNow() { return 4; }
static bool emojiPackInstalled() { return false; }
static bool emojiPackSearchable() { return false; }
static int emojiPackSearch(const char*, const char**, int) { return 0; }
#endif
