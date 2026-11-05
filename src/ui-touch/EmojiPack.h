// SPDX-License-Identifier: GPL-3.0-or-later
// The full emoji set as a file (emoji/emoji-pack.wep, scripts/build/make-emoji-pack.py):
// every single-codepoint Noto emoji, for those the firmware doesn't carry in flash.
// This reads it from memory: the index, a binary search, and unpacking one image
// (the same alpha-plane + visible-RGB565 LZ the baked emoji use, emoji_data.c).
// Loading, saving and downloading it are UITask's (EMOJI.md).
//
// Header-only and free of LVGL, so test/test_emoji_pack_file.cpp checks it on the host
// against the committed file.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace emojipack {

static constexpr size_t kHeader = 16;
static constexpr size_t kEntry = 16;
static constexpr size_t kMaxBytes = 2u * 1024u * 1024u;   // a download larger than this isn't the pack

struct View {
  const uint8_t* buf = nullptr;
  size_t len = 0;
  uint16_t count = 0;
  uint32_t data = 0;   // the images' offset in buf
  uint32_t names = 0;  // the names section's offset in buf
};

struct Entry {
  uint32_t cp;
  uint32_t off;        // from View::data
  uint16_t zlen, alen;
  uint8_t w, h;        // 0 x 0: drawn as nothing
};

namespace detail {
inline uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
// One LZ plane (pack-emoji.py's stream). False unless it fills `n` exactly.
inline bool unlz(const uint8_t* in, size_t inlen, uint8_t* out, size_t n) {
  size_t i = 0, o = 0;
  while (i < inlen) {
    const unsigned t = in[i++];
    if (t < 0x80) {
      const size_t k = t + 1;
      if (i + k > inlen || o + k > n) return false;
      memcpy(out + o, in + i, k);
      i += k;
      o += k;
    } else {
      if (i + 2 > inlen) return false;
      const size_t k = (t & 0x7F) + 3;
      const size_t off = (size_t)(in[i] | (in[i + 1] << 8)) + 1;
      i += 2;
      if (off > o || o + k > n) return false;
      for (size_t j = 0; j < k; ++j, ++o) out[o] = out[o - off];
    }
  }
  return o == n;
}
}  // namespace detail

inline Entry entryAt(const View& v, int i) {
  const uint8_t* p = v.buf + kHeader + (size_t)i * kEntry;
  Entry e;
  e.cp = detail::u32(p);
  e.off = detail::u32(p + 4);
  e.zlen = detail::u16(p + 8);
  e.alen = detail::u16(p + 10);
  e.w = p[12];
  e.h = p[13];
  return e;
}

// Checks the whole file before anything uses it: the header, every entry's image
// inside the file and at most 16 x 16, the codepoints strictly ascending.
inline bool parse(const uint8_t* buf, size_t len, View* out) {
  if (!buf || !out || len < kHeader || len > kMaxBytes || memcmp(buf, "WEP1", 4) != 0) return false;
  if (detail::u16(buf + 4) != 2) return false;
  View v;
  v.buf = buf;
  v.len = len;
  v.count = detail::u16(buf + 6);
  v.data = detail::u32(buf + 8);
  v.names = detail::u32(buf + 12);
  if (v.count == 0 || v.data != kHeader + (size_t)v.count * kEntry || v.data > len) return false;
  if (v.names < v.data || (uint64_t)v.names + 4u * v.count > len) return false;
  uint32_t prev = 0;
  for (int i = 0; i < v.count; ++i) {
    const Entry e = entryAt(v, i);
    if (i > 0 && e.cp <= prev) return false;
    prev = e.cp;
    if (e.w == 0 || e.h == 0) {
      if (e.w || e.h || e.zlen) return false;
      continue;
    }
    if (e.w > 16 || e.h > 16 || e.alen > e.zlen) return false;
    if ((uint64_t)v.data + e.off + e.zlen > v.names) return false;
  }
  // Every name: inside the file, two NUL-terminated strings.
  for (int i = 0; i < v.count; ++i) {
    const uint32_t o = detail::u32(buf + v.names + 4u * i);
    if (o == 0xFFFFFFFFu) continue;
    size_t p = (size_t)v.names + o;
    for (int part = 0; part < 2; ++part) {
      while (p < len && buf[p]) ++p;
      if (p >= len) return false;
      ++p;
    }
  }
  *out = v;
  return true;
}

// The entry for `cp`, or -1.
inline int find(const View& v, uint32_t cp) {
  int lo = 0, hi = (int)v.count - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) >> 1;
    const uint32_t c = detail::u32(v.buf + kHeader + (size_t)mid * kEntry);
    if (c == cp) return mid;
    if (cp < c) hi = mid - 1;
    else lo = mid + 1;
  }
  return -1;
}

// Image i into `px` (w*h*3: RGB565 low, high, alpha), with `tmp` (w*h*3) for the planes.
inline bool unpack(const View& v, int i, uint8_t* px, uint8_t* tmp) {
  if (i < 0 || i >= v.count) return false;
  const Entry e = entryAt(v, i);
  const size_t n = (size_t)e.w * e.h;
  if (!n) return false;
  const uint8_t* z = v.buf + v.data + e.off;
  uint8_t* alpha = tmp;
  uint8_t* rgb = tmp + n;
  if (!detail::unlz(z, e.alen, alpha, n)) return false;
  size_t vis = 0;
  for (size_t k = 0; k < n; ++k) vis += alpha[k] != 0;
  if (!detail::unlz(z + e.alen, (size_t)e.zlen - e.alen, rgb, 2 * vis)) return false;
  for (size_t k = 0, r = 0; k < n; ++k) {
    if (alpha[k]) {
      px[3 * k] = rgb[r];
      px[3 * k + 1] = rgb[r + 1];
      r += 2;
    } else {
      px[3 * k] = 0;
      px[3 * k + 1] = 0;
    }
    px[3 * k + 2] = alpha[k];
  }
  return true;
}

// ---- names and search -------------------------------------------------------------
// Entry i's text to insert (its emoji, as phones send it) and its search text
// ("tropical fish|animal fish fishes tropical"), or nullptr when it has no name.
inline const char* utf8At(const View& v, int i) {
  if (i < 0 || i >= v.count) return nullptr;
  const uint32_t o = detail::u32(v.buf + v.names + 4u * (uint32_t)i);
  return o == 0xFFFFFFFFu ? nullptr : (const char*)v.buf + v.names + o;
}
inline const char* textAt(const View& v, int i) {
  const char* u = utf8At(v, i);
  return u ? u + strlen(u) + 1 : nullptr;
}

namespace detail {
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
inline bool wordStart(const char* text, const char* p) {
  if (p == text) return true;
  const char c = p[-1];
  return c == ' ' || c == '|' || c == ':' || c == '-' || c == '_' || c == '(' || c == '"';
}
// `word` (n chars, lowercase) starts a word in `from..to`.
inline bool hasWordPrefix(const char* text, const char* from, const char* to, const char* word, size_t n) {
  for (const char* p = from; p + n <= to; ++p)
    if (wordStart(text, p) && strncmp(p, word, n) == 0) return true;
  return false;
}
}  // namespace detail

// The entries `query` finds, best first, at most `max`; returns how many. Every word
// of the query must start a word of the emoji's name or keywords ("fi tro" finds
// 🐠). The name that is the query comes first, then names starting with it, then
// names with every word, then keyword-only matches; each in codepoint order.
inline int search(const View& v, const char* query, int* out, int max) {
  if (!query || !out || max <= 0 || !v.names) return 0;
  char q[48];
  size_t qn = 0;
  for (const char* p = query; *p && qn + 1 < sizeof q; ++p) q[qn++] = detail::lower(*p);
  while (qn && q[qn - 1] == ' ') --qn;
  size_t qs = 0;
  while (qs < qn && q[qs] == ' ') ++qs;
  q[qn] = 0;
  if (qs >= qn) return 0;
  const char* qq = q + qs;
  const size_t qlen = qn - qs;
  int n = 0;
  for (int tier = 0; tier < 4 && n < max; ++tier) {
    for (int i = 0; i < v.count && n < max; ++i) {
      const char* t = textAt(v, i);
      if (!t) continue;
      const char* bar = strchr(t, '|');
      const char* name_end = bar ? bar : t + strlen(t);
      const char* end = t + strlen(t);
      bool all_name = true, all_any = true;
      for (const char* w = qq; *w;) {
        while (*w == ' ') ++w;
        if (!*w) break;
        const char* we = w;
        while (*we && *we != ' ') ++we;
        const size_t wl = (size_t)(we - w);
        if (!detail::hasWordPrefix(t, t, name_end, w, wl)) all_name = false;
        if (!detail::hasWordPrefix(t, t, end, w, wl)) { all_any = false; break; }
        w = we;
      }
      if (!all_any) continue;
      const size_t nl = (size_t)(name_end - t);
      const bool starts = nl >= qlen && strncmp(t, qq, qlen) == 0;
      const int my = (starts && nl == qlen) ? 0 : starts ? 1 : all_name ? 2 : 3;
      if (my == tier) out[n++] = i;
    }
  }
  return n;
}

}  // namespace emojipack
