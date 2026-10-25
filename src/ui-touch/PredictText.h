// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Predictive text (PREDICTIVE_TEXT.md), as a phone keyboard does it:
//  * completions of the word being typed, from a built-in dictionary of the
//    10,000 most used English words (PredictWords.inc, with their frequencies)
//    and the words you send;
//  * corrections of a mistyped word (one letter wrong, missing, extra or two
//    swapped; a missing apostrophe: "dont" -> "don't"), keys next to each other
//    on QWERTY counting as the likeliest slips;
//  * the next word, before you've typed any of it, from the word before: the
//    pairs you send most ("on my" -> "way") and a built-in table for chat;
//  * context: completions that follow the previous word in your messages rank
//    higher.
// What Space does (spaceWord): a mistyped word becomes its correction, an
// unknown one its best completion; a real word is left alone ("in" stays "in"),
// except two or three letters that are rarely a word on their own, which
// complete ("th" -> "the", "tod" -> "today"; see kShortKeepFreq).
//
// Pure logic, header-only, host-tested in test/test_predict_text.cpp.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The built-in dictionary is compiled in unless a board keeps it out of flash
// (PREDICT_BUILTIN_WORDS 0: the T-Display P4), which loads the same dictionary
// from a file instead (dict::loadFile): dictionaries/words-en.pwd.
#ifndef PREDICT_BUILTIN_WORDS
#define PREDICT_BUILTIN_WORDS 1
#endif
#if PREDICT_BUILTIN_WORDS
#include "PredictWords.inc"
#endif
#include "PredictWordsInfo.h"

namespace predict {

// Mesh and team words. Not used at run time: scripts/predict/gen_words.py
// reads this list into the built-in dictionary (PredictWords.inc) with a
// frequency floor, next to wordfreq's 10,000 everyday English words.
static const char* const kBase[] = {
  "the", "to", "and", "you", "that", "it", "for", "is", "on", "in", "have", "with", "this", "be", "are",
  "not", "just", "was", "but", "what", "they", "can", "get", "all", "know", "like", "out", "here", "there",
  "about", "going", "good", "now", "back", "see", "will", "right", "think", "when", "from", "how", "yes",
  "today", "where", "time", "people", "thanks", "thank", "sure", "okay", "please", "need", "want", "would",
  "should", "could", "really", "still", "something", "anyone", "everyone", "someone", "nothing", "anything",
  "around", "because", "before", "after", "again", "always", "never", "maybe", "already", "almost",
  "tonight", "tomorrow", "yesterday", "morning", "afternoon", "evening", "night", "weekend", "minutes",
  "hours", "later", "soon", "early", "late", "coming", "leaving", "heading", "arrived", "waiting", "meeting",
  "location", "position", "north", "south", "east", "west", "street", "road", "highway", "bridge", "park",
  "parking", "station", "downtown", "hospital", "school", "store", "building", "corner", "near", "nearby",
  "behind", "between", "across", "inside", "outside", "home", "house", "office", "friend", "friends",
  "family", "everything", "everybody", "message", "messages", "received", "receive", "reply", "replied",
  "copy", "roger", "affirmative", "negative", "standby", "over", "check", "testing", "test", "signal",
  "repeater", "repeaters", "channel", "channels", "mesh", "network", "radio", "antenna", "battery", "solar",
  "power", "range", "coverage", "contact", "contacts", "advert", "node", "nodes", "frequency", "MeshCore",
  "Wadamesh", "photo", "picture", "voice", "team", "teammate", "status", "update", "updated", "report",
  "reports", "watch", "seen", "spotted", "vehicle", "vehicles", "agents", "police", "checkpoint", "drone",
  "drones", "safe", "help", "emergency", "medical", "water", "food", "supplies", "weather", "rain", "wind",
  "snow", "fire", "smoke", "traffic", "accident", "closed", "open", "working", "broken", "fixed", "sorry",
  "welcome", "hello", "morning", "great", "awesome", "nice", "cool", "perfect", "probably", "definitely",
  "actually", "exactly", "different", "important", "interesting", "together", "without", "through",
  "during", "another", "other", "every", "first", "last", "next", "little", "long", "small", "big", "same",
  "which", "while", "until", "since", "though", "these", "those", "their", "them", "then", "than", "said",
  "make", "made", "take", "took", "come", "came", "give", "look", "looking", "find", "found", "tell",
  "told", "call", "called", "try", "trying", "keep", "help", "send", "sent", "sending", "bring", "meet",
};
constexpr int kBaseN = (int)(sizeof kBase / sizeof kBase[0]);
constexpr size_t kWordMax = 24;
constexpr int kLearnMax = 400;
constexpr int kPairMax = 400;
constexpr int kSuggest = 3;
constexpr size_t kSaveMax = 16384;   // the /words file

// What usually comes next, for chat on the mesh: "word|next next ...", the
// likeliest first. "^" is the start of a message or sentence.
static const char* const kNext[] = {
  "^|I I'm ok thanks yes hey the is anyone",
  "i|am will think can have don't was just need see know got",
  "i'm|on at going heading not here in good back",
  "i'll|be check send see try let",
  "on|my the it our channel",
  "my|way radio node phone place",
  "at|the home my work",
  "in|the a my about",
  "the|mesh repeater channel other way same road",
  "to|the be get see do go make",
  "you|are can have soon later there know",
  "are|you we there they",
  "how|are about is much far many",
  "what|is are time do about",
  "where|are is do",
  "when|you is are do will",
  "thank|you",
  "thanks|for again",
  "see|you it the",
  "good|morning night luck to job idea",
  "going|to on out home",
  "can|you I we be see hear",
  "do|you it not we",
  "will|be do have check send",
  "be|there back a the safe",
  "let|me us you",
  "let's|go meet do see",
  "come|back over in",
  "get|home there back it",
  "heading|to home out north south east west",
  "please|send check let stay be",
  "stay|safe there tuned",
  "copy|that you",
  "roger|that",
  "is|it there anyone everyone a the",
  "anyone|on here else know hear have near around",
  "any|news updates reports one",
  "have|a you to been the any",
  "need|to a help some",
  "how's|it the everyone",
  "it|is was works looks",
  "it's|a not working the on",
  "that|is was the works",
  "there|is are was",
  "we|are have can need will",
  "they|are were have will",
  "just|got saw sent now heard made",
  "signal|is from",
  "battery|is at life",
  "check|in the out your",
  "what's|up the your going",
  "this|is morning week weekend evening",
  "for|the you a me now",
  "from|the my here",
  "no|problem one worries signal",
  "yes|I please",
  "ok|I thanks will",
  "okay|I thanks will",
  "of|the a my",
  "with|the a me my you",
  "all|good the of set clear",
  "can't|hear see make",
  "don't|know have think see",
  "near|the",
  "out|of there here",
  "back|to home in",
  "same|here time",
  "talk|to soon later",
  "hi|there everyone all",
  "hey|there everyone all",
  "hello|there everyone all",
  "about|to the it",
  "looks|like good",
  "sounds|good like great",
};
constexpr int kNextN = (int)(sizeof kNext / sizeof kNext[0]);

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
inline char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }
// Letters (any non-ASCII byte counts: accented letters), digits and an apostrophe.
inline bool wordChar(char c) {
  const uint8_t b = (uint8_t)c;
  return b >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '\'';
}
inline bool startsNoCase(const char* w, const char* p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (!w[i] || lower(w[i]) != lower(p[i])) return false;
  return true;
}
inline bool eqNoCase(const char* a, const char* b) {
  for (; *a && *b; a++, b++)
    if (lower(*a) != lower(*b)) return false;
  return !*a && !*b;
}
inline bool eqNoCaseN(const char* a, const char* b, size_t bl) {
  size_t i = 0;
  for (; i < bl; i++)
    if (!a[i] || lower(a[i]) != lower(b[i])) return false;
  return !a[i];
}
// Case-insensitive order (the dictionary's): <0, 0, >0.
inline int cmpNoCase(const char* a, size_t al, const char* b, size_t bl) {
  const size_t n = al < bl ? al : bl;
  for (size_t i = 0; i < n; i++) {
    const uint8_t x = (uint8_t)lower(a[i]), y = (uint8_t)lower(b[i]);
    if (x != y) return x < y ? -1 : 1;
  }
  return al == bl ? 0 : (al < bl ? -1 : 1);
}
inline int ilog2(uint32_t v) { int r = 0; while (v > 1) { v >>= 1; r++; } return r; }

// The word being typed: the run of word characters ending at `caret`
// (a byte offset into text). Returns its start; *len its length.
inline size_t wordAt(const char* text, size_t caret, size_t* len) {
  size_t s = caret;
  while (s > 0 && wordChar(text[s - 1])) s--;
  *len = caret - s;
  return s;
}

// The word before the one starting at `at`, lowercased, into out ("^" at the
// start of the text or of a sentence; "" when there's none to go on).
inline void prevWord(const char* text, size_t at, char out[kWordMax]) {
  out[0] = 0;
  size_t i = at;
  while (i > 0 && text[i - 1] == ' ') i--;
  if (i == 0) { strcpy(out, "^"); return; }
  const char c = text[i - 1];
  if (c == '.' || c == '!' || c == '?' || c == '\n') { strcpy(out, "^"); return; }
  if (c == ',' || c == ';' || c == ':') { i--; while (i > 0 && text[i - 1] == ' ') i--; }
  size_t e = i;
  while (i > 0 && wordChar(text[i - 1])) i--;
  if (e == i || e - i >= kWordMax) return;
  for (size_t k = 0; k < e - i; k++) out[k] = lower(text[i + k]);
  out[e - i] = 0;
}

// ---- the built-in dictionary ---------------------------------------------------

// FNV-1a, 32 bit: the dictionary file's check (PredictWordsInfo.h).
inline uint32_t fnv1a(const uint8_t* p, size_t n, uint32_t h = 0x811C9DC5u) {
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
  return h;
}

namespace dict {
// Where the dictionary is: the compiled-in arrays, or a loaded file. Empty
// until a file is loaded on a board without the arrays (then only your own
// words and the next-word table are suggested).
struct Source {
  const uint8_t* data = nullptr;
  size_t len = 0;
  const uint8_t* off = nullptr;   // kBlocks little-endian u32 offsets into data
  int blocks = 0;
  int count = 0;
};
inline Source& src() {
#if PREDICT_BUILTIN_WORDS
  static Source s = [] {
    Source b;
    b.data = words::kData;
    b.len = sizeof words::kData;
    b.off = reinterpret_cast<const uint8_t*>(words::kBlockOff);   // little-endian, as the file's
    b.blocks = words::kBlocks;
    b.count = words::kCount;
    return b;
  }();
#else
  static Source s;
#endif
  return s;
}
inline uint32_t u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline bool loaded() { return src().blocks > 0; }
inline int count() { return src().count; }
// Uses a dictionary file ("PWD1", word count, block count, block offsets, the
// data) held in memory that outlives it. False (nothing changed) unless it is
// exactly the file this firmware expects.
inline bool loadFile(const uint8_t* img, size_t len) {
  if (!img || len != words::kFileBytes || fnv1a(img, len) != words::kFileFnv) return false;
  if (len < 12 || img[0] != 'P' || img[1] != 'W' || img[2] != 'D' || img[3] != '1') return false;
  const uint32_t n = u32le(img + 4), blocks = u32le(img + 8);
  if (!blocks || 12 + (size_t)blocks * 4 > len) return false;
  Source& s = src();
  s.off = img + 12;
  s.data = img + 12 + (size_t)blocks * 4;
  s.len = len - 12 - (size_t)blocks * 4;
  s.blocks = (int)blocks;
  s.count = (int)n;
  return true;
}
inline uint32_t blockOff(int b) { return u32le(src().off + (size_t)b * 4); }
inline const uint8_t* blockStart(int b) { return src().data + blockOff(b); }
inline const uint8_t* blockEnd(int b) {
  return b + 1 < src().blocks ? src().data + blockOff(b + 1) : src().data + src().len;
}
// The last block whose first word is <= key: where key (or its prefix range) starts.
inline int blockFor(const char* key, size_t kl) {
  int lo = 0, hi = src().blocks - 1;
  if (hi < 0) return 0;
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    const uint8_t* p = blockStart(mid);
    if (cmpNoCase((const char*)p + 2, p[1], key, kl) <= 0) lo = mid; else hi = mid - 1;
  }
  return lo;
}
// Each word from block b on, in order: f(word, len, freq) returns false to stop.
template <class F> inline void scanFrom(int b, F f) {
  char w[kWordMax + 1];
  for (; b < src().blocks; b++) {
    const uint8_t* p = blockStart(b);
    const uint8_t* end = blockEnd(b);
    while (p < end) {
      const uint8_t sh = p[0], sl = p[1];
      if ((size_t)sh + sl > kWordMax) return;
      memcpy(w + sh, p + 2, sl);
      w[sh + sl] = 0;
      const uint8_t fr = p[2 + sl];
      p += 3 + sl;
      if (!f(w, (size_t)sh + sl, fr)) return;
    }
  }
}
// Frequency (zipf x 10) of a word, 0 if it isn't in; disp gets its spelling ("I").
inline int find(const char* w, size_t len, char* disp = nullptr) {
  if (!len || len >= kWordMax) return 0;
  int out = 0;
  scanFrom(blockFor(w, len), [&](const char* d, size_t dl, uint8_t fr) {
    const int c = cmpNoCase(d, dl, w, len);
    if (c < 0) return true;
    if (c == 0) { out = fr; if (disp) memcpy(disp, d, dl + 1); }
    return false;
  });
  return out;
}
}  // namespace dict

// QWERTY neighbours: a slip onto one of these is the likeliest typo.
inline bool keysNear(char a, char b) {
  static const char* const kRows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
  a = lower(a); b = lower(b);
  int ra = -1, ca = 0, rb = -1, cb = 0;
  for (int r = 0; r < 3; r++)
    for (int c = 0; kRows[r][c]; c++) {
      if (kRows[r][c] == a) { ra = r; ca = c; }
      if (kRows[r][c] == b) { rb = r; cb = c; }
    }
  if (ra < 0 || rb < 0) return false;
  const int dr = ra - rb, dc = (ca + ra) - (cb + rb);   // rows are offset by about half a key
  return dr >= -1 && dr <= 1 && dc >= -1 && dc <= 1 && !(dr == 0 && dc == 0);
}

// Is `w` one slip from the typed `p` (one letter extra, missing, wrong, or two
// swapped)? *pen as suggest() scores the slip.
inline bool oneSlip(const char* p, size_t pl, const char* w, size_t wl, int* pen) {
  if (pl < 2) return false;
  if (wl == pl) {
    size_t i = 0;
    while (i < pl && lower(p[i]) == lower(w[i])) i++;
    if (i == pl) return false;   // the same word
    if (i + 1 < pl && lower(p[i]) == lower(w[i + 1]) && lower(p[i + 1]) == lower(w[i]) &&
        eqNoCaseN(w + i + 2, p + i + 2, pl - i - 2)) { *pen = 12; return true; }
    if (eqNoCaseN(w + i + 1, p + i + 1, pl - i - 1)) { *pen = keysNear(p[i], w[i]) ? 12 : 20; return true; }
    return false;
  }
  if (wl + 1 == pl || pl + 1 == wl) {
    const char* lo = wl < pl ? w : p;    // the shorter
    const char* hi = wl < pl ? p : w;
    const size_t n = wl < pl ? wl : pl;
    size_t i = 0;
    while (i < n && lower(lo[i]) == lower(hi[i])) i++;
    if (!eqNoCaseN(hi + i + 1, lo + i, n - i)) return false;
    if (wl > pl) *pen = hi[i] == '\'' ? -8 : 16;   // a letter missing from what was typed
    else *pen = pl <= 3 ? 25 : 16;                  // one too many
    return true;
  }
  return false;
}

struct Learned {
  char w[kWordMax];
  uint16_t n;      // times sent
  uint32_t last;   // a counter: higher is more recent
};
struct Pair {
  char a[kWordMax];   // the word before, lowercased ("^": a message or sentence start)
  char b[kWordMax];   // the word after, as written
  uint16_t n;
  uint32_t last;
};

// Space and short words: a 2-3 letter word below this frequency (zipf x 10) is
// completed when its best completion scores this much more. Two letters that
// rare are nearly always a fragment or an abbreviation ("th" 42 vs "the" 77,
// "co" 52 vs "could" 61, "st" 52 vs "still" 59): 0.7 zipf, five times likelier.
// Three are usually a real word ("tea" 47 vs "team" 57, "bus" 48 vs "business"
// 56, "mad" 47 vs "made" 59), so only a far likelier completion wins ("eve" 43
// vs "even" 60). A common short word ("go" 60, "to" 74) always stays.
constexpr int kShortKeepFreq = 58;
constexpr int kShortCompleteMargin2 = 7;
constexpr int kShortCompleteMargin3 = 15;

// Short words that are whole words in chat, whatever their frequency.
inline bool chatShort(const char* w, size_t len) {
  static const char* const k[] = {"hi", "ok", "oh", "ah", "ha", "um", "uh", "eh", "hm", "yo", "ya",
                                  "lol", "yep", "yup", "nah", "omg", "btw", "idk", "brb", "thx",
                                  "ty", "np", "hey", "aye", "wtf", "ffs", "imo"};
  for (const char* x : k)
    if (strlen(x) == len && eqNoCaseN(x, w, len)) return true;
  return false;
}

// What suggest() found for one word, for the strip and for Space.
struct Suggestions {
  char w[kSuggest][kWordMax];
  int n = 0;
  char space[kWordMax] = {0};   // what Space turns the typed word into ("" leaves it)
};

class Dict {
 public:
  Learned e[kLearnMax];
  int count = 0;
  Pair pr[kPairMax];
  int pairs = 0;
  uint32_t clock = 0;

  const Learned* learned(const char* w, size_t len) const {
    for (int i = 0; i < count; i++)
      if (eqNoCaseN(e[i].w, w, len)) return &e[i];
    return nullptr;
  }

  void learnWord(const char* w, size_t len) {
    if (len < 2 || len >= kWordMax) return;
    char word[kWordMax];
    memcpy(word, w, len);
    word[len] = 0;
    for (size_t i = 0; i < len; i++)
      if (word[i] >= '0' && word[i] <= '9') return;   // not numbers or codes
    clock++;
    for (int i = 0; i < count; i++)
      if (eqNoCase(e[i].w, word)) { if (e[i].n < 60000) e[i].n++; e[i].last = clock; return; }
    int slot = count;
    if (count == kLearnMax) {   // full: the least used, then the oldest, goes
      slot = 0;
      for (int i = 1; i < count; i++)
        if (e[i].n < e[slot].n || (e[i].n == e[slot].n && e[i].last < e[slot].last)) slot = i;
    } else {
      count++;
    }
    memcpy(e[slot].w, word, len + 1);
    e[slot].n = 1;
    e[slot].last = clock;
  }

  void learnPair(const char* a, const char* b, size_t bl) {
    if (!*a || bl == 0 || bl >= kWordMax) return;
    char w[kWordMax];
    memcpy(w, b, bl);
    w[bl] = 0;
    for (size_t i = 0; i < bl; i++)
      if (w[i] >= '0' && w[i] <= '9') return;
    // A dictionary word goes in as the dictionary spells it: "The" at the start
    // of a sentence is still "the"; "I" stays "I".
    char disp[kWordMax];
    if (dict::find(w, bl, disp)) memcpy(w, disp, strlen(disp) + 1);
    clock++;
    for (int i = 0; i < pairs; i++)
      if (!strcmp(pr[i].a, a) && eqNoCase(pr[i].b, w)) { if (pr[i].n < 60000) pr[i].n++; pr[i].last = clock; return; }
    int slot = pairs;
    if (pairs == kPairMax) {
      slot = 0;
      for (int i = 1; i < pairs; i++)
        if (pr[i].n < pr[slot].n || (pr[i].n == pr[slot].n && pr[i].last < pr[slot].last)) slot = i;
    } else {
      pairs++;
    }
    strncpy(pr[slot].a, a, kWordMax - 1);
    pr[slot].a[kWordMax - 1] = 0;
    memcpy(pr[slot].b, w, strlen(w) + 1);
    pr[slot].n = 1;
    pr[slot].last = clock;
  }

  // Every word of a sent message, and each word with the one before it.
  void learnText(const char* t) {
    if (!t) return;
    const char* const t0 = t;
    while (*t) {
      while (*t && !wordChar(*t)) t++;
      const char* s = t;
      while (*t && wordChar(*t)) t++;
      if (t <= s || *s == '\'') continue;
      if (s > t0 && s[-1] == '@') continue;   // a mention
      const size_t len = (size_t)(t - s);
      learnWord(s, len);
      char prev[kWordMax];
      prevWord(t0, (size_t)(s - t0), prev);
      learnPair(prev, s, len);
    }
  }

  // ---- ranking --------------------------------------------------------------

  int learnedBonus(const char* w, size_t len) const {
    const Learned* l = learned(w, len);
    if (!l) return 0;
    const uint32_t age = clock - l->last;
    return 14 + 6 * ilog2((uint32_t)l->n + 1) + (age < 200 ? 6 : age < 1000 ? 3 : 0);
  }
  // The built-in table's words after prev ("" when it has none).
  static const char* nextRow(const char* prev) {
    const size_t pl = strlen(prev);
    if (!pl) return "";
    for (int i = 0; i < kNextN; i++)
      if (!strncmp(kNext[i], prev, pl) && kNext[i][pl] == '|') return kNext[i] + pl + 1;
    return "";
  }
  // How strongly `w` follows `prev`: your pairs, then the built-in table.
  // `mine`: the indexes of your pairs after prev, when the caller has them;
  // `row`: nextRow(prev), likewise.
  int nextBonus(const char* prev, const char* w, size_t len, const int16_t* mine = nullptr, int nmine = -1,
                const char* row = nullptr) const {
    if (!*prev) return 0;
    int b = 0;
    for (int k = 0; k < (nmine >= 0 ? nmine : pairs); k++) {
      const int i = nmine >= 0 ? mine[k] : k;
      if (!strcmp(pr[i].a, prev) && eqNoCaseN(pr[i].b, w, len)) {
        const int v = 25 + 8 * ilog2((uint32_t)pr[i].n + 1);
        b = v > 60 ? 60 : v;
        break;
      }
    }
    int k = 0;
    for (const char* s = row ? row : nextRow(prev); *s;) {
      const char* e2 = s;
      while (*e2 && *e2 != ' ') e2++;
      if ((size_t)(e2 - s) == len && startsNoCase(s, w, len)) {
        const int v = 22 - 2 * k;
        if (v > b) b = v;
        break;
      }
      k++;
      s = *e2 ? e2 + 1 : e2;
    }
    return b;
  }

  // Suggestions for the word starting at `at` (wl bytes typed of it) in `text`.
  void suggest(const char* text, size_t at, size_t wl, Suggestions& out) const {
    out.n = 0;
    out.space[0] = 0;
    if (wl >= kWordMax) return;
    const char* p = text + at;
    char prev[kWordMax];
    prevWord(text, at, prev);
    struct C { char w[kWordMax]; int score; bool fix; int pen; };
    C c[20];
    int n = 0;
    int lo_i = -1;   // the weakest, once full
    auto add = [&](const char* w, size_t len, int score, bool fix, int pen = 0, bool fresh = false) {
      if (len >= kWordMax) return;
      if (n == 20 && lo_i >= 0 && score <= c[lo_i].score) return;   // can't make the cut
      for (int i = 0; fresh ? false : i < n; i++)
        if (eqNoCaseN(c[i].w, w, len)) { if (score > c[i].score) { c[i].score = score; c[i].fix = fix; c[i].pen = pen; } return; }
      int slot = n;
      if (n == 20) {
        slot = 0;
        for (int i = 1; i < n; i++) if (c[i].score < c[slot].score) slot = i;
        if (score <= c[slot].score) return;
      } else {
        n++;
      }
      memcpy(c[slot].w, w, len);
      c[slot].w[len] = 0;
      c[slot].score = score;
      c[slot].fix = fix;
      c[slot].pen = pen;
      if (n == 20) {
        lo_i = 0;
        for (int i = 1; i < n; i++) if (c[i].score < c[lo_i].score) lo_i = i;
      }
    };
    // The few of your words and pairs that can matter here, found once: a
    // one-letter prefix has a thousand completions to score.
    int16_t mine_w[kLearnMax], mine_p[kPairMax];
    int nw = 0, np = 0;
    for (int i = 0; i < count; i++) if (startsNoCase(e[i].w, p, wl)) mine_w[nw++] = (int16_t)i;
    for (int i = 0; i < pairs; i++) if (!strcmp(pr[i].a, prev)) mine_p[np++] = (int16_t)i;
    const char* const row = nextRow(prev);
    auto score = [&](const char* w, size_t len, int fr) {
      int lb = 0;
      if (startsNoCase(w, p, wl)) {
        for (int k = 0; k < nw; k++)
          if (eqNoCaseN(e[mine_w[k]].w, w, len)) { lb = learnedBonus(w, len); break; }
      } else {
        lb = learnedBonus(w, len);
      }
      return (fr ? fr : 38) + lb + nextBonus(prev, w, len, mine_p, np, row);
    };

    if (wl == 0) {
      // The next word, from the one before.
      if (!*prev) return;
      for (int i = 0; i < pairs; i++)
        if (!strcmp(pr[i].a, prev)) {
          const size_t l = strlen(pr[i].b);
          add(pr[i].b, l, score(pr[i].b, l, dict::find(pr[i].b, l)) + 10, false);
        }
      for (const char* s = row; *s;) {
        const char* e2 = s;
        while (*e2 && *e2 != ' ') e2++;
        const size_t l = (size_t)(e2 - s);
        add(s, l, score(s, l, dict::find(s, l)), false);
        s = *e2 ? e2 + 1 : e2;
      }
    } else {
      // Completions: the dictionary's and your words.
      dict::scanFrom(dict::blockFor(p, wl), [&](const char* w, size_t len, uint8_t fr) {
        const int cmp = cmpNoCase(w, len < wl ? len : wl, p, wl);
        if (cmp < 0 || (cmp == 0 && len < wl)) return true;
        if (cmp > 0) return false;
        if (len <= wl) return true;
        add(w, len, score(w, len, fr), false, 0, true);   // dictionary words are unique
        return true;
      });
      for (int i = 0; i < count; i++) {
        const size_t len = strlen(e[i].w);
        if (len > wl && startsNoCase(e[i].w, p, wl)) add(e[i].w, len, score(e[i].w, len, dict::find(e[i].w, len)), false);
      }
      // Corrections of the word as typed (two letters at least).
      if (wl >= 2) {
        char t[kWordMax + 2];
        auto tryFix = [&](size_t len, int pen) {
          if (len == 0 || len >= kWordMax) return;
          char disp[kWordMax];
          int fr = dict::find(t, len, disp);
          if (fr) add(disp, len, score(disp, len, fr) - pen, true, pen);
        };
        // Your own words, one slip away (checked directly: cheaper than looking
        // each edit up in them).
        for (int i = 0; i < count; i++) {
          int pen = 0;
          const size_t len = strlen(e[i].w);
          if (oneSlip(p, wl, e[i].w, len, &pen)) add(e[i].w, len, score(e[i].w, len, dict::find(e[i].w, len)) - pen, true, pen);
        }
        static const char kAlpha[] = "abcdefghijklmnopqrstuvwxyz'";
        for (size_t i = 0; i < wl; i++) {                    // one letter too many
          memcpy(t, p, i); memcpy(t + i, p + i + 1, wl - i - 1);
          tryFix(wl - 1, wl <= 3 ? 25 : 16);   // "im" isn't "I"
        }
        for (size_t i = 0; i + 1 < wl; i++) {                // two swapped
          memcpy(t, p, wl); t[i] = p[i + 1]; t[i + 1] = p[i];
          if (t[i] != t[i + 1]) tryFix(wl, 12);
        }
        for (size_t i = 0; i < wl; i++)                      // one wrong
          for (const char* a = kAlpha; *a; a++) {
            if (lower(p[i]) == *a) continue;
            memcpy(t, p, wl); t[i] = *a;
            tryFix(wl, keysNear(p[i], *a) ? 12 : 20);
          }
        if (wl + 1 < kWordMax)
          for (size_t i = 0; i <= wl; i++)                   // one missing
            for (const char* a = kAlpha; *a; a++) {
              memcpy(t, p, i); t[i] = *a; memcpy(t + i + 1, p + i, wl - i);
              tryFix(wl + 1, *a == '\'' ? -8 : 16);          // "dont" -> "don't": always meant
            }
      }
    }
    // Best first.
    for (int i = 1; i < n; i++)
      for (int j = i; j > 0 && c[j].score > c[j - 1].score; j--) { C tmp = c[j]; c[j] = c[j - 1]; c[j - 1] = tmp; }

    // What Space does with the typed word.
    int space = -1;
    if (wl > 0) {
      char disp[kWordMax];
      const bool known = dict::find(p, wl, disp) || learned(p, wl) || wl == 1;
      if (known) {
        // A real word stays; only its spelling may change ("i" -> "I").
        const int fw = dict::find(p, wl, disp);
        if (fw && strncmp(disp, p, wl) && disp[0] >= 'A' && disp[0] <= 'Z' &&
            !(p[0] >= 'A' && p[0] <= 'Z')) {
          memcpy(out.space, disp, wl + 1);
        }
        // Two or three letters that are only a word in the dictionary's long tail
        // ("th", "ab", "ye", "eve", an abbreviation or a fragment) are the start
        // of a word: Space completes them when the best completion is clearly
        // likelier ("th" -> "the", "co" -> "could", "eve" -> "even"). A common
        // short word ("go", "to", "be"), a chat word ("hi", "ok", "lol") and one
        // you have sent stay as typed.
        if (!out.space[0] && fw && fw < kShortKeepFreq && wl >= 2 && wl <= 3 && !learned(p, wl) &&
            !chatShort(p, wl)) {
          const int sw = score(p, wl, fw);
          for (int i = 0; i < n; i++) {
            if (c[i].fix || strlen(c[i].w) <= wl || !startsNoCase(c[i].w, p, wl)) continue;
            if (c[i].score - sw >= (wl == 2 ? kShortCompleteMargin2 : kShortCompleteMargin3)) space = i;
            break;   // best first: only the likeliest completion counts
          }
        }
      } else {
        // Not a word: the best suggestion, completion or correction alike. A
        // short word is only corrected for a likely slip (a neighbouring key,
        // two letters swapped, a missing apostrophe): "thx" isn't "the".
        for (int i = 0; i < n && space < 0; i++) {
          if (c[i].score < 25) break;
          if (!c[i].fix || wl >= 4 || c[i].pen <= 12) space = i;
        }
      }
    }
    if (space > 0) { C tmp = c[space]; for (int j = space; j > 0; j--) c[j] = c[j - 1]; c[0] = tmp; space = 0; }

    // A capital typed, or the start of a sentence: capitalised words.
    const bool cap1 = wl > 0 ? (p[0] >= 'A' && p[0] <= 'Z') : !strcmp(prev, "^");
    for (int i = 0; i < n && out.n < kSuggest; i++) {
      if (wl > 0 && eqNoCaseN(c[i].w, p, wl) && strlen(c[i].w) == wl) continue;   // never the word itself
      memcpy(out.w[out.n], c[i].w, strlen(c[i].w) + 1);
      if (cap1) out.w[out.n][0] = upper(out.w[out.n][0]);
      out.n++;
    }
    if (space == 0 && out.n > 0 && !out.space[0]) memcpy(out.space, out.w[0], strlen(out.w[0]) + 1);
    // A spelling fix of a real word ("i" -> "I") leads the strip too.
    if (space < 0 && out.space[0]) {
      int k = out.n < kSuggest ? out.n : kSuggest - 1;
      for (int j = 0; j < out.n; j++) if (!strcmp(out.w[j], out.space)) { k = j; break; }
      for (int j = k; j > 0; j--) memcpy(out.w[j], out.w[j - 1], kWordMax);
      memcpy(out.w[0], out.space, strlen(out.space) + 1);
      if (out.n < kSuggest && k == out.n) out.n++;
    }
  }

  // The typed word was kept (Space's change undone): it's a word now.
  void keep(const char* w, size_t len) { learnWord(w, len); }

  // Completions and corrections of a bare word (no sentence around it).
  int suggest(const char* prefix, size_t plen, char out[][kWordMax], int cap = kSuggest) const {
    if (plen < 1 || plen >= kWordMax) return 0;
    char text[kWordMax];
    memcpy(text, prefix, plen);
    text[plen] = 0;
    Suggestions s;
    suggest(text, 0, plen, s);
    int k = 0;
    for (; k < s.n && k < cap; k++) memcpy(out[k], s.w[k], strlen(s.w[k]) + 1);
    return k;
  }

  // "word count\n" lines, then "before after count\n" pairs (counts only;
  // recency starts over after a restart). Older firmware reads the words and
  // skips the pairs.
  size_t save(char* out, size_t cap) const {
    size_t o = 0;
    auto num = [&](unsigned v) {
      char d[6];
      int k = 0;
      do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 5);
      while (k) out[o++] = d[--k];
    };
    for (int i = 0; i < count; i++) {
      const size_t l = strlen(e[i].w);
      if (o + l + 8 >= cap) break;
      memcpy(out + o, e[i].w, l);
      o += l;
      out[o++] = ' ';
      num(e[i].n);
      out[o++] = '\n';
    }
    for (int i = 0; i < pairs; i++) {
      const size_t la = strlen(pr[i].a), lb = strlen(pr[i].b);
      if (o + la + lb + 9 >= cap) break;
      memcpy(out + o, pr[i].a, la); o += la;
      out[o++] = ' ';
      memcpy(out + o, pr[i].b, lb); o += lb;
      out[o++] = ' ';
      num(pr[i].n);
      out[o++] = '\n';
    }
    if (o < cap) out[o] = 0;
    return o;
  }
  void load(const char* s, size_t len) {
    count = 0;
    pairs = 0;
    clock = 0;
    size_t i = 0;
    while (i < len) {
      const char* tok[3];
      size_t tl[3];
      int nt = 0;
      while (i < len && s[i] != '\n') {
        while (i < len && s[i] == ' ') i++;
        if (i >= len || s[i] == '\n') break;
        const size_t a = i;
        while (i < len && s[i] != ' ' && s[i] != '\n') i++;
        if (nt < 3) { tok[nt] = s + a; tl[nt] = i - a; }
        nt++;
      }
      i++;
      unsigned v = 0;
      if (nt == 2 || nt == 3) {
        const char* d = tok[nt - 1];
        for (size_t k = 0; k < tl[nt - 1] && d[k] >= '0' && d[k] <= '9'; k++) v = v * 10 + (unsigned)(d[k] - '0');
      }
      if (!v) continue;
      if (v > 60000) v = 60000;
      if (nt == 2 && count < kLearnMax && tl[0] >= 2 && tl[0] < kWordMax) {
        memcpy(e[count].w, tok[0], tl[0]);
        e[count].w[tl[0]] = 0;
        e[count].n = (uint16_t)v;
        e[count].last = 0;
        count++;
      } else if (nt == 3 && pairs < kPairMax && tl[0] && tl[0] < kWordMax && tl[1] && tl[1] < kWordMax) {
        memcpy(pr[pairs].a, tok[0], tl[0]);
        pr[pairs].a[tl[0]] = 0;
        memcpy(pr[pairs].b, tok[1], tl[1]);
        pr[pairs].b[tl[1]] = 0;
        pr[pairs].n = (uint16_t)v;
        pr[pairs].last = 0;
        pairs++;
      }
    }
  }
};

}  // namespace predict
