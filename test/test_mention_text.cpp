// SPDX-License-Identifier: GPL-3.0-or-later
//
// MentionText.h: "@[name]" shows as "@name", with where the chip goes.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/MentionText.h"

using namespace mentiontext;

static void check(const char* in, const char* want, int n, const char* const* names) {
  char buf[256];
  snprintf(buf, sizeof buf, "%s", in);
  Spans s;
  unbracket(buf, &s);
  if (strcmp(buf, want) != 0) { fprintf(stderr, "\"%s\" -> \"%s\", want \"%s\"\n", in, buf, want); assert(0); }
  assert(s.n == n);
  for (int i = 0; i < n; ++i) {
    const size_t len = s.end[i] - s.start[i];
    assert(buf[s.start[i]] == '@');
    assert(len == 1 + strlen(names[i]) && strncmp(buf + s.start[i] + 1, names[i], len - 1) == 0);
  }
}

int main() {
  // The one in the report: a reply, a name with an emoji and spaces.
  { const char* nm[] = {"meshpunk dude \xE2\x9C\x8C deck"};
    check("@[meshpunk dude \xE2\x9C\x8C deck]: Yo dude! How have you been?",
          "@meshpunk dude \xE2\x9C\x8C deck: Yo dude! How have you been?", 1, nm); }
  { const char* nm[] = {"Bob"}; check("@[Bob] hi", "@Bob hi", 1, nm); }
  { const char* nm[] = {"Ann", "Bob"}; check("hey @[Ann] and @[Bob]!", "hey @Ann and @Bob!", 2, nm); }
  // Not mentions: left as they are.
  check("@[] hi", "@[] hi", 0, nullptr);
  check("@[Bob hi", "@[Bob hi", 0, nullptr);
  check("@[Bob\n] hi", "@[Bob\n] hi", 0, nullptr);
  check("@Bob [x]", "@Bob [x]", 0, nullptr);
  check("email@[127.0.0.1", "email@[127.0.0.1", 0, nullptr);
  check("plain text", "plain text", 0, nullptr);
  check("", "", 0, nullptr);
  // A name too long to be one.
  { char in[128] = "@[";
    memset(in + 2, 'x', kNameMax + 1);
    strcat(in, "]");
    char want[128];
    snprintf(want, sizeof want, "%s", in);
    check(in, want, 0, nullptr); }
  // More than kMax: all unbracketed, the first kMax get chips.
  { const char* nm[] = {"a", "b", "c", "d"};
    check("@[a]@[b]@[c]@[d]@[e]", "@a@b@c@d@e", 4, nm); }
  // Null-safe.
  unbracket(nullptr, nullptr);
  { char b[] = "@[x]"; unbracket(b, nullptr); assert(!strcmp(b, "@x")); }
  puts("test_mention_text: ok");
  return 0;
}
