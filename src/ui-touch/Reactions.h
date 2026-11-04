// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Message reactions, interoperable with MeshCore Open (and so MeshCore Open TAK).
//
// A reaction is a message of its own in the same channel or DM:
//
//   r:HASH:II     the app's format: HASH (4 hex) names the message reacted to,
//                 II (2 hex) is the emoji's place in kEmojis below.
//
// HASH is ReactionHelper.computeReactionHash: the Dart VM's String.hashCode over
// the message's send time (decimal seconds), its sender's name (channels and
// rooms; none in a 1:1 chat) and the first five UTF-16 units of its text, low
// 16 bits. That's QuickReply.h's DartHash, which watched-channel replies use too.
//
// MeshCore One's format is read as well, as the app does (its docs/Reactions.md):
//
//   @[name]EMOJI\nhhhhhhhh   (or EMOJI@[name]\n..., its older order; no @[name]
//                            in a DM). hhhhhhhh is the first 5 bytes of
//                            SHA-256(text UTF-8 + send time, uint32 LE) in
//                            lowercase Crockford base32.
//
// Only the app's format is sent. A MeshCore One reaction that matches a message
// here is stored in the app's format, so the rest of the firmware sees one kind.
//
// Pure logic, header-only, host-tested against the app's golden values in
// test/test_reactions.cpp. The emoji table is generated from the app's
// lib/widgets/emoji_picker.dart (quickEmojis, smileys, gestures, hearts,
// objects, in that order): its order IS the wire format, so never reorder it.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "QuickReply.h"

namespace reactions {

// ReactionHelper.reactionEmojis. A few appear twice (👍 ❤️ 😂 🎉 👏 🔥 are both
// the quick row and in their groups); an emoji is sent as its FIRST index, and
// any index is read.
static const char* const kEmojis[] = {
    "\xf0\x9f\x91\x8d",  // 00 👍
    "\xe2\x9d\xa4\xef\xb8\x8f",  // 01 ❤️
    "\xf0\x9f\x98\x82",  // 02 😂
    "\xf0\x9f\x8e\x89",  // 03 🎉
    "\xf0\x9f\x91\x8f",  // 04 👏
    "\xf0\x9f\x94\xa5",  // 05 🔥
    "\xf0\x9f\x98\x80",  // 06 😀
    "\xf0\x9f\x98\x83",  // 07 😃
    "\xf0\x9f\x98\x84",  // 08 😄
    "\xf0\x9f\x98\x81",  // 09 😁
    "\xf0\x9f\x98\x85",  // 0a 😅
    "\xf0\x9f\x98\x82",  // 0b 😂
    "\xf0\x9f\xa4\xa3",  // 0c 🤣
    "\xf0\x9f\x98\x8a",  // 0d 😊
    "\xf0\x9f\x98\x87",  // 0e 😇
    "\xf0\x9f\x99\x82",  // 0f 🙂
    "\xf0\x9f\x99\x83",  // 10 🙃
    "\xf0\x9f\x98\x89",  // 11 😉
    "\xf0\x9f\x98\x8c",  // 12 😌
    "\xf0\x9f\x98\x8d",  // 13 😍
    "\xf0\x9f\xa5\xb0",  // 14 🥰
    "\xf0\x9f\x98\x98",  // 15 😘
    "\xf0\x9f\x98\x97",  // 16 😗
    "\xf0\x9f\x98\x99",  // 17 😙
    "\xf0\x9f\x98\x9a",  // 18 😚
    "\xf0\x9f\x98\x8b",  // 19 😋
    "\xf0\x9f\x98\x9b",  // 1a 😛
    "\xf0\x9f\x98\x9d",  // 1b 😝
    "\xf0\x9f\x98\x9c",  // 1c 😜
    "\xf0\x9f\xa4\xaa",  // 1d 🤪
    "\xf0\x9f\xa4\xa8",  // 1e 🤨
    "\xf0\x9f\xa7\x90",  // 1f 🧐
    "\xf0\x9f\xa4\x93",  // 20 🤓
    "\xf0\x9f\x98\x8e",  // 21 😎
    "\xf0\x9f\xa5\xb8",  // 22 🥸
    "\xf0\x9f\xa4\xa9",  // 23 🤩
    "\xf0\x9f\xa5\xb3",  // 24 🥳
    "\xf0\x9f\x98\x8f",  // 25 😏
    "\xf0\x9f\x98\x92",  // 26 😒
    "\xf0\x9f\x98\x9e",  // 27 😞
    "\xf0\x9f\x98\x94",  // 28 😔
    "\xf0\x9f\x98\x9f",  // 29 😟
    "\xf0\x9f\x98\x95",  // 2a 😕
    "\xf0\x9f\x99\x81",  // 2b 🙁
    "\xf0\x9f\x98\xa3",  // 2c 😣
    "\xf0\x9f\x98\x96",  // 2d 😖
    "\xf0\x9f\x98\xab",  // 2e 😫
    "\xf0\x9f\x98\xa9",  // 2f 😩
    "\xf0\x9f\xa5\xba",  // 30 🥺
    "\xf0\x9f\x98\xa2",  // 31 😢
    "\xf0\x9f\x98\xad",  // 32 😭
    "\xf0\x9f\x98\xa4",  // 33 😤
    "\xf0\x9f\x98\xa0",  // 34 😠
    "\xf0\x9f\x98\xa1",  // 35 😡
    "\xf0\x9f\xa4\xac",  // 36 🤬
    "\xf0\x9f\xa4\xaf",  // 37 🤯
    "\xf0\x9f\x98\xb3",  // 38 😳
    "\xf0\x9f\xa5\xb5",  // 39 🥵
    "\xf0\x9f\xa5\xb6",  // 3a 🥶
    "\xf0\x9f\x98\xb1",  // 3b 😱
    "\xf0\x9f\x98\xa8",  // 3c 😨
    "\xf0\x9f\x98\xb0",  // 3d 😰
    "\xf0\x9f\x98\xa5",  // 3e 😥
    "\xf0\x9f\x98\x93",  // 3f 😓
    "\xf0\x9f\xa4\x97",  // 40 🤗
    "\xf0\x9f\xa4\x94",  // 41 🤔
    "\xf0\x9f\xa4\xad",  // 42 🤭
    "\xf0\x9f\xa4\xab",  // 43 🤫
    "\xf0\x9f\xa4\xa5",  // 44 🤥
    "\xf0\x9f\x98\xb6",  // 45 😶
    "\xf0\x9f\x91\x8d",  // 46 👍
    "\xf0\x9f\x91\x8e",  // 47 👎
    "\xf0\x9f\x91\x8a",  // 48 👊
    "\xe2\x9c\x8a",  // 49 ✊
    "\xf0\x9f\xa4\x9b",  // 4a 🤛
    "\xf0\x9f\xa4\x9c",  // 4b 🤜
    "\xf0\x9f\xa4\x9e",  // 4c 🤞
    "\xe2\x9c\x8c\xef\xb8\x8f",  // 4d ✌️
    "\xf0\x9f\xa4\x9f",  // 4e 🤟
    "\xf0\x9f\xa4\x98",  // 4f 🤘
    "\xf0\x9f\x91\x8c",  // 50 👌
    "\xf0\x9f\xa4\x8c",  // 51 🤌
    "\xf0\x9f\xa4\x8f",  // 52 🤏
    "\xf0\x9f\x91\x88",  // 53 👈
    "\xf0\x9f\x91\x89",  // 54 👉
    "\xf0\x9f\x91\x86",  // 55 👆
    "\xf0\x9f\x91\x87",  // 56 👇
    "\xe2\x98\x9d\xef\xb8\x8f",  // 57 ☝️
    "\xf0\x9f\x91\x8b",  // 58 👋
    "\xf0\x9f\xa4\x9a",  // 59 🤚
    "\xf0\x9f\x96\x90\xef\xb8\x8f",  // 5a 🖐️
    "\xe2\x9c\x8b",  // 5b ✋
    "\xf0\x9f\x96\x96",  // 5c 🖖
    "\xf0\x9f\x91\x8f",  // 5d 👏
    "\xf0\x9f\x99\x8c",  // 5e 🙌
    "\xf0\x9f\x91\x90",  // 5f 👐
    "\xf0\x9f\xa4\xb2",  // 60 🤲
    "\xf0\x9f\xa4\x9d",  // 61 🤝
    "\xf0\x9f\x99\x8f",  // 62 🙏
    "\xe2\x9c\x8d\xef\xb8\x8f",  // 63 ✍️
    "\xf0\x9f\x92\x85",  // 64 💅
    "\xf0\x9f\xa4\xb3",  // 65 🤳
    "\xf0\x9f\x92\xaa",  // 66 💪
    "\xe2\x9d\xa4\xef\xb8\x8f",  // 67 ❤️
    "\xf0\x9f\xa7\xa1",  // 68 🧡
    "\xf0\x9f\x92\x9b",  // 69 💛
    "\xf0\x9f\x92\x9a",  // 6a 💚
    "\xf0\x9f\x92\x99",  // 6b 💙
    "\xf0\x9f\x92\x9c",  // 6c 💜
    "\xf0\x9f\x96\xa4",  // 6d 🖤
    "\xf0\x9f\xa4\x8d",  // 6e 🤍
    "\xf0\x9f\xa4\x8e",  // 6f 🤎
    "\xf0\x9f\x92\x94",  // 70 💔
    "\xe2\x9d\xa4\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\x94\xa5",  // 71 ❤️‍🔥
    "\xe2\x9d\xa4\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\xa9\xb9",  // 72 ❤️‍🩹
    "\xf0\x9f\x92\x95",  // 73 💕
    "\xf0\x9f\x92\x9e",  // 74 💞
    "\xf0\x9f\x92\x93",  // 75 💓
    "\xf0\x9f\x92\x97",  // 76 💗
    "\xf0\x9f\x92\x96",  // 77 💖
    "\xf0\x9f\x92\x98",  // 78 💘
    "\xf0\x9f\x92\x9d",  // 79 💝
    "\xf0\x9f\x92\x9f",  // 7a 💟
    "\xf0\x9f\x92\x8c",  // 7b 💌
    "\xf0\x9f\x92\xa2",  // 7c 💢
    "\xf0\x9f\x92\xa5",  // 7d 💥
    "\xf0\x9f\x92\xab",  // 7e 💫
    "\xf0\x9f\x92\xa6",  // 7f 💦
    "\xf0\x9f\x92\xa8",  // 80 💨
    "\xf0\x9f\x95\xb3\xef\xb8\x8f",  // 81 🕳️
    "\xf0\x9f\x92\xac",  // 82 💬
    "\xf0\x9f\x91\x81\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\x97\xa8\xef\xb8\x8f",  // 83 👁️‍🗨️
    "\xf0\x9f\x97\xa8\xef\xb8\x8f",  // 84 🗨️
    "\xf0\x9f\x97\xaf\xef\xb8\x8f",  // 85 🗯️
    "\xf0\x9f\x92\xad",  // 86 💭
    "\xf0\x9f\x8e\x89",  // 87 🎉
    "\xf0\x9f\x8e\x8a",  // 88 🎊
    "\xf0\x9f\x8e\x88",  // 89 🎈
    "\xf0\x9f\x8e\x81",  // 8a 🎁
    "\xf0\x9f\x8e\x80",  // 8b 🎀
    "\xf0\x9f\xaa\x85",  // 8c 🪅
    "\xf0\x9f\xaa\x86",  // 8d 🪆
    "\xf0\x9f\x8f\x86",  // 8e 🏆
    "\xf0\x9f\xa5\x87",  // 8f 🥇
    "\xf0\x9f\xa5\x88",  // 90 🥈
    "\xf0\x9f\xa5\x89",  // 91 🥉
    "\xe2\x9a\xbd",  // 92 ⚽
    "\xe2\x9a\xbe",  // 93 ⚾
    "\xf0\x9f\xa5\x8e",  // 94 🥎
    "\xf0\x9f\x8f\x80",  // 95 🏀
    "\xf0\x9f\x8f\x90",  // 96 🏐
    "\xf0\x9f\x8f\x88",  // 97 🏈
    "\xf0\x9f\x8f\x89",  // 98 🏉
    "\xf0\x9f\x8e\xbe",  // 99 🎾
    "\xf0\x9f\xa5\x8f",  // 9a 🥏
    "\xf0\x9f\x8e\xb3",  // 9b 🎳
    "\xf0\x9f\x8f\x8f",  // 9c 🏏
    "\xf0\x9f\x8f\x91",  // 9d 🏑
    "\xf0\x9f\x8f\x92",  // 9e 🏒
    "\xf0\x9f\xa5\x8d",  // 9f 🥍
    "\xf0\x9f\x8f\x93",  // a0 🏓
    "\xf0\x9f\x8f\xb8",  // a1 🏸
    "\xf0\x9f\xa5\x8a",  // a2 🥊
    "\xf0\x9f\xa5\x8b",  // a3 🥋
    "\xf0\x9f\xa5\x85",  // a4 🥅
    "\xe2\x9b\xb3",  // a5 ⛳
    "\xf0\x9f\x94\xa5",  // a6 🔥
    "\xe2\xad\x90",  // a7 ⭐
    "\xf0\x9f\x8c\x9f",  // a8 🌟
    "\xe2\x9c\xa8",  // a9 ✨
    "\xe2\x9a\xa1",  // aa ⚡
    "\xf0\x9f\x92\xa1",  // ab 💡
    "\xf0\x9f\x94\xa6",  // ac 🔦
    "\xf0\x9f\x8f\xae",  // ad 🏮
    "\xf0\x9f\xaa\x94",  // ae 🪔
    "\xf0\x9f\x93\xb1",  // af 📱
    "\xf0\x9f\x92\xbb",  // b0 💻
    "\xe2\x8c\x9a",  // b1 ⌚
    "\xf0\x9f\x93\xb7",  // b2 📷
    "\xf0\x9f\x93\xba",  // b3 📺
    "\xf0\x9f\x93\xbb",  // b4 📻
    "\xf0\x9f\x8e\xb5",  // b5 🎵
    "\xf0\x9f\x8e\xb6",  // b6 🎶
    "\xf0\x9f\x9a\x80",  // b7 🚀
};
constexpr int kEmojiCount = (int)(sizeof(kEmojis) / sizeof(kEmojis[0]));
static_assert(kEmojiCount <= 256, "II is two hex digits");
// The six offered first (EmojiPicker.quickEmojis).
constexpr int kQuickCount = 6;

inline const char* emojiAt(int i) { return i >= 0 && i < kEmojiCount ? kEmojis[i] : nullptr; }

// The emoji's index on the wire (its first place), or -1 when it isn't one of
// the app's. `len` < 0: up to the NUL.
inline int emojiIndex(const char* s, int len = -1) {
  if (!s) return -1;
  const size_t n = len < 0 ? strlen(s) : (size_t)len;
  if (!n) return -1;
  for (int i = 0; i < kEmojiCount; i++)
    if (strlen(kEmojis[i]) == n && memcmp(kEmojis[i], s, n) == 0) return i;
  return -1;
}

// ---- the app's format -----------------------------------------------------------------------

// The app's hash of a message: its send time, its sender (nullptr in a 1:1
// chat) and its text.
inline uint16_t hashOf(uint32_t sent_epoch, const char* sender, const char* text) {
  return quickreply::reportHash(sent_epoch, sender, text);
}

// A channel reply's text without its "@[name] " (ChannelMessage.parseReplyMention:
// ^@\[([^\]]+)\]\s+(.+)$): the app keeps a reply's text that way, so hashes it so.
// Unchanged when it isn't one.
inline const char* replyBody(const char* text) {
  if (!text || text[0] != '@' || text[1] != '[' || text[2] == ']' || !text[2]) return text;
  const char* close = strchr(text + 2, ']');
  if (!close) return text;
  const char* p = close + 1;
  if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\f' && *p != '\v') return text;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
  return *p ? p : text;
}

// The app's hash of a message as it holds it: a channel message's text without
// a reply's mention.
inline uint16_t hashOfMessage(uint32_t sent_epoch, const char* sender, const char* text, bool channel) {
  return hashOf(sent_epoch, sender, channel ? replyBody(text) : text);
}

// "r:3fa1:00". `out` holds at least 10 bytes.
inline void format(uint16_t hash, int emoji_index, char* out, size_t cap) {
  snprintf(out, cap, "r:%04x:%02x", (unsigned)hash, (unsigned)(emoji_index & 0xFF));
}

namespace detail {
inline int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;   // the app's pattern is lowercase only
}
}  // namespace detail

// Parses the app's "r:HASH:II", exactly (nothing before or after, lowercase hex,
// an index that names an emoji), as ReactionHelper.parseReactionOurs.
inline bool parse(const char* t, uint16_t* hash, int* emoji_index) {
  if (!t || strlen(t) != 9 || t[0] != 'r' || t[1] != ':' || t[6] != ':') return false;
  uint32_t h = 0;
  for (int i = 2; i < 6; i++) {
    const int v = detail::hexv(t[i]);
    if (v < 0) return false;
    h = (h << 4) | (uint32_t)v;
  }
  const int a = detail::hexv(t[7]), b = detail::hexv(t[8]);
  if (a < 0 || b < 0) return false;
  const int idx = a * 16 + b;
  if (idx >= kEmojiCount) return false;
  if (hash) *hash = (uint16_t)h;
  if (emoji_index) *emoji_index = idx;
  return true;
}

// A stored reaction: what the timeline hides and gathers under its message.
inline bool isReaction(const char* t) { return parse(t, nullptr, nullptr); }

// ---- MeshCore One's format ------------------------------------------------------------------

namespace detail {

// SHA-256 (FIPS 180-4), just enough for MeshCore One's 5-byte hash.
struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf[64];
  size_t blen = 0;
  uint64_t total = 0;
  static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
      const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void add(const uint8_t* p, size_t n) {
    total += n;
    while (n--) {
      buf[blen++] = *p++;
      if (blen == 64) { block(buf); blen = 0; }
    }
  }
  void finish(uint8_t out[32]) {
    const uint64_t bits = total * 8;
    const uint8_t one = 0x80, zero = 0;
    add(&one, 1);
    while (blen != 56) add(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    add(len, 8);
    for (int i = 0; i < 8; i++) {
      out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16);
      out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i];
    }
  }
};

// UTF-8 -> code point; returns the bytes it took (0 at the end).
inline int nextCp(const char* s, uint32_t* cp) {
  const uint8_t* p = (const uint8_t*)s;
  if (!*p) return 0;
  int k;
  uint32_t c;
  if (*p < 0x80) { c = *p; k = 1; }
  else if ((*p & 0xE0) == 0xC0) { c = *p & 0x1F; k = 2; }
  else if ((*p & 0xF0) == 0xE0) { c = *p & 0x0F; k = 3; }
  else { c = *p & 0x07; k = 4; }
  for (int i = 1; i < k; i++) {
    if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return i; }
    c = (c << 6) | (p[i] & 0x3F);
  }
  *cp = c;
  return k;
}

inline bool isHashChar(char c) {
  // [a-tv-zA-TV-Z0-9]: Crockford base32 with no 'u' (MeshCore One sends no checksum)
  if (c >= '0' && c <= '9') return true;
  const char l = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  return l >= 'a' && l <= 'z' && l != 'u';
}

}  // namespace detail

// ReactionHelper._looksLikeEmoji: one grapheme (here: a base, then only what
// joins onto it -- variation selectors, a keycap, skin tones, tag characters,
// ZWJ + the next, or a second regional indicator), starting at U+2000 or above
// or carrying FE0F / 20E3. `len` bytes of `s`.
inline bool looksLikeEmoji(const char* s, size_t len) {
  if (!s || !len || len > 63) return false;
  char tmp[64];
  memcpy(tmp, s, len);
  tmp[len] = '\0';
  uint32_t cp;
  int k = detail::nextCp(tmp, &cp);
  if (!k) return false;
  const uint32_t first = cp;
  bool selector = false;
  const bool ri = first >= 0x1F1E6 && first <= 0x1F1FF;
  int ri_pairs = 0;
  size_t at = (size_t)k;
  bool joined = false;
  while (at < len) {
    k = detail::nextCp(tmp + at, &cp);
    if (!k) break;
    at += (size_t)k;
    if (joined) { joined = false; continue; }   // anything after a ZWJ
    if (cp == 0xFE0F || cp == 0xFE0E || cp == 0x20E3) { selector |= cp != 0xFE0E; continue; }
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) continue;   // skin tone
    if (cp >= 0xE0020 && cp <= 0xE007F) continue;   // tags (subdivision flags)
    if (cp == 0x200D) { joined = true; continue; }
    if (ri && cp >= 0x1F1E6 && cp <= 0x1F1FF && ri_pairs == 0) { ri_pairs++; continue; }
    return false;   // a second character: not one grapheme
  }
  if (joined) return false;   // ends on a ZWJ
  return first >= 0x2000 || selector;
}

struct Mc1 {
  char emoji[64];
  char name[48];     // empty: no @[name] (a DM)
  bool has_name;
  char hash[9];      // lowercase
};

namespace detail {
// "...\nhhhhhhhh": the part before the newline (no newline in it, as Dart's '.').
inline bool splitTail(const char* t, size_t* head_len, char hash[9]) {
  const size_t n = strlen(t);
  if (n < 10 || t[n - 9] != '\n') return false;
  for (size_t i = 0; i < n - 9; i++) if (t[i] == '\n') return false;
  for (int i = 0; i < 8; i++) {
    const char c = t[n - 8 + i];
    if (!isHashChar(c)) return false;
    hash[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  hash[8] = '\0';
  *head_len = n - 9;
  return true;
}
inline bool take(Mc1* out, const char* emoji, size_t elen, const char* name, size_t nlen, bool has_name) {
  if (!looksLikeEmoji(emoji, elen) || elen >= sizeof out->emoji || nlen >= sizeof out->name) return false;
  memcpy(out->emoji, emoji, elen);
  out->emoji[elen] = '\0';
  memcpy(out->name, name, nlen);
  out->name[nlen] = '\0';
  out->has_name = has_name;
  return true;
}
}  // namespace detail

// ReactionHelper.parseReactionMC1, then parseReactionMC1Legacy.
inline bool parseMc1(const char* t, Mc1* out) {
  if (!t || !out) return false;
  size_t hl;
  if (!detail::splitTail(t, &hl, out->hash)) return false;
  // ^(?:@\[(.*)])?(.+?)\n : the name runs to the LAST ']' with something after it.
  if (hl > 2 && t[0] == '@' && t[1] == '[') {
    for (size_t j = hl - 1; j >= 2; j--) {
      if (t[j] == ']' && j + 1 < hl) {
        if (detail::take(out, t + j + 1, hl - j - 1, t + 2, j - 2, true)) return true;
        break;   // a greedy (.*) only backs off to shorter names, which leave a longer, non-emoji tail
      }
      if (j == 2) break;
    }
  }
  if (detail::take(out, t, hl, "", 0, false)) return true;
  // Legacy ^(.+?)(?:@\[(.*)])?\n : the emoji is the shortest prefix before "@[...]".
  if (hl > 3 && t[hl - 1] == ']') {
    for (size_t i = 1; i + 2 < hl; i++) {
      if (t[i] == '@' && t[i + 1] == '[')
        return detail::take(out, t, i, t + i + 2, hl - 1 - (i + 2), true);
    }
  }
  return false;
}

// MeshCore One's hash of a message: its text and send time ("tyvbkb33").
inline void hashMc1(uint32_t sent_epoch, const char* text, char out[9]) {
  detail::Sha256 s;
  if (text) s.add((const uint8_t*)text, strlen(text));
  const uint8_t ts[4] = {(uint8_t)sent_epoch, (uint8_t)(sent_epoch >> 8), (uint8_t)(sent_epoch >> 16),
                         (uint8_t)(sent_epoch >> 24)};
  s.add(ts, 4);
  uint8_t d[32];
  s.finish(d);
  static const char kAlpha[] = "0123456789abcdefghjkmnpqrstvwxyz";
  const uint64_t v = (uint64_t)d[0] << 32 | (uint64_t)d[1] << 24 | (uint64_t)d[2] << 16 | (uint64_t)d[3] << 8 | d[4];
  for (int i = 0; i < 8; i++) out[i] = kAlpha[(v >> (35 - 5 * i)) & 31];
  out[8] = '\0';
}

// ---- tallies --------------------------------------------------------------------------------

// The reactions on one message: each emoji once per person, in the order first
// seen, as the app's reactions map.
struct Tally {
  static constexpr int kMax = 8;
  uint8_t emoji[kMax];
  uint8_t count[kMax];
  uint32_t who[kMax][4];   // up to 4 people per emoji, by name hash (one each)
  int n = 0;

  // False when this person already reacted with it (or it's full).
  bool add(int emoji_index, const char* person) {
    quickreply::DartHash d;
    d.utf8(person ? person : "");
    const uint32_t p = d.done();
    for (int i = 0; i < n; i++) {
      if (emoji[i] != (uint8_t)emoji_index) continue;
      const int seen = count[i] < 4 ? count[i] : 4;
      for (int j = 0; j < seen; j++) if (who[i][j] == p) return false;
      if (count[i] < 4) who[i][count[i]] = p;
      if (count[i] < 255) count[i]++;
      return true;
    }
    if (n >= kMax) return false;
    emoji[n] = (uint8_t)emoji_index;
    count[n] = 1;
    who[n][0] = p;
    n++;
    return true;
  }

  // "👍 2  ❤️" (a count only past one), into `out`.
  void text(char* out, size_t cap) const {
    size_t at = 0;
    if (cap) out[0] = '\0';
    for (int i = 0; i < n && at + 1 < cap; i++) {
      const int w = count[i] > 1
          ? snprintf(out + at, cap - at, "%s%s %u", i ? "  " : "", kEmojis[emoji[i]], (unsigned)count[i])
          : snprintf(out + at, cap - at, "%s%s", i ? "  " : "", kEmojis[emoji[i]]);
      if (w < 0 || (size_t)w >= cap - at) { out[at] = '\0'; break; }
      at += (size_t)w;
    }
  }
};

}  // namespace reactions
