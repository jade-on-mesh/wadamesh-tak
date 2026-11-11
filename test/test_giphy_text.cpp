// SPDX-License-Identifier: GPL-3.0-or-later
//
// GiphyText.h against MeshCore Open's GifHelper (lib/helpers/gif_helper.dart):
// the same messages are GIFs, with the same ids.
// g++ -std=c++17 -I src test/test_giphy_text.cpp -o /tmp/t && /tmp/t

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/GiphyText.h"

using namespace giphy;

static bool is(const char* text, const char* want) {
  char id[kIdMax];
  if (!idOf(text, id, sizeof id)) return want == nullptr;
  return want && !strcmp(id, want);
}

int main() {
  // The app's own form, and the links it understands.
  assert(is("g:3o7TKSjRrfIPjeiVyM", "3o7TKSjRrfIPjeiVyM"));
  assert(is("  g:abc_DEF-12 \n", "abc_DEF-12"));
  assert(is("https://media.giphy.com/media/l0MYt5jPR6QX5pnqM/giphy.gif", "l0MYt5jPR6QX5pnqM"));
  assert(is("http://media.giphy.com/media/l0MYt5jPR6QX5pnqM/giphy.gif", "l0MYt5jPR6QX5pnqM"));
  assert(is("media.giphy.com/media/l0MYt5jPR6QX5pnqM/giphy.gif", "l0MYt5jPR6QX5pnqM"));
  assert(is("https://giphy.com/gifs/cat-funny-JIX9t2j0ZTN9S", "JIX9t2j0ZTN9S"));
  assert(is("https://giphy.com/gifs/JIX9t2j0ZTN9S/", "JIX9t2j0ZTN9S"));
  assert(is("giphy.com/gifs/a-b-c-XYZ123", "XYZ123"));
  // Not GIFs.
  assert(is("g:", nullptr));
  assert(is("g:has space", nullptr));
  assert(is("look g:abc", nullptr));
  assert(is("g:abc and more", nullptr));
  assert(is("https://media.giphy.com/media/abc/200w.gif", nullptr));   // the app takes giphy.gif only
  assert(is("https://giphy.com/gifs/cat/funny-abc", nullptr));
  assert(is("https://example.com/cat.gif", nullptr));
  assert(is("hello", nullptr));
  assert(is("", nullptr));
  assert(is(nullptr, nullptr));

  char url[96], id[kIdMax];
  assert(mediaUrl("abc", "200w_s", url, sizeof url));
  assert(!strcmp(url, "https://media.giphy.com/media/abc/200w_s.gif"));
  assert(idOfMediaUrl(url, id, sizeof id) && !strcmp(id, "abc"));
  assert(!idOfMediaUrl("https://example.com/media/abc/x.gif", id, sizeof id));
  assert(!mediaUrl("abc", "200w", url, 20));   // doesn't fit
  assert(message("abc", url, sizeof url) && !strcmp(url, "g:abc") && is(url, "abc"));

  // The API's answer: each result's own id, not its user's or its images'.
  const char* json =
      "{\"data\":[{\"type\":\"gif\",\"id\":\"AAA111\",\"url\":\"x\",\"user\":{\"id\":\"nope\"},"
      "\"images\":{\"id\":\"no\"}},{\"type\":\"gif\",\"id\":\"BBB-222\",\"x\":1},"
      "{\"type\":\"gif\",\"id\":\"CCC333\"}],\"pagination\":{}}";
  char ids[4][kIdMax];
  assert(idsInJson(json, strlen(json), ids, 4) == 3);
  assert(!strcmp(ids[0], "AAA111") && !strcmp(ids[1], "BBB-222") && !strcmp(ids[2], "CCC333"));
  assert(idsInJson(json, strlen(json), ids, 2) == 2);
  assert(idsInJson("{\"data\":[]}", 11, ids, 4) == 0);
  // Cut off mid-id: that one is left out.
  const char* cut = "[{\"type\":\"gif\",\"id\":\"AAA111\"},{\"type\":\"gif\",\"id\":\"BB";
  assert(idsInJson(cut, strlen(cut), ids, 4) == 1);

  char q[32];
  assert(queryEscape("happy cat", q, sizeof q) && !strcmp(q, "happy+cat"));
  assert(queryEscape("a&b=c", q, sizeof q) && !strcmp(q, "a%26b%3Dc"));
  assert(queryEscape("\xC3\xA9", q, sizeof q) && !strcmp(q, "%C3%A9"));
  assert(!queryEscape("aaaaaaaaaaaaaaaa", q, 8));

  printf("test_giphy_text: ok\n");
  return 0;
}
