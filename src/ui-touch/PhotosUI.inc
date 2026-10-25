// SPDX-License-Identifier: GPL-3.0-or-later
// Photos (app drawer -> APPACT_PHOTOS): the photos downloaded from the mesh
// (MeshCore Open TAK photos, VoxPhotos' /vox_<sid>.vi1), newest first, as a grid
// of their thumbnails. Tap one to view it; hold one (the triangle key on the
// M9) to send it to a chat or delete it. A photo without a thumbnail yet shows a placeholder and
// asks for one: the background decoder (voxThumbTick) makes it, and the grid
// fills in. Included by UITask.cpp after the chat's photo helpers.

namespace photosui {

static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_body = nullptr;
static lv_timer_t* s_timer = nullptr;
static int s_limit = 24;              // cells shown; "Show more" adds a page
static constexpr int kPage = 24;
static constexpr int kMaxListed = 240;
static uint32_t s_seen_thumb_gen = 0;
static uint32_t s_seen_state = 0;
static uint32_t s_del_sid = 0;        // the photo a hold asked to delete

static void pageBuild();
static void pageClose();
static void pageBack() { pageClose(); }

// ---- the look: the fork apps' shared glass (ForkLook.inc) ---------------------
using forklook::accentSoft;
using forklook::iconBubble;
using forklook::kGap;
using forklook::kThumbR;
using forklook::plainLook;
static void styleGlassBox(lv_obj_t* o, lv_coord_t radius) { forklook::glassBox(o, radius); }
static void styleTile(lv_obj_t* c) {
  forklook::tile(c);
  lv_obj_set_style_pad_all(c, 4, LV_PART_MAIN);
}

static void thumbDeleteCb(lv_event_t* e) {
  lv_img_dsc_t* dsc = (lv_img_dsc_t*)lv_event_get_user_data(e);
  if (!dsc) return;
  free((void*)dsc->data);
  free(dsc);
}

// The thumbnail centred in `cell`; false (and a placeholder) when there is none yet.
static bool addThumb(lv_obj_t* cell, uint32_t sid) {
  int w = 0, h = 0;
  uint16_t* px = nullptr;
  if (VoxPhotos::loadThumb(sid, &w, &h, &px)) {
    lv_img_dsc_t* dsc = (lv_img_dsc_t*)calloc(1, sizeof(lv_img_dsc_t));
    if (dsc) {
      dsc->header.always_zero = 0;
      dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
      dsc->header.w = (uint32_t)w;
      dsc->header.h = (uint32_t)h;
      dsc->data_size = (uint32_t)w * (uint32_t)h * 2u;
      dsc->data = (const uint8_t*)px;
      lv_obj_t* img = lv_img_create(cell);
      lv_img_set_src(img, dsc);
      lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_radius(img, kThumbR, LV_PART_MAIN);
      lv_obj_set_style_clip_corner(img, true, LV_PART_MAIN);
      lv_obj_add_event_cb(img, thumbDeleteCb, LV_EVENT_DELETE, dsc);
      lv_obj_center(img);
      return true;
    }
    free(px);
  }
  // Not decoded yet: a soft accent glow with the photo icon, filled in shortly.
  lv_obj_t* ph = lv_obj_create(cell);
  lv_obj_remove_style_all(ph);
  lv_obj_set_size(ph, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(ph, kThumbR, LV_PART_MAIN);
  if (!plainLook()) {
    lv_obj_set_style_bg_opa(ph, 150, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ph, lv_color_hex(accentSoft(150)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(ph, lv_color_hex(accentSoft(215)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(ph, LV_GRAD_DIR_VER, LV_PART_MAIN);
  }
  lv_obj_t* l = lv_label_create(ph);
  lv_label_set_text(l, UI_ICON_IMAGE);
  lv_obj_set_style_text_font(l, &ui_icons_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(plainLook() ? COLOR_SUB : COLOR_GLOW), LV_PART_MAIN);
  lv_obj_center(l);
  VoxPhotos::requestThumb(sid);   // the background decoder makes one; the grid fills in
  return false;
}

static void openCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const uint32_t sid = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  if (sid) voxShowStoredPhoto(sid, "photo.vi1");
}

static void deleteApply() {
  const uint32_t sid = s_del_sid;
  s_del_sid = 0;
  if (!sid) return;
  if (!VoxPhotos::deletePhoto(sid) && g_lv.task) g_lv.task->showAlert(TR("Couldn't delete the photo"), 1800);
  g_lv.dirty_timeline = true;   // an open chat shows it as "tap to download" again
  pageBuild();
}

// ---- sending: a stored photo to a channel or a contact ---------------------------
// The photo's VI1 envelope goes out as an ordinary message; whoever wants the
// photo asks for it and this radio serves it (VoxServe), as the app does.
static uint32_t s_send_sid = 0;

static bool sendToThread(int tidx, const char* text) {
  if (!g_lv.task || tidx < 0 || !text || !*text) return false;
  bool ch = false; uint16_t unread = 0; uint32_t ts = 0; char name[48] = {0};
  if (!g_lv.task->getThreadInfo(tidx, ch, unread, ts, name, sizeof name)) return false;
  if (ch) {
    // By name at send time, never a cached slot: a stale slot is another channel's key (#589).
    for (int s = 0; s < MAX_GROUP_CHANNELS; ++s) {
      ChannelDetails cd;
      if (the_mesh.getChannel(s, cd) && cd.name[0] && strcmp(cd.name, name) == 0) {
        termDoSend(true, nullptr, (int16_t)s, name, text);
        return true;
      }
    }
    return false;
  }
  uint8_t pub[32];
  if (!g_lv.task->getThreadContactPub(tidx, pub)) return false;
  termDoSend(false, pub, -1, name, text);
  return true;
}

static void sendPickedCb(lv_event_t* e) {
  const int tidx = (int)(intptr_t)lv_event_get_user_data(e);
  teamui::sheetClose();
  char env[48];
  bool ch = false; uint16_t unread = 0; uint32_t ts = 0; char name[48] = {0};
  const bool known = g_lv.task && g_lv.task->getThreadInfo(tidx, ch, unread, ts, name, sizeof name);
  if (!known || !VoxPhotos::photoEnvelope(s_send_sid, env, sizeof env) || !sendToThread(tidx, env)) {
    if (g_lv.task) g_lv.task->showAlert(TR("Couldn't send the photo"), 2000);
    return;
  }
  char msg[80];
  snprintf(msg, sizeof msg, TR("Photo sent to %s"), name);
  if (g_lv.task) g_lv.task->showAlert(msg, 1800);
  g_lv.dirty_threads = true;
  g_lv.dirty_timeline = true;
}

// The chats to send a photo to: channels first, then direct chats.
static void sendPick(uint32_t sid) {
  if (!sid || !g_lv.task) return;
  s_send_sid = sid;
  lv_obj_t* card = teamui::sheetOpen(TR("Send the photo to"), "");
  int shown = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < UITask::MAX_UI_THREADS; i++) {
      bool ch = false; uint16_t unread = 0; uint32_t ts = 0; char name[48] = {0};
      if (!g_lv.task->getThreadInfo(i, ch, unread, ts, name, sizeof name) || !name[0]) continue;
      if (ch != (pass == 0)) continue;
      teamui::sheetBtn(card, name, sendPickedCb, (void*)(intptr_t)i);
      ++shown;
    }
  }
  if (!shown) {
    lv_obj_t* l = lv_label_create(card);
    lv_label_set_text(l, TR("No chats yet."));
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
  }
  teamui::sheetBtn(card, TR("Close"), [](lv_event_t*) { teamui::sheetClose(); });
}

static void holdCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
  s_del_sid = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  if (!s_del_sid) return;
  lv_indev_t* a = lv_indev_get_act();
  if (a) lv_indev_wait_release(a);   // the hold's release mustn't also open it
  lv_obj_t* card = teamui::sheetOpen(TR("Photo"), "");
  teamui::sheetBtn(card, TR("Send to a chat"), [](lv_event_t*) { teamui::sheetClose(); sendPick(s_del_sid); });
  teamui::sheetBtn(card, TR("Delete"), [](lv_event_t*) {
    teamui::sheetClose();
    showConfirm(TR("Delete this photo?\nIt can be downloaded again\nwhile someone still has it."), TR("Delete"),
                deleteApply);
  }, nullptr, 0x5A2D2D);
  teamui::sheetBtn(card, TR("Close"), [](lv_event_t*) { teamui::sheetClose(); });
}

static void pageBuild() {
  if (!s_root) return;
  if (s_body) { lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN); lv_obj_del_async(s_body); }
  s_seen_thumb_gen = chatThumbGen();
  s_seen_state = VoxPhotos::stateVersion();
  s_body = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_body);
  lv_obj_set_size(s_body, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_all(s_body, kGap, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_body, kGap, LV_PART_MAIN);
  lv_obj_set_style_pad_column(s_body, kGap, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_ACTIVE);

  VoxPhotos::StoredPhoto* list =
      (VoxPhotos::StoredPhoto*)heap_caps_malloc(sizeof(VoxPhotos::StoredPhoto) * kMaxListed,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const int n = list ? VoxPhotos::listPhotos(list, kMaxListed) : 0;

  // Tiles about a thumbnail (kThumbMax) plus the frame wide, as many as fit across.
  const lv_coord_t avail = lv_obj_get_content_width(s_root) > 0 ? lv_obj_get_content_width(s_root) - 2 * kGap
                                                                : lv_disp_get_hor_res(nullptr) - 2 * kGap;
  const lv_coord_t want = VoxPhotos::kThumbMax + 8;
  int cols = (int)((avail + kGap) / (want + kGap));
  if (cols < 2) cols = 2;
  const lv_coord_t cell = (lv_coord_t)((avail - (cols - 1) * kGap) / cols);

  char line[96];
  if (!n) {
    // Empty: a friendly card in the middle of the page.
    lv_obj_t* card = lv_obj_create(s_body);
    lv_obj_remove_style_all(card);
    styleGlassBox(card, 20);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_ver(card, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    iconBubble(card, 52, UI_ICON_IMAGE);
    lv_obj_t* t = lv_label_create(card);
    lv_label_set_text(t, TR("No photos yet"));
    lv_obj_set_style_text_font(t, &g_font_semi_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_t* d = lv_label_create(card);
    lv_label_set_text(d, TR("Photos you download from the mesh show up here."));
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(d, LV_PCT(100));
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(d, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(d, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
    if (list) heap_caps_free(list);
    return;
  }

  // Header: an accent bubble, the count, and a pill with "newest first".
  {
    lv_obj_t* head = lv_obj_create(s_body);
    lv_obj_remove_style_all(head);
    styleGlassBox(head, 18);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(head, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(head, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    iconBubble(head, 36, UI_ICON_IMAGE);
    lv_obj_t* col = lv_obj_create(head);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t* t = lv_label_create(col);
    lv_label_set_text(t, TR("From the mesh"));
    lv_obj_set_style_text_font(t, &g_font_semi_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_t* c = lv_label_create(col);
    snprintf(line, sizeof line, n == 1 ? TR("1 photo") : TR("%d photos"), n);
    lv_label_set_text(c, line);
    lv_obj_set_style_text_font(c, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(c, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
    lv_obj_t* pill = lv_label_create(head);
    lv_label_set_text(pill, TR("Newest first"));
    lv_obj_set_style_text_font(pill, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(pill, 9, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(pill, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
    if (plainLook()) {
      lv_obj_set_style_bg_color(pill, lv_color_hex(COLOR_CONTROL), LV_PART_MAIN);
      lv_obj_set_style_text_color(pill, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(pill, lv_color_hex(accentSoft(s_theme_day ? 200 : 170)), LV_PART_MAIN);
      lv_obj_set_style_text_color(pill, lv_color_hex(s_theme_day ? COLOR_ACCENT : COLOR_GLOW), LV_PART_MAIN);
    }
  }

  const int shown = n < s_limit ? n : s_limit;
  for (int i = 0; i < shown; i++) {
    lv_obj_t* c = lv_btn_create(s_body);
    styleButton(c);
    styleTile(c);
    lv_obj_set_size(c, cell, cell);
    lv_obj_add_event_cb(c, openCb, LV_EVENT_CLICKED, (void*)(uintptr_t)list[i].sid);
    lv_obj_add_event_cb(c, holdCb, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)list[i].sid);
    addThumb(c, list[i].sid);
  }
  if (n > shown) {
    lv_obj_t* more = lv_btn_create(s_body);
    styleButton(more);
    styleTile(more);
    lv_obj_set_style_radius(more, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_size(more, LV_PCT(100), 36);
    lv_obj_add_event_cb(more, [](lv_event_t*) { s_limit += kPage; pageBuild(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(more);
    snprintf(line, sizeof line, TR("Show more (%d)"), n - shown);
    lv_label_set_text(l, line);
    useChainedFont(l);
    lv_obj_set_style_text_color(l, lv_color_hex(plainLook() ? COLOR_TEXT : COLOR_GLOW), LV_PART_MAIN);
    lv_obj_center(l);
  }
  if (n) {
    lv_obj_t* tip = lv_label_create(s_body);
    lv_label_set_text(tip, TR("Tap a photo to view it. Hold it to send it to a chat or delete it."));
    lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tip, LV_PCT(100));
    lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_top(tip, 2, LV_PART_MAIN);
    lv_obj_set_style_text_font(tip, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(tip, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
  }
  if (list) heap_caps_free(list);
}

static void pageClose() {
  if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
  s_body = nullptr;
  if (s_root) { lv_obj_t* r = s_root; s_root = nullptr; appPageDeleteRootAsync(r); }
  appPageEnd(pageBack);
}

static void pageOpen() {
  if (s_root) return;
  s_root = appPageCreateRoot(COLOR_BG);
  appPageBeginSlim("Photos", pageBack);
  s_limit = kPage;
  pageBuild();
  // Thumbnails land one at a time from the background decoder, and a download
  // can finish while the page is open: rebuild when either has happened.
  s_timer = lv_timer_create([](lv_timer_t*) {
    if (chatThumbGen() != s_seen_thumb_gen || VoxPhotos::stateVersion() != s_seen_state) pageBuild();
  }, 1500, nullptr);
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

}  // namespace photosui
