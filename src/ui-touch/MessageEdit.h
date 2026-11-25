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

// By the sender's clock, the edit (sent at edit_ts) is within the accept window of
// its message (sent at msg_ts). Unknown times (0) don't count against it.
inline bool arrivedInTime(uint32_t msg_ts, uint32_t edit_ts) {
  if (!msg_ts || !edit_ts) return true;
  return edit_ts >= msg_ts && edit_ts - msg_ts <= kAcceptWindowS;
}

}  // namespace msgedit
