// SPDX-License-Identifier: GPL-3.0-or-later
// Edits to sent messages, as MeshCore Open TAK sends them (lib/helpers/edit_helper.dart).
//
// An edit is an ordinary text message of two lines. The first reads on its own, so
// a client without edits shows a sensible correction; the second says where it goes:
//
//   ✎ …at 6 tonight
//   e:3fa1:7c:8:12
//
//   3fa1  the target message's reaction hash (Reactions.h hashOfMessage: send time,
//         sender in a channel, first five UTF-16 units of the text first sent)
//   7c    low byte of the Dart string hash of the text the edit applies to, so an
//         edit is never applied over a missed earlier one
//   8:12  replace 12 UTF-16 units at UTF-16 offset 8 with the first line's words.
//         A leading "…" (when the offset is past 0) and a trailing one (when text
//         follows the replaced span) mark text left out; they are not inserted.
//
// Header-only and free of Arduino/LVGL so the host test can include it.
// Build the test: g++ -std=c++17 -I src -o /tmp/test_message_edit test/test_message_edit.cpp
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ReactionCodec.h"

namespace msgedit {

static const char kMark[] = "\xE2\x9C\x8E ";        // "✎ "
static const char kEllipsis[] = "\xE2\x80\xA6";     // "…"
static constexpr size_t kMarkLen = sizeof kMark - 1;
static constexpr size_t kEllipsisLen = sizeof kEllipsis - 1;

// How late an edit may come after its message, by the sender's clock (seconds):
// the app's 15-minute edit window plus slack, as messageEditAcceptWindow.
static constexpr uint32_t kAcceptWindowS = 20 * 60;

struct Edit {
  uint16_t target_hash;
  uint8_t base_check;
  uint32_t offset;        // UTF-16 units
  uint32_t delete_len;    // UTF-16 units
  const char* words;      // into the parsed text, not NUL-terminated at words_len
  size_t words_len;       // bytes
};

namespace detail {
inline int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;   // the app writes lowercase only
}

// Reads 1-4 decimal digits from p up to `end`; false on anything else.
inline bool dec(const char* p, const char* end, uint32_t* out) {
  const size_t n = (size_t)(end - p);
  if (n < 1 || n > 4) return false;
  uint32_t v = 0;
  for (; p < end; ++p) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (uint32_t)(*p - '0');
  }
  *out = v;
  return true;
}

// UTF-16 length of one UTF-8 sequence starting with lead byte `b`.
inline int unitsOfLead(uint8_t b) { return b >= 0xF0 ? 2 : 1; }

// Bytes in a UTF-8 sequence starting with lead byte `b`.
inline int bytesOfLead(uint8_t b) {
  if (b < 0x80) return 1;
  if (b >= 0xF0) return 4;
  if (b >= 0xE0) return 3;
  if (b >= 0xC0) return 2;
  return 1;   // a stray continuation byte: step over it alone
}
}  // namespace detail

// UTF-16 units in the first `bytes` bytes of `s` (all of it for SIZE_MAX).
inline size_t utf16Len(const char* s, size_t bytes = (size_t)-1) {
  size_t units = 0, i = 0;
  while (i < bytes && s[i]) {
    const uint8_t b = (uint8_t)s[i];
    units += (size_t)detail::unitsOfLead(b);
    i += (size_t)detail::bytesOfLead(b);
  }
  return units;
}

// Byte offset of UTF-16 offset `units` in `s`. False when it is past the end or
// falls inside a surrogate pair.
inline bool byteOffset(const char* s, size_t units, size_t* out) {
  size_t u = 0, i = 0;
  while (u < units) {
    if (!s[i]) return false;
    const uint8_t b = (uint8_t)s[i];
    u += (size_t)detail::unitsOfLead(b);
    i += (size_t)detail::bytesOfLead(b);
  }
  if (u != units) return false;
  *out = i;
  return true;
}

// The two-hex-digit check of `base` (MessageEdit.checkOf).
inline uint8_t checkOf(const char* base) {
  return (uint8_t)(ReactionCodec::dartStringHash(base) & 0xFF);
}

// Parses an edit's wire text; false when `t` isn't one.
inline bool parse(const char* t, Edit* out) {
  if (!t || strncmp(t, kMark, kMarkLen) != 0) return false;
  const char* nl = strrchr(t, '\n');
  if (!nl || nl < t + kMarkLen) return false;
  const char* tag = nl + 1;
  // e:HHHH:CC:N:N
  if (tag[0] != 'e' || tag[1] != ':' || tag[6] != ':' || tag[9] != ':') return false;
  uint32_t h = 0;
  for (int i = 2; i < 6; ++i) {
    const int v = detail::hexv(tag[i]);
    if (v < 0) return false;
    h = (h << 4) | (uint32_t)v;
  }
  const int c1 = detail::hexv(tag[7]), c2 = detail::hexv(tag[8]);
  if (c1 < 0 || c2 < 0) return false;
  const char* a = tag + 10;
  const char* colon = strchr(a, ':');
  if (!colon) return false;
  const char* b = colon + 1;
  const char* end = b + strlen(b);
  Edit e;
  if (!detail::dec(a, colon, &e.offset) || !detail::dec(b, end, &e.delete_len)) return false;
  e.target_hash = (uint16_t)h;
  e.base_check = (uint8_t)((c1 << 4) | c2);
  e.words = t + kMarkLen;
  e.words_len = (size_t)(nl - e.words);
  if (out) *out = e;
  return true;
}

inline bool looks(const char* t) { return parse(t, nullptr); }

namespace detail {
inline bool startsWithEllipsis(const char* p, size_t n) {
  return n >= kEllipsisLen && memcmp(p, kEllipsis, kEllipsisLen) == 0;
}
inline bool endsWithEllipsis(const char* p, size_t n) {
  return n >= kEllipsisLen && memcmp(p + n - kEllipsisLen, kEllipsis, kEllipsisLen) == 0;
}
}  // namespace detail

// `base` with the edit applied, into out[cap]. False when the edit was made against
// other text (a missed earlier edit), doesn't fit `base`, leaves nothing, or the
// result is longer than cap - 1 bytes.
inline bool applyTo(const Edit& e, const char* base, char* out, size_t cap) {
  if (!base || !out || !cap) return false;
  if (checkOf(base) != e.base_check) return false;
  size_t start = 0, stop = 0;
  if (!byteOffset(base, e.offset, &start)) return false;
  if (!byteOffset(base, (size_t)e.offset + e.delete_len, &stop)) return false;
  const size_t base_len = strlen(base);
  const char* w = e.words;
  size_t wn = e.words_len;
  if (e.offset > 0) {
    if (!detail::startsWithEllipsis(w, wn)) return false;
    w += kEllipsisLen;
    wn -= kEllipsisLen;
  }
  if (stop < base_len) {
    if (!detail::endsWithEllipsis(w, wn)) return false;
    wn -= kEllipsisLen;
  }
  const size_t total = start + wn + (base_len - stop);
  if (total + 1 > cap) return false;
  memmove(out, base, start);   // out may not alias base: callers pass a copy
  memcpy(out + start, w, wn);
  memcpy(out + start + wn, base + stop, base_len - stop);
  out[total] = '\0';
  for (size_t i = 0; i < total; ++i) {
    const char c = out[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return true;
  }
  return false;   // nothing but whitespace left
}

// Whether `current` already reads as this edit made it: a repeat of an applied edit.
inline bool alreadyIn(const Edit& e, const char* current) {
  if (!current) return false;
  const char* w = e.words;
  size_t wn = e.words_len;
  if (e.offset > 0 && detail::startsWithEllipsis(w, wn)) { w += kEllipsisLen; wn -= kEllipsisLen; }
  if (detail::endsWithEllipsis(w, wn)) wn -= kEllipsisLen;
  size_t at = 0;
  if (!byteOffset(current, e.offset, &at)) return false;
  return strlen(current + at) >= wn && memcmp(current + at, w, wn) == 0;
}

// ---- sending: the edit that turns one text into another (MessageEdit.between) ----

// How long after sending our own message can still be edited (seconds), as the
// app's messageEditWindow.
static constexpr uint32_t kEditWindowS = 15 * 60;

namespace detail {
static constexpr size_t kMaxUnits = 512;

// UTF-8 to UTF-16 units, as Dart holds a String. False when it doesn't fit.
inline bool toUnits(const char* s, uint16_t* out, size_t cap, size_t* n) {
  size_t k = 0;
  const char* p = s;
  while (*p) {
    const uint32_t cp = ReactionCodec::nextCodePoint(p);
    if (cp >= 0x10000) {
      if (k + 2 > cap) return false;
      const uint32_t v = cp - 0x10000;
      out[k++] = (uint16_t)(0xD800 + (v >> 10));
      out[k++] = (uint16_t)(0xDC00 + (v & 0x3FF));
    } else {
      if (k + 1 > cap) return false;
      out[k++] = (uint16_t)cp;
    }
  }
  *n = k;
  return true;
}

// UTF-16 units back to UTF-8, appended at out[*len]. False when it doesn't fit.
inline bool appendUtf8(const uint16_t* u, size_t n, char* out, size_t cap, size_t* len) {
  for (size_t i = 0; i < n; ++i) {
    uint32_t cp = u[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
      ++i;
    }
    char b[4];
    size_t m;
    if (cp < 0x80) { b[0] = (char)cp; m = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); m = 2; }
    else if (cp < 0x10000) {
      b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      b[2] = (char)(0x80 | (cp & 0x3F)); m = 3;
    } else {
      b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
      b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); m = 4;
    }
    if (*len + m + 1 > cap) return false;
    memcpy(out + *len, b, m);
    *len += m;
  }
  out[*len] = '\0';
  return true;
}

inline bool isSpace(uint16_t u) { return u == 0x20 || u == 0x0A || u == 0x09 || u == 0x0D; }
}  // namespace detail

// The wire text of the edit turning `before` into `after` for the message whose
// reaction hash is `target_hash`, into out[cap], exactly as the app's
// MessageEdit.between(...).encode(before.length): the changed span widened to
// whole words plus one word each side. False when nothing changed, `after` is
// blank, or it doesn't fit.
inline bool encode(uint16_t target_hash, const char* before, const char* after, char* out, size_t cap) {
  if (!before || !after || !out || cap < 2) return false;
  static uint16_t b[detail::kMaxUnits], a[detail::kMaxUnits];   // one edit at a time, on the UI task
  size_t nb = 0, na = 0;
  if (!detail::toUnits(before, b, detail::kMaxUnits, &nb) || !detail::toUnits(after, a, detail::kMaxUnits, &na))
    return false;
  bool blank = true;
  for (size_t i = 0; i < na && blank; ++i) blank = detail::isSpace(a[i]);
  if (blank) return false;
  if (nb == na && memcmp(a, b, nb * sizeof(uint16_t)) == 0) return false;
  size_t start = 0;
  const size_t max_prefix = nb < na ? nb : na;
  while (start < max_prefix && b[start] == a[start]) ++start;
  size_t suffix = 0;
  while (suffix < nb - start && suffix < na - start && b[nb - 1 - suffix] == a[na - 1 - suffix]) ++suffix;
  // To whole words, then one more word each side (the text around the change is
  // the same in both, so one walk over `before` suits both).
  while (start > 0 && !detail::isSpace(b[start - 1])) --start;
  while (suffix > 0 && !detail::isSpace(b[nb - suffix])) --suffix;
  while (start > 0 && detail::isSpace(b[start - 1])) --start;
  while (start > 0 && !detail::isSpace(b[start - 1])) --start;
  while (suffix > 0 && detail::isSpace(b[nb - suffix])) --suffix;
  while (suffix > 0 && !detail::isSpace(b[nb - suffix])) --suffix;
  const size_t del = nb - suffix - start;
  size_t len = 0;
  if (kMarkLen + 1 > cap) return false;
  memcpy(out, kMark, kMarkLen);
  len = kMarkLen;
  out[len] = '\0';
  auto put = [&](const char* s, size_t n) {
    if (len + n + 1 > cap) return false;
    memcpy(out + len, s, n);
    len += n;
    out[len] = '\0';
    return true;
  };
  if (start > 0 && !put(kEllipsis, kEllipsisLen)) return false;
  if (!detail::appendUtf8(a + start, na - suffix - start, out, cap, &len)) return false;
  if (start + del < nb && !put(kEllipsis, kEllipsisLen)) return false;
  char tag[32];
  const int tn = snprintf(tag, sizeof tag, "\ne:%04x:%02x:%u:%u", (unsigned)target_hash,
                          (unsigned)checkOf(before), (unsigned)start, (unsigned)del);
  if (tn <= 0 || (size_t)tn >= sizeof tag) return false;
  if ((unsigned)start > 9999 || (unsigned)del > 9999) return false;
  return put(tag, (size_t)tn);
}

// By the sender's clock, the edit (sent at edit_ts) is within the accept window of
// its message (sent at msg_ts). Unknown times (0) don't count against it.
inline bool arrivedInTime(uint32_t msg_ts, uint32_t edit_ts) {
  if (!msg_ts || !edit_ts) return true;
  return edit_ts >= msg_ts && edit_ts - msg_ts <= kAcceptWindowS;
}

}  // namespace msgedit
