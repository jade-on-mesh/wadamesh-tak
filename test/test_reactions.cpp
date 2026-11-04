// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reactions.h against MeshCore Open's own values (test/reaction_helper_test.dart).

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/Reactions.h"

using namespace reactions;

static void ours() {
  // computeReactionHash: the app's legacy-native golden vectors.
  assert(hashOf(1700000000, "Bob", "hello") == 0xe779);
  assert(hashOf(1700000000, nullptr, "hi") == 0xb5bd);
  assert(hashOf(1700000000, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x98\x80", "x") == 0x7fee);   // "Привет😀"
  // Only the first five UTF-16 units of the text count.
  assert(hashOf(1700000000, "Bob", "hello world") == hashOf(1700000000, "Bob", "hello there"));
  assert(hashOf(1700000000, "Bob", "hello") != hashOf(1700000001, "Bob", "hello"));
  assert(hashOf(1700000000, "Bob", "hello") != hashOf(1700000000, "Ann", "hello"));

  // The emoji table: 184 entries, 👍 = 00, ❤️ = 01, as emojiToIndex.
  assert(kEmojiCount == 184);
  assert(emojiIndex("\xF0\x9F\x91\x8D") == 0);               // 👍
  assert(emojiIndex("\xE2\x9D\xA4\xEF\xB8\x8F") == 1);       // ❤️
  assert(emojiIndex("\xE2\x9D\xA4") == -1);                  // ❤ without FE0F isn't in the list
  assert(emojiIndex("\xF0\x9F\xA6\x84") == -1);              // 🦄
  assert(emojiIndex("") == -1 && emojiIndex("invalid") == -1);
  // A duplicate is sent as its first place, and every place reads back.
  for (int i = 0; i < kEmojiCount; i++) {
    const int first = emojiIndex(kEmojis[i]);
    assert(first >= 0 && first <= i && strcmp(kEmojis[first], kEmojis[i]) == 0);
  }
  assert(emojiIndex(kEmojis[kEmojiCount - 1]) == kEmojiCount - 1);   // 🚀, the last

  // encodeReaction / parseReactionOurs.
  char t[16];
  format(0xe779, 0, t, sizeof t);
  assert(strcmp(t, "r:e779:00") == 0);
  format(0x00af, 0x2a, t, sizeof t);
  assert(strcmp(t, "r:00af:2a") == 0);
  uint16_t h = 0;
  int e = -1;
  assert(parse("r:e779:00", &h, &e) && h == 0xe779 && e == 0);
  assert(parse("r:00af:b7", &h, &e) && e == 0xb7);
  assert(!parse("r:00af:b8", &h, &e));     // 184: past the table
  assert(!parse("r:E779:00", &h, &e));     // uppercase isn't the app's pattern
  assert(!parse("r:e779:00 ", &h, &e));
  assert(!parse(" r:e779:00", &h, &e));
  assert(!parse("r:e77:00", &h, &e));
  assert(!parse("r:e779:0", &h, &e));
  assert(!parse(nullptr, &h, &e));
  assert(isReaction("r:1234:05") && !isReaction("hello"));

  // A channel reply is hashed without its mention, as the app keeps it.
  assert(strcmp(replyBody("@[Bob] hello there"), "hello there") == 0);
  assert(strcmp(replyBody("@[Bob]\n  hi"), "hi") == 0);
  assert(strcmp(replyBody("@[Bob]hello"), "@[Bob]hello") == 0);    // no space: not a reply
  assert(strcmp(replyBody("@[] hello"), "@[] hello") == 0);         // [^\]]+ needs a name
  assert(strcmp(replyBody("@[Bob] "), "@[Bob] ") == 0);             // (.+) needs text
  assert(strcmp(replyBody("hi @[Bob] x"), "hi @[Bob] x") == 0);
  assert(hashOfMessage(1700000000, "Bob", "@[Ann] hello", true) == 0xe779);
  assert(hashOfMessage(1700000000, nullptr, "hi", false) == 0xb5bd);
}

static void mc1() {
  // computeReactionHashMC1(1234567890, 'Alice', 'Hello world!') == 'tyvbkb33'
  char hh[9];
  hashMc1(1234567890, "Hello world!", hh);
  assert(strcmp(hh, "tyvbkb33") == 0);

  Mc1 m;
  // '@[entropy 📓]🥳\nb3je6qf7'
  assert(parseMc1("@[entropy \xF0\x9F\x93\x93]\xF0\x9F\xA5\xB3\nb3je6qf7", &m));
  assert(strcmp(m.emoji, "\xF0\x9F\xA5\xB3") == 0 && m.has_name &&
         strcmp(m.name, "entropy \xF0\x9F\x93\x93") == 0 && strcmp(m.hash, "b3je6qf7") == 0);
  // Its legacy order, and a DM's (no name).
  assert(parseMc1("\xF0\x9F\xA5\xB3@[entropy \xF0\x9F\x93\x93]\nb3je6qf7", &m));
  assert(strcmp(m.emoji, "\xF0\x9F\xA5\xB3") == 0 && strcmp(m.name, "entropy \xF0\x9F\x93\x93") == 0);
  assert(parseMc1("\xF0\x9F\xA5\xB3\nb3je6qf7", &m) && !m.has_name && strcmp(m.emoji, "\xF0\x9F\xA5\xB3") == 0);
  assert(parseMc1("\xF0\x9F\xA5\xB3\nB3JE6QF7", &m) && strcmp(m.hash, "b3je6qf7") == 0);   // case-free hash

  // The app's must-parse list (both orders).
  const char* ok[] = {
      "\xF0\x9F\xA5\xB3\n01234567",
      "@[entropy]\xF0\x9F\xA5\xB3\n01234567",
      "\xF0\x9F\x91\x8C\xF0\x9F\x8F\xBF\n01234567",                       // 👌🏿
      "@[entropy]\xF0\x9F\x91\x8C\xF0\x9F\x8F\xBF\n01234567",
      "@[entropy]\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\xA7\x92\n01234567",   // 🧑‍🧑‍🧒
      "\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\xA7\x92\xE2\x80\x8D\xF0\x9F\xA7\x92\n01234567",
      "\xF0\x9F\xA5\xB3@[entropy]\n01234567",
      "\xF0\x9F\x91\x8C\xF0\x9F\x8F\xBF@[entropy]\n01234567",
      "\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\xA7\x92@[entropy]\n01234567",
      "\xE2\x9D\xA4\xEF\xB8\x8F\n01234567",                                // ❤️
  };
  for (const char* s : ok) {
    if (!parseMc1(s, &m)) { fprintf(stderr, "should parse: %s\n", s); assert(0); }
  }
  // ...and its must-not list.
  const char* bad[] = {
      "test\nabcdefgh",
      "\xF0\x9F\xA5\xB3\nhashistoolong",
      "\xF0\x9F\xA5\xB3\nshort",
      "\xF0\x9F\xA5\xB3\xF0\x9F\xA5\xB3\n01234567",   // two emoji, not one grapheme
      "X\n01234567",
      "\xF0\x9F\xA5\xB3\nabcuuxyz",                   // 'u' isn't a hash character
      "r:e779:00",
      "hello there",
      "\xF0\x9F\xA5\xB3\nline two\n01234567",         // a newline before the last one
  };
  for (const char* s : bad) {
    if (parseMc1(s, &m)) { fprintf(stderr, "should not parse: %s\n", s); assert(0); }
  }
}

static void tally() {
  Tally t;
  assert(t.add(0, "Ann"));
  assert(t.add(0, "Bob"));
  assert(!t.add(0, "Ann"));   // once per person per emoji
  assert(t.add(1, "Ann"));
  char s[64];
  t.text(s, sizeof s);
  assert(strcmp(s, "\xF0\x9F\x91\x8D 2  \xE2\x9D\xA4\xEF\xB8\x8F") == 0);   // "👍 2  ❤️"
  Tally empty;
  empty.text(s, sizeof s);
  assert(s[0] == '\0');
}

int main() {
  ours();
  mc1();
  tally();
  puts("test_reactions: ok");
  return 0;
}
