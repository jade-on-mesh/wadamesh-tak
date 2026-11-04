// SPDX-License-Identifier: GPL-3.0-or-later
// Reactions (Reactions.h): the picker a message's React button opens, and sending
// one. Included into UITask.cpp after TeamUI.inc (it uses the team action sheet).
//
// A reaction goes to the message's own chat as the app's "r:HASH:II", so MeshCore
// Open (and Open TAK) show it on the message, and this radio does the same with
// the app's. The emoji are the app's own list, in its order; the six it offers
// first come first here too. Emoji this radio can't draw are left out of the
// picker (one received still counts, and shows as well as the font allows).

namespace reactui {

static int s_slot = -1;   // the ring slot of the message being reacted to

static bool drawable(int i) {
#if LV_USE_IMGFONT   // the colour-emoji font (EmojiFit.h) sits in the text fonts' chain
  const char* e = reactions::emojiAt(i);
  uint32_t cp = 0;
  return e && reactions::detail::nextCp(e, &cp) && emojiGlyphLookup(cp) != nullptr;
#else
  return true;   // no colour emoji here: offer them all; the app shows them properly
#endif
}

static void sendCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const int ei = (int)(intptr_t)lv_event_get_user_data(e);
  lv_indev_t* a = lv_indev_get_act();
  if (a) lv_indev_wait_release(a);
  teamui::sheetClose();
  uint16_t h = 0;
  if (!g_lv.task || s_slot < 0 || !g_lv.task->reactionHashOfSlot(s_slot, &h)) {
    if (g_lv.task) g_lv.task->showAlert(TR("Couldn't react to that message"), 1800);
    return;
  }
  char t[12];
  reactions::format(h, ei, t, sizeof t);
  // To the open chat (the message's own), as any message from the composer is
  // sent, without touching the draft in it.
  if (!g_lv.task->composerSend(t)) return;   // composerSend says why
  g_lv.dirty_timeline = true;
}

static void open(int slot) {
  if (!g_lv.task || slot < 0) return;
  s_slot = slot;
  lv_obj_t* card = teamui::sheetOpen(TR("React"), "");
  for (int i = 0; i < reactions::kEmojiCount; ++i) {
    if (reactions::emojiIndex(reactions::kEmojis[i]) != i) continue;   // a repeat: offered once, at its first place
    if (!drawable(i)) continue;
    lv_obj_t* b = teamui::sheetBtn(card, reactions::kEmojis[i], sendCb, (void*)(intptr_t)i);
    lv_obj_set_style_pad_hor(b, 6, LV_PART_MAIN);
    lv_obj_set_style_min_width(b, 40, LV_PART_MAIN);
  }
  teamui::sheetBtn(card, TR("Close"), [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) teamui::sheetClose();
  });
}

}  // namespace reactui

static void reactPickerOpen(int slot) { reactui::open(slot); }
