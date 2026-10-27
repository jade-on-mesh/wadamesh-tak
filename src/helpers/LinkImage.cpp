// SPDX-License-Identifier: GPL-3.0-or-later
// Pictures linked in a message (LinkImage.h).

#include "LinkImage.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

// Working memory for the decoders: PSRAM on the radios (a picture's copy is
// megabytes), plain malloc on a computer.
static void* liWorkAlloc(size_t n) {
#if defined(ESP32)
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
#else
  return malloc(n);
#endif
}
static void* liWorkRealloc(void* p, size_t n) {
#if defined(ESP32)
  void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return q ? q : realloc(p, n);
#else
  return realloc(p, n);
#endif
}
static void liWorkFree(void* p) { free(p); }   // heap_caps memory frees with free()

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS 8192
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(sz) liWorkAlloc(sz)
#define STBI_REALLOC(p, sz) liWorkRealloc(p, sz)
#define STBI_FREE(p) liWorkFree(p)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "../../lib/stb_image/stb_image.h"

#define SIMPLEWEBP_IMPLEMENTATION
#define SIMPLEWEBP_DISABLE_STDIO
#include "../../lib/simplewebp/simplewebp.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace linkimg {
namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool ciPrefix(const char* s, const char* p) {
  for (; *p; s++, p++)
    if (lower(*s) != *p) return false;
  return true;
}

// The path of [a, b) ends in a picture's extension (before any ? or #).
bool pictureExt(const char* a, const char* b) {
  const char* end = a;
  while (end < b && *end != '?' && *end != '#') end++;
  static const char* const kExt[] = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp"};
  for (const char* e : kExt) {
    const size_t n = strlen(e);
    if ((size_t)(end - a) <= n) continue;
    bool same = true;
    for (size_t i = 0; i < n && same; i++) same = lower(end[-(long)n + (long)i]) == e[i];
    if (same) return true;
  }
  return false;
}

simplewebp_allocator webpAllocator() {
  simplewebp_allocator a;
  a.alloc = [](void*, size_t n) -> void* { return liWorkAlloc(n); };
  a.free = [](void*, void* p) { liWorkFree(p); };
  a.userdata = nullptr;
  return a;
}

// Bytes of working memory per pixel, at most, by kind (the decoded RGBA copy,
// plus what the decoder keeps: a JPEG's component planes and a progressive
// one's coefficients, a PNG's unfiltered rows, a GIF's two frames and
// history, a WebP's planes).
size_t perPixel(Kind k) {
  switch (k) {
    case kJpeg: return 10;
    case kPng: return 9;
    case kGif: return 13;
    case kWebp: return 9;
    case kBmp: return 5;
    default: return 0;
  }
}

}  // namespace

bool findUrl(const char* s, char* out, size_t cap) {
  if (!s || !out || cap == 0) return false;
  for (size_t i = 0; s[i]; i++) {
    size_t scheme;
    if (ciPrefix(s + i, "https://")) scheme = 8;
    else if (ciPrefix(s + i, "http://")) scheme = 7;
    else continue;
    size_t j = i + scheme;
    while (s[j] && (unsigned char)s[j] > ' ' && s[j] != '<' && s[j] != '>' && s[j] != '"') j++;
    while (j > i + scheme) {   // trailing punctuation belongs to the sentence
      const char c = s[j - 1];
      if (c == '.' || c == ',' || c == ')' || c == ';' || c == '!' || c == '?' || c == '\'' || c == ':' || c == ']')
        j--;
      else break;
    }
    const char* host = s + i + scheme;
    const char* slash = (const char*)memchr(host, '/', j - (i + scheme));
    if (!slash || !memchr(host, '.', (size_t)(slash - host))) { i = j ? j - 1 : i; continue; }
    if (!pictureExt(slash, s + j)) { i = j - 1; continue; }
    const size_t n = j - i;
    if (n + 1 > cap) return false;
    memcpy(out, s + i, n);
    out[n] = 0;
    return true;
  }
  return false;
}

bool fetchUrl(const char* url, int max_side, char* out, size_t cap) {
  if (!url || !out || cap == 0) return false;
  const char* host = strstr(url, "://");
  host = host ? host + 3 : url;
  const char* q = strchr(url, '?');
  char extra[96] = "";
  if (ciPrefix(host, "images.pexels.com/")) {
    if (!q || (!strstr(q, "w=") && !strstr(q, "h=")))
      snprintf(extra, sizeof extra, "auto=compress&cs=tinysrgb&w=%d", max_side);
  } else if (ciPrefix(host, "images.unsplash.com/")) {
    if (!q || !strstr(q, "w="))
      snprintf(extra, sizeof extra, "w=%d&q=75&fm=jpg", max_side);
  }
  const char* hash = strchr(url, '#');
  const size_t base = hash ? (size_t)(hash - url) : strlen(url);
  const size_t ex = strlen(extra);
  const size_t need = base + (ex ? ex + 1 : 0) + 1;
  if (need > cap) return false;
  memcpy(out, url, base);
  size_t n = base;
  if (ex) {
    out[n++] = q && q < url + base ? '&' : '?';
    memcpy(out + n, extra, ex);
    n += ex;
  }
  out[n] = 0;
  return true;
}

uint32_t urlId(const char* url) {
  uint32_t h = 2166136261u;
  for (const char* p = url; p && *p; p++) {
    h ^= (uint8_t)*p;
    h *= 16777619u;
  }
  return h ? h : 1;
}

Kind sniff(const uint8_t* d, size_t n) {
  if (!d) return kUnknown;
  if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return kJpeg;
  if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return kPng;
  if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) return kGif;
  if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return kWebp;
  if (n >= 2 && d[0] == 'B' && d[1] == 'M') return kBmp;
  return kUnknown;
}

const char* kindName(Kind k) {
  switch (k) {
    case kJpeg: return "JPEG";
    case kPng: return "PNG";
    case kGif: return "GIF";
    case kWebp: return "WebP";
    case kBmp: return "BMP";
    default: return "not a picture";
  }
}

const char* resultText(Result r) {
  switch (r) {
    case kOk: return "OK";
    case kNotPicture: return "Not a picture this can show";
    case kTooLarge: return "Picture too large";
    case kBroken: return "Couldn't read the picture";
    case kNoMemory: return "Out of memory";
  }
  return "?";
}

bool size(const uint8_t* d, size_t n, int* w, int* h) {
  const Kind k = sniff(d, n);
  if (k == kUnknown || n > 0x7FFFFFFF) return false;
  int x = 0, y = 0;
  if (k == kWebp) {
    const simplewebp_allocator a = webpAllocator();
    simplewebp* sw = nullptr;
    if (simplewebp_load_from_memory((void*)d, n, &a, &sw) != SIMPLEWEBP_NO_ERROR || !sw) return false;
    size_t sx = 0, sy = 0;
    simplewebp_get_dimensions(sw, &sx, &sy);
    simplewebp_unload(sw);
    x = (int)sx;
    y = (int)sy;
  } else {
    int c = 0;
    if (!stbi_info_from_memory(d, (int)n, &x, &y, &c)) return false;
  }
  if (x <= 0 || y <= 0) return false;
  if (w) *w = x;
  if (h) *h = y;
  return true;
}

size_t workBytes(const uint8_t* d, size_t n) {
  int w = 0, h = 0;
  if (!size(d, n, &w, &h)) return 0;
  return (size_t)w * (size_t)h * perPixel(sniff(d, n)) + n;
}

Result decode(const uint8_t* d, size_t n, int max_side, size_t budget, uint16_t bg,
              void* (*out_alloc)(size_t), uint16_t** out, int* out_w, int* out_h) {
  if (!out || !out_w || !out_h || !out_alloc || max_side < 1) return kBroken;
  *out = nullptr;
  const Kind k = sniff(d, n);
  if (k == kUnknown) return kNotPicture;
  int sw = 0, sh = 0;
  if (!size(d, n, &sw, &sh)) return kBroken;
  if ((size_t)sw * (size_t)sh * perPixel(k) + n > budget) return kTooLarge;

  // The picture as RGBA, full size.
  uint8_t* rgba = nullptr;
  bool from_stb = false;
  if (k == kWebp) {
    const simplewebp_allocator a = webpAllocator();
    simplewebp* wp = nullptr;
    if (simplewebp_load_from_memory((void*)d, n, &a, &wp) != SIMPLEWEBP_NO_ERROR || !wp) return kBroken;
    rgba = (uint8_t*)liWorkAlloc((size_t)sw * sh * 4);
    if (!rgba) { simplewebp_unload(wp); return kNoMemory; }
    const simplewebp_error e = simplewebp_decode(wp, rgba, nullptr);
    simplewebp_unload(wp);
    if (e != SIMPLEWEBP_NO_ERROR) { liWorkFree(rgba); return e == SIMPLEWEBP_ALLOC_ERROR ? kNoMemory : kBroken; }
  } else {
    int x = 0, y = 0, c = 0;
    rgba = stbi_load_from_memory(d, (int)n, &x, &y, &c, 4);
    if (!rgba) return kBroken;
    if (x != sw || y != sh) { stbi_image_free(rgba); return kBroken; }
    from_stb = true;
  }

  // Scaled to fit max_side (never up), each pixel the average of the ones it
  // covers, see-through parts over bg.
  int dw = sw, dh = sh;
  if (sw > max_side || sh > max_side) {
    if (sw >= sh) { dw = max_side; dh = (int)(((long long)sh * max_side + sw / 2) / sw); }
    else          { dh = max_side; dw = (int)(((long long)sw * max_side + sh / 2) / sh); }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
  }
  uint16_t* px = (uint16_t*)out_alloc((size_t)dw * dh * 2);
  if (!px) {
    if (from_stb) stbi_image_free(rgba); else liWorkFree(rgba);
    return kNoMemory;
  }
  const int bg_r = ((bg >> 11) & 31) * 255 / 31, bg_g = ((bg >> 5) & 63) * 255 / 63, bg_b = (bg & 31) * 255 / 31;
  for (int y = 0; y < dh; y++) {
    const int y0 = (int)((long long)y * sh / dh);
    int y1 = (int)((long long)(y + 1) * sh / dh);
    if (y1 <= y0) y1 = y0 + 1;
    for (int x = 0; x < dw; x++) {
      const int x0 = (int)((long long)x * sw / dw);
      int x1 = (int)((long long)(x + 1) * sw / dw);
      if (x1 <= x0) x1 = x0 + 1;
      uint32_t r = 0, g = 0, b = 0, cnt = 0;
      for (int yy = y0; yy < y1; yy++) {
        const uint8_t* p = rgba + ((size_t)yy * sw + x0) * 4;
        for (int xx = x0; xx < x1; xx++, p += 4) {
          const uint32_t a = p[3];
          r += (p[0] * a + bg_r * (255 - a)) / 255;
          g += (p[1] * a + bg_g * (255 - a)) / 255;
          b += (p[2] * a + bg_b * (255 - a)) / 255;
          cnt++;
        }
      }
      r /= cnt; g /= cnt; b /= cnt;
      px[(size_t)y * dw + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
  if (from_stb) stbi_image_free(rgba); else liWorkFree(rgba);
  *out = px;
  *out_w = dw;
  *out_h = dh;
  return kOk;
}

}  // namespace linkimg
