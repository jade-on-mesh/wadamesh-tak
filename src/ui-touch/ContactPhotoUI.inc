// SPDX-License-Identifier: GPL-3.0-or-later
// Contact photos (ContactPhoto.h): a circle cut from a photo on this radio, shown in
// place of a direct chat's glyph or chat icon in Chats, Contacts, the chat's header,
// the lock screen, the glance card and OTP Messenger.
//
// Set from a direct chat's sheet ("Contact photo": Photos opens to pick one) or from
// Photos (hold a photo, "Use as a contact photo", pick the chat). Either way the crop
// screen follows: drag the photo inside the circle, - and + to zoom, Use.
//
// Kept beside the photos: /cph_<key>.cp1 on internal storage, else /meshcomod on the
// SD card when internal storage is full or not mounted. 64 x 64 with the circle's
// edge in its alpha, about 12 KB each. Included by UITask.cpp before PhotosUI.inc.

#include "ContactPhoto.h"

namespace cphoto {

static constexpr int kMaxKnown = 96;    // photos found on storage (listed once, then kept up to date)
static constexpr int kCacheN = 12;      // decoded ones kept in memory for the lists
static uint32_t s_known[kMaxKnown];
static int s_known_n = 0;
static bool s_listed = false;
static uint8_t s_gen = 1;               // bumped by every set or remove: the header redraws

struct Cached { uint32_t key; lv_img_dsc_t* dsc; };
static Cached s_cache[kCacheN];
static uint8_t s_cache_next = 0;

static void* bigAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}

// The two places a photo can be: 0 internal storage (its root), 1 the SD card's /meshcomod.
static fs::FS* storeAt(int where, char* root, size_t cap) {
  if (where == 0) { snprintf(root, cap, "%s", ""); return backupInternalFs(); }
  snprintf(root, cap, "%s", "/meshcomod");
  return backupSdFs();
}
static void pathFor(uint32_t key, const char* root, char* out, size_t cap) {
  char nm[24];
  ContactPhoto::fileName(key, nm, sizeof nm);
  snprintf(out, cap, "%s/%s", root, nm);
}

static bool knownHas(uint32_t key) {
  for (int i = 0; i < s_known_n; ++i) if (s_known[i] == key) return true;
  return false;
}
static void knownAdd(uint32_t key) {
  if (!key || knownHas(key) || s_known_n >= kMaxKnown) return;
  s_known[s_known_n++] = key;
}
static void knownDrop(uint32_t key) {
  for (int i = 0; i < s_known_n; ++i)
    if (s_known[i] == key) { s_known[i] = s_known[--s_known_n]; return; }
}

// Which photos exist: the folders are read once, so drawing a list never touches storage
// for a contact without one.
static void listOnce() {
  if (s_listed) return;
  s_listed = true;
  for (int where = 0; where < 2; ++where) {
    char root[16];
    fs::FS* fs = storeAt(where, root, sizeof root);
    if (!fs) continue;
    File dir = fs->open(root[0] ? root : "/");
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); continue; }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (!f.isDirectory() && f.size() == ContactPhoto::kFileBytes) knownAdd(ContactPhoto::keyOfFile(f.name()));
      f.close();
    }
    dir.close();
  }
}

static void cacheDrop(uint32_t key) {
  for (auto& c : s_cache) {
    if (c.dsc && c.key == key) {
      // Avatars draw their own copies (avatarSetContactPhoto), so nothing shows this one.
      lv_img_cache_invalidate_src(c.dsc);
      heap_caps_free((void*)c.dsc->data);
      heap_caps_free(c.dsc);
      c.dsc = nullptr;
      c.key = 0;
    }
  }
}

static bool has(const char* name) {
  if (!name || !*name) return false;
  listOnce();
  return knownHas(ContactPhoto::keyOf(name));
}

// The photo for this name, decoded and kept; nullptr without one.
static const lv_img_dsc_t* get(const char* name) {
  if (!has(name)) return nullptr;
  const uint32_t key = ContactPhoto::keyOf(name);
  for (const auto& c : s_cache) if (c.dsc && c.key == key) return c.dsc;
  uint8_t* px = (uint8_t*)bigAlloc(ContactPhoto::kPixels);
  if (!px) return nullptr;
  bool ok = false;
  for (int where = 0; where < 2 && !ok; ++where) {
    char root[16], path[48];
    fs::FS* fs = storeAt(where, root, sizeof root);
    if (!fs) continue;
    pathFor(key, root, path, sizeof path);
    if (!fs->exists(path)) continue;
    File f = fs->open(path, "r");
    if (!f) continue;
    uint8_t h[ContactPhoto::kHeader];
    ok = f.read(h, sizeof h) == sizeof h && ContactPhoto::headerOk(h, f.size()) &&
         f.read(px, ContactPhoto::kPixels) == ContactPhoto::kPixels;
    f.close();
  }
  if (!ok) { heap_caps_free(px); knownDrop(key); return nullptr; }
  lv_img_dsc_t* d = (lv_img_dsc_t*)bigAlloc(sizeof(lv_img_dsc_t));
  if (!d) { heap_caps_free(px); return nullptr; }
  memset(d, 0, sizeof *d);
  d->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
  d->header.w = ContactPhoto::kSide;
  d->header.h = ContactPhoto::kSide;
  d->data_size = ContactPhoto::kPixels;
  d->data = px;
  Cached& slot = s_cache[s_cache_next];
  s_cache_next = (uint8_t)((s_cache_next + 1) % kCacheN);
  if (slot.dsc) cacheDrop(slot.key);
  slot.key = key;
  slot.dsc = d;
  return d;
}

static void changed() {
  ++s_gen;
  if (!s_gen) s_gen = 1;
  g_lv.dirty_threads = true;   // Chats redraws its rows
  contactsListMarkStale();     // and so does Contacts
}

static bool save(const char* name, const uint8_t* px) {
  if (!name || !*name || !px) return false;
  const uint32_t key = ContactPhoto::keyOf(name);
  uint8_t h[ContactPhoto::kHeader];
  ContactPhoto::header(h);
  bool ok = false;
  for (int where = 0; where < 2 && !ok; ++where) {
    char root[16], path[48];
    fs::FS* fs = storeAt(where, root, sizeof root);
    if (!fs) continue;
    if (where == 1 && !fs->exists(root)) fs->mkdir(root);
    pathFor(key, root, path, sizeof path);
    File f = fs->open(path, "w");
    if (!f) continue;
    ok = f.write(h, sizeof h) == sizeof h && f.write(px, ContactPhoto::kPixels) == ContactPhoto::kPixels;
    f.close();
    if (!ok) fs->remove(path);   // didn't fit: nothing half written left behind
    else if (where == 0) {
      // A copy from before on the card would be read after this one: drop it.
      char r1[16], p1[48];
      if (fs::FS* sd = storeAt(1, r1, sizeof r1)) { pathFor(key, r1, p1, sizeof p1); if (sd->exists(p1)) sd->remove(p1); }
    }
  }
  if (!ok) return false;
  listOnce();
  knownAdd(key);
  cacheDrop(key);
  changed();
  return true;
}

static void removeFor(const char* name) {
  if (!name || !*name) return;
  const uint32_t key = ContactPhoto::keyOf(name);
  for (int where = 0; where < 2; ++where) {
    char root[16], path[48];
    fs::FS* fs = storeAt(where, root, sizeof root);
    if (!fs) continue;
    pathFor(key, root, path, sizeof path);
    if (fs->exists(path)) fs->remove(path);
  }
  knownDrop(key);
  cacheDrop(key);
  changed();
}

// ---- the crop screen ----------------------------------------------------------------
static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_view = nullptr;
static lv_obj_t* s_img = nullptr;
static lv_img_dsc_t s_src_dsc;           // the photo's thumbnail, as LVGL draws it
static uint16_t* s_src = nullptr;        // its pixels (RGB565)
static int s_sw = 0, s_sh = 0;
static float s_cx = 0, s_cy = 0;         // the circle's centre, in source pixels
static int s_zoom = 0;                   // ContactPhoto::kZooms index
static lv_coord_t s_d = 0;               // the circle's diameter on screen
static char s_name[UITask::MAX_THREAD_NAME + 1];
static void (*s_after_use)() = nullptr;  // Photos' pick mode closes itself after a Use

static void cropClose();

static float radius() { return ContactPhoto::radiusAt(s_sw, s_sh, s_zoom); }
static float scale() { return (float)s_d / (2.0f * radius()); }   // screen px per source px

static void place() {
  if (!s_img || !s_view) return;
  const float v = scale();
  lv_img_set_zoom(s_img, (uint16_t)LV_MIN(65535.0f, 256.0f * v + 0.5f));
  const lv_coord_t vw = lv_obj_get_width(s_view), vh = lv_obj_get_height(s_view);
  lv_obj_set_pos(s_img, (lv_coord_t)(vw / 2 - s_cx * v), (lv_coord_t)(vh / 2 - s_cy * v));
}

static void dragCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_PRESSING) return;
  lv_indev_t* in = lv_indev_get_act();
  if (!in) return;
  lv_point_t v;
  lv_indev_get_vect(in, &v);
  if (!v.x && !v.y) return;
  const float k = scale();
  s_cx -= (float)v.x / k;
  s_cy -= (float)v.y / k;
  ContactPhoto::clampCentre(s_sw, s_sh, radius(), s_cx, s_cy);
  place();
}

static void zoomBy(int d) {
  const int z = LV_CLAMP(0, s_zoom + d, ContactPhoto::kZoomCount - 1);
  if (z == s_zoom) return;
  s_zoom = z;
  ContactPhoto::clampCentre(s_sw, s_sh, radius(), s_cx, s_cy);
  place();
}

static void useCb(lv_event_t*) {
  uint8_t* out = (uint8_t*)bigAlloc(ContactPhoto::kPixels);
  bool ok = out && ContactPhoto::crop(s_src, s_sw, s_sh, s_cx, s_cy, radius(), out) && save(s_name, out);
  if (out) heap_caps_free(out);
  if (g_lv.task) g_lv.task->showAlert(ok ? TR("Contact photo set") : TR("Couldn't keep the photo"), ok ? 1400 : 2200);
  if (!ok) return;
  void (*after)() = s_after_use;
  cropClose();
  if (after) after();
}

static lv_obj_t* roundBtn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, bool primary) {
  lv_obj_t* b = lv_btn_create(parent);
  styleButton(b);
  if (primary) stylePrimary(b);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_height(b, SC(36));
  lv_obj_set_flex_grow(b, primary ? 2 : 1);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  useChainedFont(l);
  lv_obj_center(l);
  return b;
}

static void cropClose() {
  settingsSubPageClose(cropClose);
  if (s_root) {
    lv_obj_t* r = s_root;
    s_root = nullptr;
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);   // never drawn again: its pixels are freed below
    appPageDeleteRootAsync(r);
  }
  s_view = s_img = nullptr;
  if (s_src) { heap_caps_free(s_src); s_src = nullptr; }
  lv_img_cache_invalidate_src(&s_src_dsc);
  s_after_use = nullptr;
}

// Opens the crop screen on a stored photo, for the direct chat keyed `name`.
static void cropOpen(uint32_t sid, const char* name, void (*after_use)()) {
  if (!sid || !name || !*name) return;
  if (s_root) cropClose();
  int w = 0, h = 0;
  uint16_t* px = nullptr;
  if (!VoxPhotos::loadThumb(sid, &w, &h, &px) || w <= 0 || h <= 0) {
    VoxPhotos::requestThumb(sid);   // the background decoder makes one; try again shortly
    if (g_lv.task) g_lv.task->showAlert(TR("This photo isn't ready yet. Try again in a moment."), 2200);
    return;
  }
  snprintf(s_name, sizeof s_name, "%s", name);
  s_after_use = after_use;
  s_src = px;
  s_sw = w;
  s_sh = h;
  s_zoom = 0;
  s_cx = w * 0.5f;
  s_cy = h * 0.5f;
  memset(&s_src_dsc, 0, sizeof s_src_dsc);
  s_src_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  s_src_dsc.header.w = (uint32_t)w;
  s_src_dsc.header.h = (uint32_t)h;
  s_src_dsc.data_size = (uint32_t)w * (uint32_t)h * 2u;
  s_src_dsc.data = (const uint8_t*)px;
  lv_img_cache_invalidate_src(&s_src_dsc);   // the same descriptor held another photo before

  s_root = settingsSubPageOpen(TR("Contact photo"), cropClose);
  lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(s_root, SC(8), LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_root, SC(8), LV_PART_MAIN);

  lv_obj_t* hint = lv_label_create(s_root);
  char line[96];
  snprintf(line, sizeof line, TR("Drag the photo to place %s in the circle."), senderShownName(s_name));
  lv_label_set_text(hint, line);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(hint, LV_PCT(100));
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_font(hint, &g_font_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_SUB), LV_PART_MAIN);

  // The viewport: the photo under a dimmed frame with a round hole, dragged anywhere.
  s_view = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_view);
  lv_obj_set_width(s_view, LV_PCT(100));
  lv_obj_set_flex_grow(s_view, 1);
  lv_obj_clear_flag(s_view, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_view, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(s_view, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_view, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_view, SC(12), LV_PART_MAIN);
  lv_obj_set_style_clip_corner(s_view, true, LV_PART_MAIN);
  lv_obj_add_event_cb(s_view, dragCb, LV_EVENT_PRESSING, nullptr);

  lv_obj_t* row = lv_obj_create(s_root);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, SC(8), LV_PART_MAIN);
  roundBtn(row, LV_SYMBOL_MINUS, [](lv_event_t*) { zoomBy(-1); }, false);
  roundBtn(row, LV_SYMBOL_PLUS, [](lv_event_t*) { zoomBy(1); }, false);
  roundBtn(row, TR("Use"), useCb, true);

  lv_obj_update_layout(s_root);
  const lv_coord_t vw = lv_obj_get_width(s_view), vh = lv_obj_get_height(s_view);
  s_d = (lv_coord_t)LV_MAX(48, LV_MIN(vw, vh) - SC(16));

  s_img = lv_img_create(s_view);
  lv_img_set_src(s_img, &s_src_dsc);
  lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
  lv_img_set_pivot(s_img, 0, 0);
  lv_img_set_antialias(s_img, false);   // quick while dragging; Use samples it smoothly

  // The frame: a ring so wide its outside covers the viewport's corners, leaving the circle.
  const lv_coord_t reach = (lv_coord_t)(vw + vh);
  lv_obj_t* dim = lv_obj_create(s_view);
  lv_obj_remove_style_all(dim);
  lv_obj_clear_flag(dim, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(dim, s_d + 2 * reach, s_d + 2 * reach);
  lv_obj_set_style_radius(dim, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(dim, reach, LV_PART_MAIN);
  lv_obj_set_style_border_color(dim, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_opa(dim, LV_OPA_60, LV_PART_MAIN);
  lv_obj_center(dim);
  lv_obj_t* ring = lv_obj_create(s_view);
  lv_obj_remove_style_all(ring);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(ring, s_d + 4, s_d + 4);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(ring, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(ring, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_border_opa(ring, LV_OPA_80, LV_PART_MAIN);
  lv_obj_center(ring);

  place();
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

}  // namespace cphoto

// A direct chat's photo on its avatar (makeGlyphAvatar's), keyed on the chat's name
// (the name its chat icon is keyed on). False, and the avatar untouched, without one.
static void contactPhotoFreeCb(lv_event_t* e) {
  lv_img_dsc_t* d = (lv_img_dsc_t*)lv_event_get_user_data(e);
  if (!d) return;
  lv_img_cache_invalidate_src(d);   // a later copy can land at this address
  heap_caps_free((void*)d->data);
  heap_caps_free(d);
}
static bool avatarSetContactPhoto(lv_obj_t* av, const char* name) {
  const lv_img_dsc_t* c = av ? cphoto::get(name) : nullptr;
  if (!c) return false;
  // Its own copy, freed with the image: the cache can drop or replace its one at any
  // time (a new photo, a 13th contact) while this avatar is still on screen.
  lv_img_dsc_t* d = (lv_img_dsc_t*)cphoto::bigAlloc(sizeof(lv_img_dsc_t));
  uint8_t* px = (uint8_t*)cphoto::bigAlloc(ContactPhoto::kPixels);
  if (!d || !px) { if (d) heap_caps_free(d); if (px) heap_caps_free(px); return false; }
  *d = *c;
  memcpy(px, c->data, ContactPhoto::kPixels);
  d->data = px;
  lv_obj_clean(av);
  lv_obj_set_user_data(av, nullptr);   // no glyph under it
  lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(av, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(av, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_t* im = lv_img_create(av);
  lv_obj_clear_flag(im, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_img_set_src(im, d);
  lv_obj_add_event_cb(im, contactPhotoFreeCb, LV_EVENT_DELETE, d);
  const lv_coord_t sz = lv_obj_get_style_width(av, LV_PART_MAIN);
  lv_img_set_zoom(im, (uint16_t)LV_MAX(1, 256 * sz / ContactPhoto::kSide));
  lv_img_set_antialias(im, sz != ContactPhoto::kSide);
  lv_obj_center(im);
  lv_obj_invalidate(av);
  return true;
}
static uint8_t contactPhotoGen() { return cphoto::s_gen; }
