// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GIFs as MeshCore Open (and Open TAK) send them: a message that is only a Giphy
// GIF's id, "g:<id>", or one of its links. The app shows the GIF itself; so does a
// standalone with Wi-Fi (LinkImages.inc), and a preview says "GIF".
//
// The forms (MeshCore Open lib/helpers/gif_helper.dart, MIT):
//   g:<id>
//   [http[s]://]media.giphy.com/media/<id>/giphy.gif
//   [http[s]://]giphy.com/gifs/[<title>-]<id>[/]
// Pure, so the host test runs it (test/test_giphy_text.cpp).

#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace giphy {

constexpr size_t kIdMax = 40;   // ids are about 13 to 20 characters

namespace detail {
inline bool idChar(char c, bool dash) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (dash && c == '-');
}
inline const char* skipScheme(const char* s) {
  if (!strncmp(s, "https://", 8)) return s + 8;
  if (!strncmp(s, "http://", 7)) return s + 7;
  return s;
}
// [a, b) as the id when every character is one; false otherwise.
inline bool take(const char* a, const char* b, bool dash, char* out, size_t cap) {
  const size_t n = (size_t)(b - a);
  if (!n || n >= cap || n >= kIdMax) return false;
  for (const char* p = a; p < b; ++p) if (!idChar(*p, dash)) return false;
  memcpy(out, a, n);
  out[n] = 0;
  return true;
}
}  // namespace detail

// The GIF a whole message is (blanks around it allowed); false for anything else.
inline bool idOf(const char* text, char* out, size_t cap) {
  if (!text || !out || !cap) return false;
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') ++text;
  const char* end = text + strlen(text);
  while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) --end;
  if (end - text > 2 && text[0] == 'g' && text[1] == ':') return detail::take(text + 2, end, true, out, cap);
  const char* s = detail::skipScheme(text);
  static const char kMedia[] = "media.giphy.com/media/";
  static const char kTail[] = "/giphy.gif";
  const size_t ml = sizeof kMedia - 1, tl = sizeof kTail - 1;
  if (!strncmp(s, kMedia, ml) && (size_t)(end - s) > ml + tl && !strncmp(end - tl, kTail, tl))
    return detail::take(s + ml, end - tl, true, out, cap);
  static const char kPage[] = "giphy.com/gifs/";
  const size_t pl = sizeof kPage - 1;
  if (!strncmp(s, kPage, pl)) {
    const char* a = s + pl;
    const char* b = end;
    if (b > a && b[-1] == '/') --b;
    // The id is after the last dash (titles are dashed; these ids aren't).
    const char* d = b;
    while (d > a && d[-1] != '-') --d;
    for (const char* p = a; p < b; ++p) if (*p == '/' || *p == '?') return false;
    return detail::take(d, b, false, out, cap);
  }
  return false;
}

// https://media.giphy.com/media/<id>/<rendition>.gif. Renditions used here:
// "200w_s" a still 200 px wide (the bubble), "200w" that animated (the player),
// "100w_s" a still 100 px wide (the picker's grid).
inline bool mediaUrl(const char* id, const char* rendition, char* out, size_t cap) {
  if (!id || !*id || !rendition || !out) return false;
  const int n = snprintf(out, cap, "https://media.giphy.com/media/%s/%s.gif", id, rendition);
  return n > 0 && (size_t)n < cap;
}

// The id in a media.giphy.com link (any rendition); false for any other link.
inline bool idOfMediaUrl(const char* url, char* out, size_t cap) {
  if (!url || !out) return false;
  const char* s = detail::skipScheme(url);
  static const char kMedia[] = "media.giphy.com/media/";
  const size_t ml = sizeof kMedia - 1;
  if (strncmp(s, kMedia, ml)) return false;
  const char* a = s + ml;
  const char* b = strchr(a, '/');
  return b && detail::take(a, b, true, out, cap);
}

// The message that sends a GIF: "g:<id>".
inline bool message(const char* id, char* out, size_t cap) {
  if (!id || !*id || !out) return false;
  const int n = snprintf(out, cap, "g:%s", id);
  return n > 0 && (size_t)n < cap;
}

// The GIFs' ids in a Giphy API answer (/v1/gifs/search, /v1/gifs/trending), in
// order: each result opens "type":"gif","id":"<id>" (other objects in it have ids
// of their own, so the pair is what's looked for). Returns how many, at most max.
inline int idsInJson(const char* json, size_t len, char (*ids)[kIdMax], int max) {
  if (!json || !ids || max <= 0) return 0;
  static const char kKey[] = "\"type\":\"gif\",\"id\":\"";
  const size_t kl = sizeof kKey - 1;
  int n = 0;
  size_t i = 0;
  while (n < max && i + kl < len) {
    const char* hit = nullptr;
    for (size_t k = i; k + kl <= len; ++k)
      if (json[k] == '"' && !memcmp(json + k, kKey, kl)) { hit = json + k; break; }
    if (!hit) break;
    const char* a = hit + kl;
    const char* lim = json + len;
    const char* b = a;
    while (b < lim && *b != '"') ++b;
    if (b >= lim) break;
    if (detail::take(a, b, true, ids[n], kIdMax)) ++n;
    i = (size_t)(b - json) + 1;
  }
  return n;
}

// `q` made safe for a URL's query (letters, digits and -_.~ kept, blanks as +,
// the rest %XX). False when it doesn't fit.
inline bool queryEscape(const char* q, char* out, size_t cap) {
  if (!out || !cap) return false;
  size_t o = 0;
  for (const unsigned char* p = (const unsigned char*)(q ? q : ""); *p; ++p) {
    const unsigned char c = *p;
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                      c == '_' || c == '.' || c == '~';
    if (keep || c == ' ') {
      if (o + 1 >= cap) return false;
      out[o++] = c == ' ' ? '+' : (char)c;
    } else {
      if (o + 3 >= cap) return false;
      static const char kHex[] = "0123456789ABCDEF";
      out[o++] = '%';
      out[o++] = kHex[c >> 4];
      out[o++] = kHex[c & 15];
    }
  }
  out[o] = 0;
  return true;
}

}  // namespace giphy
