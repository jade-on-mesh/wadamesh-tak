// SPDX-License-Identifier: GPL-3.0-or-later
//
// EmojiPack.h against the committed emoji/emoji-pack.wep (make-emoji-pack.py writes
// the expected values): it parses, every image unpacks, the samples' pixels are
// the generator's, and a damaged file is refused.
//
//   g++ -std=c++17 -I src -o /tmp/t test/test_emoji_pack_file.cpp && /tmp/t emoji/emoji-pack.wep

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "ui-touch/EmojiPack.h"
#include "ui-touch/FlagText.h"
#include "emoji_pack_file_expected.h"

static uint32_t fnv(const uint8_t* b, size_t n) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x01000193u;
  return h;
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "emoji/emoji-pack.wep";
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "can't open %s\n", path); return 1; }
  std::vector<uint8_t> b;
  uint8_t chunk[4096];
  size_t r;
  while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) b.insert(b.end(), chunk, chunk + r);
  fclose(f);
  assert(b.size() == kPackBytes);

  emojipack::View v;
  assert(emojipack::parse(b.data(), b.size(), &v));
  assert(v.count == kPackCount);

  // Every image unpacks; the zero-width ones are skin tones, keycap, tags.
  uint8_t px[16 * 16 * 3], tmp[16 * 16 * 3];
  int drawn = 0, flags = 0;
  for (int i = 0; i < v.count; ++i) {
    const emojipack::Entry e = emojipack::entryAt(v, i);
    assert(emojipack::find(v, e.cp) == i);
    if (!e.w) {
      assert((e.cp >= 0x1F3FB && e.cp <= 0x1F3FF) || e.cp == 0x20E3 || (e.cp >= 0xE0020 && e.cp <= 0xE007F));
      continue;
    }
    assert(emojipack::unpack(v, i, px, tmp));
    ++drawn;
    flags += flagtext::isKey(e.cp);
  }
  assert(drawn == kPackDrawn);
  assert(flags == kPackFlags);
  // The flags LVGL reads (FlagText.h keys): Lesotho, the one baked, and others.
  assert(emojipack::find(v, flagtext::key(0x1F1F1, 0x1F1F8)) >= 0);   // LS
  assert(emojipack::find(v, flagtext::key(0x1F1FA, 0x1F1F8)) >= 0);   // US
  assert(emojipack::find(v, flagtext::key(0x1F1F8, 0x1F1EA)) >= 0);   // SE

  for (const auto& s : kPackSamples) {
    const int i = emojipack::find(v, s.cp);
    assert(i >= 0);
    const emojipack::Entry e = emojipack::entryAt(v, i);
    assert(e.w == 16 && e.h == 16);
    assert(emojipack::unpack(v, i, px, tmp));
    if (fnv(px, sizeof px) != s.fnv) { fprintf(stderr, "U+%X pixels differ\n", (unsigned)s.cp); return 1; }
  }
  assert(emojipack::find(v, 'A') < 0);
  assert(emojipack::find(v, 0x10FFFF) < 0);

  // Damage is refused, not trusted.
  std::vector<uint8_t> d = b;
  d[0] = 'X';
  assert(!emojipack::parse(d.data(), d.size(), &v));
  d = b;
  assert(!emojipack::parse(d.data(), d.size() - 1, &v));   // the last image runs off the end
  d = b;
  d[12] = d[13] = d[14] = 0xFF;   // the first codepoint now sorts after the second
  assert(!emojipack::parse(d.data(), d.size(), &v));
  d = b;
  d[4] = 1;        // a version this firmware doesn't read (v1 had no names)
  assert(!emojipack::parse(d.data(), d.size(), &v));
  assert(!emojipack::parse(b.data(), 8, &v));
  assert(!emojipack::parse(nullptr, 0, &v));

  // Names: the text to insert and what search reads.
  {
    int named = 0;
    for (int i = 0; i < v.count; ++i) named += emojipack::utf8At(v, i) != nullptr;
    assert(named == kPackNamed);
    const int fish = emojipack::find(v, 0x1F420);
    assert(!strcmp(emojipack::utf8At(v, fish), "\xF0\x9F\x90\xA0"));
    assert(!strcmp(emojipack::textAt(v, fish), "tropical fish|animal fish fishes tropical"));
    const int heart = emojipack::find(v, 0x2764);
    assert(!strcmp(emojipack::utf8At(v, heart), "\xE2\x9D\xA4\xEF\xB8\x8F"));   // ❤️ with its FE0F
    const int us = emojipack::find(v, flagtext::key(0x1F1FA, 0x1F1F8));
    assert(!strcmp(emojipack::utf8At(v, us), "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"));   // both letters
    assert(emojipack::utf8At(v, emojipack::find(v, 0x1F3FB)) == nullptr);           // a skin tone: not offered

    int r[64];
    int n = emojipack::search(v, "fish", r, 64);
    assert(n >= 3);
    bool has_fish = false;
    for (int k = 0; k < n; ++k) has_fish |= emojipack::entryAt(v, r[k]).cp == 0x1F420;
    assert(has_fish);
    assert(emojipack::entryAt(v, r[0]).cp == 0x1F41F);   // "fish" itself first: its name starts with it
    n = emojipack::search(v, "TROP fi", r, 64);          // any case, word starts, any order
    assert(n >= 1 && emojipack::entryAt(v, r[0]).cp == 0x1F420);
    n = emojipack::search(v, "united states", r, 64);
    assert(n >= 1 && emojipack::entryAt(v, r[0]).cp == flagtext::key(0x1F1FA, 0x1F1F8));
    n = emojipack::search(v, "lesotho", r, 64);
    assert(n == 1 && emojipack::entryAt(v, r[0]).cp == flagtext::key(0x1F1F1, 0x1F1F8));
    n = emojipack::search(v, "gold medal", r, 64);   // "gold" is only a keyword of 🥇
    bool has_gold = false;
    for (int k = 0; k < n; ++k) has_gold |= emojipack::entryAt(v, r[k]).cp == 0x1F947;
    assert(has_gold);
    assert(emojipack::search(v, "  ", r, 64) == 0);
    assert(emojipack::search(v, "zzqxj", r, 64) == 0);
    assert(emojipack::search(v, "heart", r, 3) == 3);    // the cap holds
  }

  // A broken LZ stream fails rather than writing past the image.
  {
    std::vector<uint8_t> c = b;
    emojipack::View cv;
    assert(emojipack::parse(c.data(), c.size(), &cv));
    const int i = emojipack::find(cv, 0x1F420);
    const emojipack::Entry e = emojipack::entryAt(cv, i);
    c[cv.data + e.off] = 0x7F;   // a 128-byte literal run in a 256-byte plane... then garbage
    c[cv.data + e.off + 1] = 0xFF;
    uint8_t px2[16 * 16 * 3], tmp2[16 * 16 * 3];
    (void)emojipack::unpack(cv, i, px2, tmp2);   // must not crash (ASan checks); result may be false
  }

  printf("test_emoji_pack_file: ok (%d entries, %d images, %zu bytes)\n", (int)kPackCount, drawn, b.size());
  return 0;
}
