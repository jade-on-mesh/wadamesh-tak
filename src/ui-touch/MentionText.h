// SPDX-License-Identifier: GPL-3.0-or-later
// Mentions in a message as MeshCore Open shows them. On the air a mention is
// "@[name]" (a reply starts with one: "@[Bob]: hi"); the app shows "@name" on a
// chip. unbracket() turns the text a bubble shows into that ("@[Bob]: hi" ->
// "@Bob: hi", in place) and says where each "@name" now is, so the bubble draws a
// chip behind it. Measuring and drawing both read the rewritten text, so wraps and
// heights stay right. Who is mentioned doesn't matter here; a mention of you also
// tints the bubble (textMentionsMe, which reads the text as received).
//
// Header-only: test/test_mention_text.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mentiontext {

static constexpr int kMax = 4;            // chips per message
static constexpr size_t kNameMax = 48;    // bytes between the brackets

struct Spans {
  int n = 0;
  uint16_t start[kMax];   // byte offsets of "@name" in the rewritten text
  uint16_t end[kMax];
};

// Rewrites every "@[name]" in `text` to "@name" (a name: 1..kNameMax bytes, no
// '[', ']' or line break) and records where the first kMax are.
inline void unbracket(char* text, Spans* out) {
  if (out) out->n = 0;
  if (!text) return;
  char* w = text;
  const char* r = text;
  while (*r) {
    if (r[0] == '@' && r[1] == '[') {
      const char* name = r + 2;
      const char* q = name;
      while (*q && *q != ']' && *q != '[' && *q != '\n' && (size_t)(q - name) <= kNameMax) ++q;
      const size_t nl = (size_t)(q - name);
      if (*q == ']' && nl >= 1 && nl <= kNameMax) {
        const size_t at = (size_t)(w - text);
        *w++ = '@';
        memmove(w, name, nl);   // w never passes r, so moving left is safe
        w += nl;
        if (out && out->n < kMax && at + 1 + nl <= 0xFFFF) {
          out->start[out->n] = (uint16_t)at;
          out->end[out->n] = (uint16_t)(at + 1 + nl);
          ++out->n;
        }
        r = q + 1;
        continue;
      }
    }
    *w++ = *r++;
  }
  *w = '\0';
}

}  // namespace mentiontext
