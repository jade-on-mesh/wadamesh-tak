// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pictures linked in a message: someone posts "https://.../cat.jpg" in a
// channel or a DM, and with Settings > General > Linked images on, the radio
// downloads it over Wi-Fi and shows it in the bubble.
//
// This file is the part with no radio in it, so it runs (and is tested) on a
// computer: finding the link in a message, naming it, and decoding the file
// (JPEG, PNG, GIF, BMP with stb_image; WebP with simplewebp) into RGB565
// pixels the size the screen wants. The downloading and the bubble are in
// ui-touch/LinkImages.inc. Tested by test/test_link_image.cpp.

#include <stddef.h>
#include <stdint.h>

namespace linkimg {

enum Kind : uint8_t { kUnknown = 0, kJpeg, kPng, kGif, kWebp, kBmp };

// The first http(s) link in `text` whose path ends in a picture's extension
// (.jpg .jpeg .png .gif .webp .bmp, any case; a ?query or #part may follow).
// False when there's none, or it doesn't fit in `cap`.
bool findUrl(const char* text, char* out, size_t cap);

// A link's id (FNV-1a of its text, never 0): its thumbnail and file are kept
// under it.
uint32_t urlId(const char* url);

// What the bytes are, from their first few (not from the link's extension:
// servers send WebP for .jpg links and the like).
Kind sniff(const uint8_t* data, size_t len);
const char* kindName(Kind k);

// The picture's size from its header. False if it isn't one this can decode.
bool size(const uint8_t* data, size_t len, int* w, int* h);

// Working memory a decode of this picture takes at most, in bytes (0 when it
// can't be decoded). A decode is refused above the budget it is given.
size_t workBytes(const uint8_t* data, size_t len);

enum Result : uint8_t { kOk = 0, kNotPicture, kTooLarge, kBroken, kNoMemory };
const char* resultText(Result r);

// Decodes the picture (a GIF's first frame) and scales it down to fit
// `max_side` pixels a side, never up. Transparent parts go over `bg`
// (RGB565). The pixels are RGB565, w*h, in memory from `out_alloc` (the
// caller frees it with the matching free). `budget`: bytes the decode may
// use for its working copy.
Result decode(const uint8_t* data, size_t len, int max_side, size_t budget, uint16_t bg,
              void* (*out_alloc)(size_t), uint16_t** out, int* out_w, int* out_h);

}  // namespace linkimg
