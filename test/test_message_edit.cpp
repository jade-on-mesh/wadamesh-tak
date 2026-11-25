// SPDX-License-Identifier: GPL-3.0-or-later
// Edits to sent messages from MeshCore Open TAK (ui-touch/MessageEdit.h).
// Build: g++ -std=c++17 -Wall -Wextra -I src -o /tmp/test_message_edit test/test_message_edit.cpp
//
// The wire texts are what the app's MessageEdit.between(...).encode() writes for
// each pair (test/helpers/edit_helper_test.dart checks the first one too).
#include <stdio.h>
#include <string.h>

#include "ui-touch/MessageEdit.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b)) != 0) { \
  printf("FAIL %s:%d %s = \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); ++failures; } } while (0)

struct Vec { const char* before; const char* after; const char* wire; };
static const Vec kVecs[] = {
    { "see you at 5 tonight", "see you at 6 tonight", "\xE2""\x9C""\x8E"" \xE2""\x80""\xA6""at 6 tonight\x0A""e:3fa1:62:8:12" },
    { "hello", "hello there", "\xE2""\x9C""\x8E"" hello there\x0A""e:3fa1:1b:0:5" },
    { "meet at the park", "meet at the old park", "\xE2""\x9C""\x8E"" \xE2""\x80""\xA6""the old park\x0A""e:3fa1:ce:8:8" },
    { "ok see you at 5 tonight bye", "ok see you at 6 tonight bye", "\xE2""\x9C""\x8E"" \xE2""\x80""\xA6""at 6 tonight\xE2""\x80""\xA6""\x0A""e:3fa1:f0:11:12" },
    { "h\xC3""\xA9""llo \xF0""\x9F""\x91""\x8B"" w\xC3""\xB6""rld", "h\xC3""\xA9""llo \xF0""\x9F""\x91""\x8B"" world", "\xE2""\x9C""\x8E"" \xE2""\x80""\xA6""\xF0""\x9F""\x91""\x8B"" world\x0A""e:3fa1:c8:6:8" },
    { "line one\x0A""line two", "line one\x0A""line 2", "\xE2""\x9C""\x8E"" \xE2""\x80""\xA6""line 2\x0A""e:3fa1:d7:9:8" },
    { "teh cat", "the cat", "\xE2""\x9C""\x8E"" the cat\x0A""e:3fa1:0c:0:7" },
};

static void testRoundTrip() {
  for (const Vec& v : kVecs) {
    msgedit::Edit e;
    CHECK(msgedit::parse(v.wire, &e));
    CHECK(e.target_hash == 0x3fa1);
    char out[200];
    CHECK(msgedit::applyTo(e, v.before, out, sizeof out));
    CHECK_STR(out, v.after);
    // A repeat over the edited text: recognised, not applied again.
    char again[200];
    CHECK(!msgedit::applyTo(e, v.after, again, sizeof again));
    CHECK(msgedit::alreadyIn(e, v.after));
  }
}

static void testChained() {
  // A second edit applies on top of the first, and refuses the text without it.
  msgedit::Edit e1, e2;
  CHECK(msgedit::parse(kVecs[0].wire, &e1));
  // "see you at 6 tonight" -> "see you at 6 tomorrow"
  char wire2[64];
  snprintf(wire2, sizeof wire2, "%s%s6 tomorrow\ne:3fa1:%02x:11:9", msgedit::kMark, msgedit::kEllipsis,
           (unsigned)msgedit::checkOf("see you at 6 tonight"));
  CHECK(msgedit::parse(wire2, &e2));
  char a[64], b[64];
  CHECK(msgedit::applyTo(e1, "see you at 5 tonight", a, sizeof a));
  CHECK(msgedit::applyTo(e2, a, b, sizeof b));
  CHECK_STR(b, "see you at 6 tomorrow");
  CHECK(!msgedit::applyTo(e2, "see you at 5 tonight", b, sizeof b));
}

static void testNotEdits() {
  const char* bad[] = {
    "hello",
    "\xE2\x9C\x8E just a pencil",
    "\xE2\x9C\x8E two lines\nbut no tag",
    "e:3fa1:7c:1:2",
    "\xE2\x9C\x8E x\ne:3fa1:7c:1",
    "\xE2\x9C\x8E x\ne:XYZW:7c:1:2",
    "\xE2\x9C\x8E x\ne:3fa1:7c:12345:2",
    "\xE2\x9C\x8E x\ne:3fa1:7c:1:2 ",
  };
  for (const char* t : bad) {
    if (msgedit::looks(t)) { printf("FAIL looks(\"%s\")\n", t); ++failures; }
  }
}

static void testBounds() {
  msgedit::Edit e;
  char wire[64];
  snprintf(wire, sizeof wire, "%s%sx\ne:3fa1:%02x:50:3", msgedit::kMark, msgedit::kEllipsis,
           (unsigned)msgedit::checkOf("short"));
  CHECK(msgedit::parse(wire, &e));
  char out[64];
  CHECK(!msgedit::applyTo(e, "short", out, sizeof out));
  CHECK(!msgedit::alreadyIn(e, "short"));
  // Too small an output buffer refuses rather than truncating.
  CHECK(msgedit::parse(kVecs[1].wire, &e));
  CHECK(!msgedit::applyTo(e, "hello", out, 6));
  // An offset inside an emoji's surrogate pair is refused.
  size_t at = 0;
  CHECK(!msgedit::byteOffset("a\xF0\x9F\x91\x8B" "b", 2, &at));
  CHECK(msgedit::byteOffset("a\xF0\x9F\x91\x8B" "b", 3, &at) && at == 5);
  CHECK(msgedit::utf16Len("h\xC3\xA9llo \xF0\x9F\x91\x8B") == 8);
}

static void testWindow() {
  CHECK(msgedit::arrivedInTime(1700000000, 1700000000 + 60));
  CHECK(msgedit::arrivedInTime(1700000000, 1700000000 + 20 * 60));
  CHECK(!msgedit::arrivedInTime(1700000000, 1700000000 + 20 * 60 + 1));
  CHECK(!msgedit::arrivedInTime(1700000000, 1699999999));
  CHECK(msgedit::arrivedInTime(0, 1700000000));
}

int main() {
  testRoundTrip();
  testChained();
  testNotEdits();
  testBounds();
  testWindow();
  if (failures) { printf("%d failure(s)\n", failures); return 1; }
  printf("message edit: all passed\n");
  return 0;
}
