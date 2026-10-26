// SPDX-License-Identifier: GPL-3.0-or-later
// ---- Linked pictures ---------------------------------------------------------------
// Someone posts a link to a picture (https://.../cat.jpg, .png, .gif, .webp, .bmp) in a
// channel or a DM. With Settings > General > Linked images on, the radio downloads it
// over Wi-Fi and the bubble shows it above the text; a tap on it opens it full size.
// Off by default: fetching a link tells that server this radio's address.
//
// One picture at a time, on a worker (the shared audio worker when it's free, as the
// web reader does: TLS needs its 16 KB stack in internal RAM), never the UI loop:
//   - get:  download (at most kLiMaxBytes), keep the file on the SD card
//           (/linkimg/<id>.img) when there is one, decode it small for the bubble;
//   - view: the kept file (or download it again), decoded to fit kLiViewSide.
// The bubble's thumbnail goes in the photo store beside the mesh photos' (VoxPhotos
// saveThumb, under the link's id), so the chat lays it out the same way.
// Decoding: helpers/LinkImage (stb_image, simplewebp). A JPEG too big for that in
// this radio's memory is decoded scaled (TJpgDec) on the UI side instead.
// Included in UITask.cpp after ChatBubbleDisplay.

#if CAP_LINK_IMAGES
#include "../helpers/LinkImage.h"

static fs::FS* backupSdFs();   // (with the Backups page) the SD card, mounted on demand

#if defined(HAS_TDISPLAY_P4) || defined(HAS_CROWPANEL_P4_101) || defined(HAS_TANMATSU)
static constexpr size_t kLiMaxBytes = 8u << 20;   // 32 MB of PSRAM
#else
static constexpr size_t kLiMaxBytes = 3u << 20;
#endif
static constexpr int kLiViewSide = 1024;                      // the viewer's own limit
static constexpr int kLiThumbSide = 2 * VoxPhotos::kThumbMax;  // saveThumb averages it down
static constexpr size_t kLiKeepFree = 1536u * 1024u;          // PSRAM left for everything else

// Settings > General > Linked images.
static int8_t s_li_on = -1;
static bool linkImgOn() {
  if (s_li_on < 0) {
    uint8_t v = 0;
    s_li_on = (touchPrefsGetBlob("linkimg", &v, 1) == 1 && v) ? 1 : 0;
  }
  return s_li_on == 1;
}
static void linkImgToggleCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  const bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  s_li_on = on ? 1 : 0;
  const uint8_t v = on ? 1 : 0;
  touchPrefsSetBlob("linkimg", &v, 1);
  ++s_chat_thumb_gen;   // the open chat lays out again: pictures in or out
  g_lv.dirty_timeline = true;
}

static bool linkImgWifiUp() { return WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0; }

// Links waiting to be fetched (newest last), and the ones tried since boot.
struct LiWant { uint32_t id; char url[240]; };
static LiWant   s_li_want[6];
static uint8_t  s_li_want_n = 0;
static uint32_t s_li_tried[64];
static uint8_t  s_li_tried_at = 0;
static uint32_t s_li_retry_ms = 0;   // a failure with no Wi-Fi: not before this

static bool linkImgTried(uint32_t id) {
  for (uint32_t t : s_li_tried) if (t == id) return true;
  return false;
}
static void linkImgWant(uint32_t id, const char* url) {
  if (linkImgTried(id)) return;
  for (uint8_t i = 0; i < s_li_want_n; i++) if (s_li_want[i].id == id) return;
  if (s_li_want_n == sizeof s_li_want / sizeof s_li_want[0]) {   // full: the oldest goes
    memmove(&s_li_want[0], &s_li_want[1], sizeof s_li_want[0] * (s_li_want_n - 1));
    --s_li_want_n;
  }
  s_li_want[s_li_want_n].id = id;
  snprintf(s_li_want[s_li_want_n].url, sizeof s_li_want[0].url, "%s", url);
  ++s_li_want_n;
}

// The job on the worker. state: 0 free, 1 running, 2 done (the UI collects it).
struct LiJob {
  volatile uint8_t state = 0;
  bool view = false;
  uint32_t id = 0;
  char url[240] = "";
  char title[48] = "";
  fs::FS* sd = nullptr;   // where files are kept (nullptr: no card)
  // Results.
  linkimg::Result res = linkimg::kOk;
  int http = 0;           // the HTTP status, or < 0 without a connection; 0 read from the card
  uint16_t* px = nullptr; // view: lvglPsramAlloc'd, else malloc'd
  int w = 0, h = 0;
  uint8_t* raw = nullptr; // a JPEG too big to decode here: the UI tries it scaled
  size_t raw_len = 0;
};
static LiJob s_li_job;

class LiSink final : public Stream {
 public:
  LiSink(uint8_t* d, size_t cap) : d_(d), cap_(cap) {}
  using Print::write;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* d, size_t n) override {
    const size_t room = n_ < cap_ ? cap_ - n_ : 0;
    if (n > room) over_ = true;
    const size_t k = n < room ? n : room;
    memcpy(d_ + n_, d, k);
    n_ += k;
    return k;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t size() const { return n_; }
  bool over() const { return over_; }

 private:
  uint8_t* d_;
  size_t cap_;
  size_t n_ = 0;
  bool over_ = false;
};

static void linkImgPath(uint32_t id, char* p, size_t cap) {
  snprintf(p, cap, "/linkimg/%08lx.img", (unsigned long)id);
}

static void* linkImgThumbAlloc(size_t n) { return malloc(n); }
static void* linkImgViewAlloc(size_t n) { return lvglPsramAlloc(n); }

// On the worker: no LVGL here.
static void linkImgJobBody() {
  LiJob& j = s_li_job;
  uint8_t* raw = nullptr;
  size_t len = 0;
  char path[40];
  linkImgPath(j.id, path, sizeof path);

  if (j.view && j.sd && j.sd->exists(path)) {   // kept from when it was fetched
    File f = j.sd->open(path, "r");
    const size_t n = f ? f.size() : 0;
    if (n && n <= kLiMaxBytes && (raw = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))
      len = f.read(raw, n);
    if (f) f.close();
    if (raw && len != n) { free(raw); raw = nullptr; len = 0; }
    j.http = 0;
  }
  if (!raw) {
    raw = (uint8_t*)heap_caps_malloc(kLiMaxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) { j.res = linkimg::kNoMemory; j.state = 2; return; }
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(9000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("wadamesh");
    WiFiClientSecure scli;
    WiFiClient pcli;
    const bool https = !strncasecmp(j.url, "https://", 8);
    bool began;
    if (https) { scli.setInsecure(); began = http.begin(scli, j.url); }
    else began = http.begin(pcli, j.url);
    int code = -1;
    bool over = false;
    if (began) {
      http.addHeader("Accept", "image/jpeg,image/png,image/gif,image/webp,image/bmp;q=0.9,*/*;q=0.5");
      http.addHeader("Accept-Encoding", "identity");
      code = http.GET();
      if (code == HTTP_CODE_OK) {
        const int clen = http.getSize();
        if (clen > 0 && (size_t)clen > kLiMaxBytes) {
          over = true;
        } else {
          LiSink sink(raw, kLiMaxBytes);
          http.writeToStream(&sink);
          len = sink.size();
          over = sink.over();
        }
      }
    }
    http.end();
    j.http = code;
    if (over) { free(raw); j.res = linkimg::kTooLarge; j.state = 2; return; }
    if (code != HTTP_CODE_OK || !len) { free(raw); j.res = linkimg::kBroken; j.state = 2; return; }
    if (linkimg::sniff(raw, len) == linkimg::kUnknown) { free(raw); j.res = linkimg::kNotPicture; j.state = 2; return; }
    if (uint8_t* fit = (uint8_t*)heap_caps_realloc(raw, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) raw = fit;
    if (j.sd) {   // keep it for viewing
      j.sd->mkdir("/linkimg");
      File f = j.sd->open(path, "w");
      if (f) {
        const bool ok = f.write(raw, len) == len;
        f.close();
        if (!ok) j.sd->remove(path);
      }
    }
  }

  const size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t budget = free_ps > kLiKeepFree ? free_ps - kLiKeepFree : 0;
  j.res = linkimg::decode(raw, len, j.view ? kLiViewSide : kLiThumbSide, budget, 0x2104,
                          j.view ? linkImgViewAlloc : linkImgThumbAlloc, &j.px, &j.w, &j.h);
  if (j.res == linkimg::kTooLarge && linkimg::sniff(raw, len) == linkimg::kJpeg) {
    j.raw = raw;   // the UI decodes it scaled
    j.raw_len = len;
  } else {
    free(raw);
  }
  j.state = 2;
}
static void linkImgJobTaskFn(void*) { linkImgJobBody(); vTaskDelete(nullptr); }
static void linkImgJobWorkerFn(void*) { linkImgJobBody(); }

static bool linkImgStart(bool view, uint32_t id, const char* url, const char* title) {
  LiJob& j = s_li_job;
  if (j.state != 0) return false;
  j = LiJob();
  j.view = view;
  j.id = id;
  snprintf(j.url, sizeof j.url, "%s", url ? url : "");
  snprintf(j.title, sizeof j.title, "%s", title ? title : "");
  j.sd = backupSdFs();
  j.state = 1;
  if (!VoxAudioTask::run(linkImgJobWorkerFn, nullptr) &&
      xTaskCreatePinnedToCore(linkImgJobTaskFn, "linkimg", 16384, nullptr, 2, nullptr, 0) != pdPASS) {
    j.state = 0;
    return false;
  }
  return true;
}

static const char* linkImgWhy(const LiJob& j) {
  if (j.http < 0) return "No connection to the picture's server";
  if (j.http > 0 && j.http != HTTP_CODE_OK) return "The picture's server said no";
  return linkimg::resultText(j.res);
}

// Opens a linked picture full size: from the card, or downloaded now.
static void linkImgView(const char* url) {
  if (!url || !url[0] || !g_lv.task) return;
  char path[40];
  const uint32_t id = linkimg::urlId(url);
  linkImgPath(id, path, sizeof path);
  fs::FS* sd = backupSdFs();
  if (!(sd && sd->exists(path)) && !linkImgWifiUp()) {
    g_lv.task->showAlert(TR("Wi-Fi not connected (Settings \xe2\x86\x92 Wi-Fi)"), 2000);
    return;
  }
  const char* slash = strrchr(url, '/');
  char title[48];
  snprintf(title, sizeof title, "%s", slash && slash[1] ? slash + 1 : "picture");
  if (char* q = strpbrk(title, "?#")) *q = 0;
  if (!linkImgStart(true, id, url, title)) {
    g_lv.task->showAlert(TR("Busy, try again in a moment"), 1500);
    return;
  }
  g_lv.task->showAlert(TR("Opening picture..."), 1500);
}

// From the UI loop: collects a finished job, then starts the next download.
static void linkImgTick() {
  LiJob& j = s_li_job;
  if (j.state == 1) return;
  if (j.state == 2) {
    const uint32_t id = j.id;
    if (j.raw) {   // a JPEG too big for the full decode: scaled by TJpgDec
      int w = 0, h = 0;
      uint8_t* rgb = decodeJpegScaledToRgb565(j.raw, j.raw_len, &w, &h, j.view ? kLiViewSide : kLiThumbSide);
      free(j.raw);
      j.raw = nullptr;
      if (rgb && w > 0 && h > 0) {
        if (j.view) { j.px = (uint16_t*)rgb; }
        else {
          const size_t n = (size_t)w * h * 2;
          j.px = (uint16_t*)malloc(n);
          if (j.px) memcpy(j.px, rgb, n);
          lvglPsramFree(rgb);
        }
        j.w = w;
        j.h = h;
        j.res = j.px ? linkimg::kOk : linkimg::kNoMemory;
      } else if (rgb) {
        lvglPsramFree(rgb);
      }
    }
    if (j.view) {
      if (j.res == linkimg::kOk && j.px) {
        s_fm_img_path[0] = '\0';   // not a file the viewer can set as a wallpaper
        s_fm_img_on_sd = false;
        fmImageShowDecoded(j.title, (uint8_t*)j.px, j.w, j.h);   // the viewer owns the pixels now
      } else {
        if (j.px) lvglPsramFree(j.px);
        if (g_lv.task) g_lv.task->showAlert(TR(linkImgWhy(j)), 2200);
      }
    } else {
      if (j.res == linkimg::kOk && j.px && VoxPhotos::saveThumb(id, j.px, j.w, j.h)) voxThumbLanded();
      if (j.res != linkimg::kOk) Serial.printf("[LINKIMG] %08lx: %s (http %d)\n", (unsigned long)id, linkImgWhy(j), j.http);
      free(j.px);
      // No Wi-Fi when it ran: try again later rather than never.
      if (j.http < 0 && !linkImgWifiUp()) {
        linkImgWant(id, j.url);
        s_li_retry_ms = millis() + 30000;
      } else {
        s_li_tried[s_li_tried_at++ % (sizeof s_li_tried / sizeof s_li_tried[0])] = id;
      }
    }
    j.px = nullptr;
    j.state = 0;
    return;
  }
  if (!s_li_want_n || !linkImgOn() || !linkImgWifiUp()) return;
  if (s_li_retry_ms && (int32_t)(millis() - s_li_retry_ms) < 0) return;
  s_li_retry_ms = 0;
  const LiWant w = s_li_want[--s_li_want_n];   // newest first: the one on screen
  if (VoxPhotos::thumbSize(w.id, nullptr, nullptr)) return;
  if (!linkImgStart(false, w.id, w.url, nullptr)) linkImgWant(w.id, w.url);
}

// The bubble: a linked picture that's here shows above the text; one that
// isn't is asked for.
static void linkImgBubble(const char* text, ChatBubbleDisplay& d) {
  if (!linkImgOn() || !text) return;
  char url[sizeof d.thumb_url];
  if (!linkimg::findUrl(text, url, sizeof url)) return;
  const uint32_t id = linkimg::urlId(url);
  int tw = 0, th = 0;
  if (VoxPhotos::thumbSize(id, &tw, &th)) {
    d.thumb_sid = id;
    d.thumb_w = (lv_coord_t)tw;
    d.thumb_h = (lv_coord_t)th;
    d.thumb_link = true;
    snprintf(d.thumb_url, sizeof d.thumb_url, "%s", url);
  } else {
    linkImgWant(id, url);
  }
}

// Its thumbnail in the bubble; a tap opens the picture.
static void linkImgThumbEventCb(lv_event_t* e) {
  char* url = (char*)lv_event_get_user_data(e);
  if (lv_event_get_code(e) == LV_EVENT_DELETE) {
    lv_img_dsc_t* dsc = (lv_img_dsc_t*)lv_img_get_src(lv_event_get_target(e));
    if (dsc) { free((void*)dsc->data); free(dsc); }
    free(url);
    return;
  }
  if (lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED && url) linkImgView(url);
}
static void linkImgAddThumb(lv_obj_t* parent, const ChatBubbleDisplay& d, lv_coord_t x, lv_coord_t y) {
  int w = 0, h = 0;
  uint16_t* px = nullptr;
  if (!VoxPhotos::loadThumb(d.thumb_sid, &w, &h, &px)) return;   // the row keeps its room
  lv_img_dsc_t* dsc = (lv_img_dsc_t*)calloc(1, sizeof(lv_img_dsc_t));
  char* url = strdup(d.thumb_url);
  if (!dsc || !url) { free(px); free(dsc); free(url); return; }
  dsc->header.always_zero = 0;
  dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
  dsc->header.w = (uint32_t)w;
  dsc->header.h = (uint32_t)h;
  dsc->data_size = (uint32_t)w * (uint32_t)h * 2u;
  dsc->data = (const uint8_t*)px;
  lv_obj_t* img = lv_img_create(parent);
  lv_img_set_src(img, dsc);
  lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(img, LV_OBJ_FLAG_SCROLL_CHAIN);
  lv_obj_set_pos(img, x, y);
  lv_obj_set_style_radius(img, 4, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(img, true, LV_PART_MAIN);
  lv_obj_add_event_cb(img, linkImgThumbEventCb, LV_EVENT_ALL, url);
}

#endif  // CAP_LINK_IMAGES
