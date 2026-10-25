// SPDX-License-Identifier: GPL-3.0-or-later
// Predictive text (src/ui-touch/PredictText.h).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/PredictText.h"

using namespace predict;

static bool has(const Suggestions& s, const char* w) {
  for (int i = 0; i < s.n; i++) if (!strcmp(s.w[i], w)) return true;
  return false;
}
// Suggestions for the word ending the text (a trailing space: the next word).
static Suggestions at(const Dict& d, const char* text) {
  size_t wl;
  const size_t a = wordAt(text, strlen(text), &wl);
  Suggestions s;
  d.suggest(text, a, wl, s);
  return s;
}

int main() {
  static Dict d;
  char out[kSuggest][kWordMax];
  // The dictionary: sorted, front-coded, findable.
  assert(dict::count() > 9000 && dict::count() == words::kFileWords);
  assert(dict::find("the", 3) > 70);
  assert(dict::find("repeater", 8) >= 45);   // a mesh word, with its floor
  assert(dict::find("zzqx", 4) == 0);
  char disp[kWordMax];
  assert(dict::find("i", 1, disp) && !strcmp(disp, "I"));
  assert(dict::find("dont", 4) == 0 && dict::find("don't", 5) > 0);
  int seen = 0;
  dict::scanFrom(0, [&](const char*, size_t, uint8_t) { seen++; return true; });
  assert(seen == dict::count());

  // Completions, most used first, from one letter.
  int n = d.suggest("th", 2, out);
  assert(n == 3 && !strcmp(out[0], "the"));
  assert(d.suggest("t", 1, out) == 3);
  assert(d.suggest("zzqx", 4, out) == 0);
  n = d.suggest("Rep", 3, out);
  assert(n >= 1 && out[0][0] == 'R');   // a capital typed: capitalised words

  // Corrections, and what Space does.
  Suggestions s = at(d, "teh");
  assert(!strcmp(s.w[0], "the") && !strcmp(s.space, "the"));
  s = at(d, "I dont");
  assert(!strcmp(s.space, "don't"));
  s = at(d, "recieve");
  assert(!strcmp(s.space, "receive"));
  s = at(d, "i");
  assert(!strcmp(s.space, "I"));
  s = at(d, "meet in");
  assert(s.space[0] == 0);   // a real word is left alone
  s = at(d, "hello");
  assert(s.space[0] == 0);
  s = at(d, "thoguh");
  assert(!strcmp(s.space, "though"));   // not a word: Space fixes it
  s = at(d, "thx");
  assert(strcmp(s.space, "the") != 0);  // a short word: only a likely slip is fixed
  s = at(d, "im");
  assert(!strcmp(s.space, "I'm"));
  s = at(d, "repeat");
  assert(s.space[0] == 0 && has(s, "repeater"));
  // Two or three letters, rarely a word on their own: Space completes them.
  static const char* const kDone[][2] = {{"th", "the"}, {"ab", "about"}, {"co", "could"}, {"re", "really"},
                                         {"st", "still"}, {"ye", "yes"}, {"tod", "today"}, {"bec", "because"},
                                         {"eve", "even"}};
  for (const auto& k : kDone) {
    s = at(d, k[0]);
    if (strcmp(s.space, k[1])) { fprintf(stderr, "%s -> %s, want %s\n", k[0], s.space, k[1]); assert(false); }
  }
  // Common short words, chat words and real three-letter words stay.
  static const char* const kKept[] = {"go", "to", "be", "we", "so", "hi", "ok", "lol", "ha", "tea", "bus",
                                      "mad", "sum", "bat", "art", "cat", "dog", "yes"};
  for (const char* k : kKept) {
    s = at(d, k);
    if (s.space[0]) { fprintf(stderr, "%s -> %s, want it kept\n", k, s.space); assert(false); }
  }
  {   // A short word you send is yours: it stays.
    Dict mine;
    for (int i = 0; i < 3; i++) mine.learnText("th");
    s = at(mine, "th");
    assert(s.space[0] == 0);
  }
  assert(keysNear('a', 's') && keysNear('g', 'h') && !keysNear('a', 'p'));

  // The next word, before any of it is typed.
  s = at(d, "on my ");
  assert(s.n >= 1 && !strcmp(s.w[0], "way"));
  s = at(d, "thank ");
  assert(!strcmp(s.w[0], "you"));
  s = at(d, "ok. ");
  assert(s.n >= 1 && s.w[0][0] >= 'A' && s.w[0][0] <= 'Z');   // a new sentence: capitals
  s = at(d, "12345 ");
  assert(s.n == 0);   // nothing to go on

  // Your words and pairs come first, the more you send them.
  d.learnText("Meet at Thornton Creek trailhead, thornton creek again");
  d.learnText("Thornton is closed");
  n = d.suggest("tho", 3, out);
  assert(n >= 1 && !strcmp(out[0], "Thornton"));   // as first written, 3 times
  n = d.suggest("tr", 2, out);
  bool th = false;
  for (int i = 0; i < n; i++) th |= !strcmp(out[i], "trailhead");
  assert(th);
  d.learnText("checking in on my radio");
  d.learnText("left it on my radio shelf");
  s = at(d, "is it on my ");
  assert(!strcmp(s.w[0], "radio"));
  s = at(d, "the trai");
  assert(has(s, "trailhead"));
  // Short words, numbers and the word itself aren't suggested.
  d.learnText("ok 12345 abc123");
  assert(d.suggest("12", 2, out) == 0);
  n = d.suggest("the", 3, out);
  for (int i = 0; i < n; i++) assert(strcmp(out[i], "the") != 0);

  // The word at the caret, and the one before it.
  size_t len;
  const char* t = "heading to thorn";
  size_t st = wordAt(t, strlen(t), &len);
  assert(st == 11 && len == 5);
  st = wordAt("hi ", 3, &len);
  assert(len == 0);
  char pv[kWordMax];
  prevWord("heading to thorn", 11, pv);
  assert(!strcmp(pv, "to"));
  prevWord("Done. Next", 6, pv);
  assert(!strcmp(pv, "^"));
  prevWord("well, maybe", 6, pv);
  assert(!strcmp(pv, "well"));

  // Save and load keep the counts and the pairs; old files still load.
  static char buf[kSaveMax];
  d.save(buf, sizeof buf);
  static Dict e;
  e.load(buf, strlen(buf));
  assert(e.count == d.count && e.pairs == d.pairs);
  n = e.suggest("tho", 3, out);
  assert(!strcmp(out[0], "Thornton"));
  s = at(e, "on my ");
  assert(!strcmp(s.w[0], "radio"));
  static Dict old;
  const char* oldf = "Thornton 3\ntrailhead 1\n";
  old.load(oldf, strlen(oldf));
  assert(old.count == 2 && old.pairs == 0);

  // Full: the least used goes.
  for (int i = 0; i < kLearnMax + 20; i++) {
    char w[16];
    snprintf(w, sizeof w, "word%c%c", 'a' + i % 26, 'a' + i / 26);
    d.learnText(w);
  }
  assert(d.count == kLearnMax);
  n = d.suggest("tho", 3, out);
  assert(!strcmp(out[0], "Thornton"));   // used three times: kept
  printf("predict text: ok\n");
  return 0;
}
