// Host test: every baked emoji unpacks (scripts/build/pack-emoji.py ->
// src/ui-touch/emoji_data.c) to exactly the pixels it was baked with, at its
// size, and the same pointer comes back each time.
//
//   cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined
//      -I test/stub_lvgl -I src/ui-touch -I test -o /tmp/t
//      test/test_emoji_pack.c src/ui-touch/emoji_data.c
#include <assert.h>
#include <stdio.h>
#include "emoji_data.h"
#include "emoji_pack_expected.h"

static uint32_t fnv(const uint8_t* b, uint32_t n) {
  uint32_t h = 0x811C9DC5u;
  for (uint32_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x01000193u;
  return h;
}

int main(void) {
  const int n = (int)(sizeof kEmojiExpected / sizeof kEmojiExpected[0]);
  for (int i = 0; i < n; ++i) {
    const lv_img_dsc_t* d = emojiGlyphLookup(kEmojiExpected[i].cp);
    assert(d);
    assert(d->header.cf == LV_IMG_CF_TRUE_COLOR_ALPHA);
    assert((int)d->header.w == kEmojiExpected[i].w && (int)d->header.h == kEmojiExpected[i].h);
    assert(d->data_size == (uint32_t)(kEmojiExpected[i].w * kEmojiExpected[i].h * 3));
    assert(fnv(d->data, d->data_size) == kEmojiExpected[i].fnv);
    assert(emojiGlyphLookup(kEmojiExpected[i].cp) == d);   // kept, not unpacked again
  }
  assert(!emojiGlyphLookup(0x41));         // 'A': no glyph
  assert(!emojiGlyphLookup(0x10FFFF));
  assert(kEmojiGlyphCount == 280);
  printf("emoji pack: %d glyphs ok\n", n);
  return 0;
}
